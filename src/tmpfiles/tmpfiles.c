/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <getopt.h>
#include <glob.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#include "sd-path.h"

#include "acl-util.h"
#include "alloc-util.h"
#include "btrfs-util.h"
#include "capability-util.h"
#include "chattr-util.h"
#include "conf-files.h"
#include "copy.h"
#include "def.h"
#include "dirent-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "glob-util.h"
#include "io-util.h"
#include "label.h"
#include "log.h"
#include "macro.h"
#include "missing.h"
#include "mkdir.h"
#include "mount-util.h"
#include "pager.h"
#include "parse-util.h"
#include "path-lookup.h"
#include "path-util.h"
#include "rm-rf.h"
#include "selinux-util.h"
#include "set.h"
#include "specifier.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "umask-util.h"
#include "user-util.h"
#include "util.h"

/* This reads all files listed in /etc/tmpfiles.d/?*.conf and creates
 * them in the file system. This is intended to be used to create
 * properly owned directories beneath /tmp, /var/tmp, /run, which are
 * volatile and hence need to be recreated on bootup. */

typedef enum ItemType {
        /* These ones take file names */
        CREATE_FILE = 'f',
        TRUNCATE_FILE = 'F',
        CREATE_DIRECTORY = 'd',
        TRUNCATE_DIRECTORY = 'D',
        CREATE_SUBVOLUME = 'v',
        CREATE_SUBVOLUME_INHERIT_QUOTA = 'q',
        CREATE_SUBVOLUME_NEW_QUOTA = 'Q',
        CREATE_FIFO = 'p',
        CREATE_SYMLINK = 'L',
        CREATE_CHAR_DEVICE = 'c',
        CREATE_BLOCK_DEVICE = 'b',
        COPY_FILES = 'C',

        /* These ones take globs */
        WRITE_FILE = 'w',
        EMPTY_DIRECTORY = 'e',
        SET_XATTR = 't',
        RECURSIVE_SET_XATTR = 'T',
        SET_ACL = 'a',
        RECURSIVE_SET_ACL = 'A',
        SET_ATTRIBUTE = 'h',
        RECURSIVE_SET_ATTRIBUTE = 'H',
        IGNORE_PATH = 'x',
        IGNORE_DIRECTORY_PATH = 'X',
        REMOVE_PATH = 'r',
        RECURSIVE_REMOVE_PATH = 'R',
        RELABEL_PATH = 'z',
        RECURSIVE_RELABEL_PATH = 'Z',
        ADJUST_MODE = 'm', /* legacy, 'z' is identical to this */
} ItemType;

typedef struct Item {
        ItemType type;

        char *path;
        char *argument;
        char **xattrs;
#if HAVE_ACL
        acl_t acl_access;
        acl_t acl_default;
#endif
        uid_t uid;
        gid_t gid;
        mode_t mode;
        usec_t age;

        dev_t major_minor;
        unsigned attribute_value;
        unsigned attribute_mask;

        bool uid_set:1;
        bool gid_set:1;
        bool mode_set:1;
        bool age_set:1;
        bool mask_perms:1;
        bool attribute_set:1;

        bool keep_first_level:1;

        bool force:1;

        bool done:1;
} Item;

typedef struct ItemArray {
        Item *items;
        size_t count;
        size_t size;
} ItemArray;

typedef enum DirectoryType {
        DIRECTORY_RUNTIME = 0,
        DIRECTORY_STATE,
        DIRECTORY_CACHE,
        DIRECTORY_LOGS,
        _DIRECTORY_TYPE_MAX,
} DirectoryType;

static bool arg_cat_config = false;
static bool arg_user = false;
static bool arg_create = false;
static bool arg_clean = false;
static bool arg_remove = false;
static bool arg_boot = false;
static bool arg_no_pager = false;

static char **arg_include_prefixes = NULL;
static char **arg_exclude_prefixes = NULL;
static char *arg_root = NULL;
static char *arg_replace = NULL;

#define MAX_DEPTH 256

static OrderedHashmap *items = NULL, *globs = NULL;
static Set *unix_sockets = NULL;

static int specifier_machine_id_safe(char specifier, void *data, void *userdata, char **ret);
static int specifier_directory(char specifier, void *data, void *userdata, char **ret);

static const Specifier specifier_table[] = {
        { 'm', specifier_machine_id_safe, NULL },
        { 'b', specifier_boot_id,         NULL },
        { 'H', specifier_host_name,       NULL },
        { 'v', specifier_kernel_release,  NULL },

        { 'U', specifier_user_id,         NULL },
        { 'u', specifier_user_name,       NULL },
        { 'h', specifier_user_home,       NULL },

        { 't', specifier_directory,       UINT_TO_PTR(DIRECTORY_RUNTIME) },
        { 'S', specifier_directory,       UINT_TO_PTR(DIRECTORY_STATE) },
        { 'C', specifier_directory,       UINT_TO_PTR(DIRECTORY_CACHE) },
        { 'L', specifier_directory,       UINT_TO_PTR(DIRECTORY_LOGS) },
        { 'T', specifier_tmp_dir,         NULL },
        { 'V', specifier_var_tmp_dir,     NULL },
        {}
};

static int specifier_machine_id_safe(char specifier, void *data, void *userdata, char **ret) {
        int r;

        /* If /etc/machine_id is missing or empty (e.g. in a chroot environment)
         * return a recognizable error so that the caller can skip the rule
         * gracefully. */

        r = specifier_machine_id(specifier, data, userdata, ret);
        if (IN_SET(r, -ENOENT, -ENOMEDIUM))
                return -ENXIO;

        return r;
}

static int specifier_directory(char specifier, void *data, void *userdata, char **ret) {
        struct table_entry {
                uint64_t type;
                const char *suffix;
        };

        static const struct table_entry paths_system[] = {
                [DIRECTORY_RUNTIME] = { SD_PATH_SYSTEM_RUNTIME            },
                [DIRECTORY_STATE] =   { SD_PATH_SYSTEM_STATE_PRIVATE      },
                [DIRECTORY_CACHE] =   { SD_PATH_SYSTEM_STATE_CACHE        },
                [DIRECTORY_LOGS] =    { SD_PATH_SYSTEM_STATE_LOGS         },
        };

        static const struct table_entry paths_user[] = {
                [DIRECTORY_RUNTIME] = { SD_PATH_USER_RUNTIME              },
                [DIRECTORY_STATE] =   { SD_PATH_USER_CONFIGURATION        },
                [DIRECTORY_CACHE] =   { SD_PATH_USER_STATE_CACHE          },
                [DIRECTORY_LOGS] =    { SD_PATH_USER_CONFIGURATION, "log" },
        };

        unsigned i;
        const struct table_entry *paths;

        assert_cc(ELEMENTSOF(paths_system) == ELEMENTSOF(paths_user));
        paths = arg_user ? paths_user : paths_system;

        i = PTR_TO_UINT(data);
        assert(i < ELEMENTSOF(paths_system));

        return sd_path_home(paths[i].type, paths[i].suffix, ret);
}

static int log_unresolvable_specifier(const char *filename, unsigned line) {
        static bool notified = false;

        /* In system mode, this is called when /etc is not fully initialized (e.g.
         * in a chroot environment) where some specifiers are unresolvable. In user
         * mode, this is called when some variables are not defined. These cases are
         * not considered as an error so log at LOG_NOTICE only for the first time
         * and then downgrade this to LOG_DEBUG for the rest. */

        log_full(notified ? LOG_DEBUG : LOG_NOTICE,
                 "[%s:%u] Failed to resolve specifier: %s, skipping",
                 filename, line,
                 arg_user ? "Required $XDG_... variable not defined" : "uninitialized /etc detected");

        if (!notified)
                log_notice("All rules containing unresolvable specifiers will be skipped.");

        notified = true;
        return 0;
}

static int user_config_paths(char*** ret) {
        _cleanup_strv_free_ char **config_dirs = NULL, **data_dirs = NULL;
        _cleanup_free_ char *persistent_config = NULL, *runtime_config = NULL, *data_home = NULL;
        _cleanup_strv_free_ char **res = NULL;
        int r;

        r = xdg_user_dirs(&config_dirs, &data_dirs);
        if (r < 0)
                return r;

        r = xdg_user_config_dir(&persistent_config, "/user-tmpfiles.d");
        if (r < 0 && r != -ENXIO)
                return r;

        r = xdg_user_runtime_dir(&runtime_config, "/user-tmpfiles.d");
        if (r < 0 && r != -ENXIO)
                return r;

        r = xdg_user_data_dir(&data_home, "/user-tmpfiles.d");
        if (r < 0 && r != -ENXIO)
                return r;

        r = strv_extend_strv_concat(&res, config_dirs, "/user-tmpfiles.d");
        if (r < 0)
                return r;

        r = strv_extend(&res, persistent_config);
        if (r < 0)
                return r;

        r = strv_extend(&res, runtime_config);
        if (r < 0)
                return r;

        r = strv_extend(&res, data_home);
        if (r < 0)
                return r;

        r = strv_extend_strv_concat(&res, data_dirs, "/user-tmpfiles.d");
        if (r < 0)
                return r;

        r = path_strv_make_absolute_cwd(res);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(res);
        return 0;
}

static bool needs_glob(ItemType t) {
        return IN_SET(t,
                      WRITE_FILE,
                      IGNORE_PATH,
                      IGNORE_DIRECTORY_PATH,
                      REMOVE_PATH,
                      RECURSIVE_REMOVE_PATH,
                      EMPTY_DIRECTORY,
                      ADJUST_MODE,
                      RELABEL_PATH,
                      RECURSIVE_RELABEL_PATH,
                      SET_XATTR,
                      RECURSIVE_SET_XATTR,
                      SET_ACL,
                      RECURSIVE_SET_ACL,
                      SET_ATTRIBUTE,
                      RECURSIVE_SET_ATTRIBUTE);
}

static bool takes_ownership(ItemType t) {
        return IN_SET(t,
                      CREATE_FILE,
                      TRUNCATE_FILE,
                      CREATE_DIRECTORY,
                      EMPTY_DIRECTORY,
                      TRUNCATE_DIRECTORY,
                      CREATE_SUBVOLUME,
                      CREATE_SUBVOLUME_INHERIT_QUOTA,
                      CREATE_SUBVOLUME_NEW_QUOTA,
                      CREATE_FIFO,
                      CREATE_SYMLINK,
                      CREATE_CHAR_DEVICE,
                      CREATE_BLOCK_DEVICE,
                      COPY_FILES,
                      WRITE_FILE,
                      IGNORE_PATH,
                      IGNORE_DIRECTORY_PATH,
                      REMOVE_PATH,
                      RECURSIVE_REMOVE_PATH);
}

static struct Item* find_glob(OrderedHashmap *h, const char *match) {
        ItemArray *j;
        Iterator i;

        ORDERED_HASHMAP_FOREACH(j, h, i) {
                unsigned n;

                for (n = 0; n < j->count; n++) {
                        Item *item = j->items + n;

                        if (fnmatch(item->path, match, FNM_PATHNAME|FNM_PERIOD) == 0)
                                return item;
                }
        }

        return NULL;
}

static void load_unix_sockets(void) {
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        if (unix_sockets)
                return;

        /* We maintain a cache of the sockets we found in /proc/net/unix to speed things up a little. */

        unix_sockets = set_new(&path_hash_ops);
        if (!unix_sockets) {
                log_oom();
                return;
        }

        f = fopen("/proc/net/unix", "re");
        if (!f) {
                log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_WARNING, errno,
                               "Failed to open /proc/net/unix, ignoring: %m");
                goto fail;
        }

        /* Skip header */
        r = read_line(f, LONG_LINE_MAX, NULL);
        if (r < 0) {
                log_warning_errno(r, "Failed to skip /proc/net/unix header line: %m");
                goto fail;
        }
        if (r == 0) {
                log_warning("Premature end of file reading /proc/net/unix.");
                goto fail;
        }

        for (;;) {
                _cleanup_free_ char *line = NULL;
                char *p, *s;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0) {
                        log_warning_errno(r, "Failed to read /proc/net/unix line, ignoring: %m");
                        goto fail;
                }
                if (r == 0) /* EOF */
                        break;

                p = strchr(line, ':');
                if (!p)
                        continue;

                if (strlen(p) < 37)
                        continue;

                p += 37;
                p += strspn(p, WHITESPACE);
                p += strcspn(p, WHITESPACE); /* skip one more word */
                p += strspn(p, WHITESPACE);

                if (*p != '/')
                        continue;

                s = strdup(p);
                if (!s) {
                        log_oom();
                        goto fail;
                }

                path_simplify(s, false);

                r = set_consume(unix_sockets, s);
                if (r < 0 && r != -EEXIST) {
                        log_warning_errno(r, "Failed to add AF_UNIX socket to set, ignoring: %m");
                        goto fail;
                }
        }

        return;

fail:
        unix_sockets = set_free_free(unix_sockets);
}

static bool unix_socket_alive(const char *fn) {
        assert(fn);

        load_unix_sockets();

        if (unix_sockets)
                return !!set_get(unix_sockets, (char*) fn);

        /* We don't know, so assume yes */
        return true;
}

static int dir_is_mount_point(DIR *d, const char *subdir) {

        int mount_id_parent, mount_id;
        int r_p, r;

        r_p = name_to_handle_at_loop(dirfd(d), ".", NULL, &mount_id_parent, 0);
        if (r_p < 0)
                r_p = -errno;

        r = name_to_handle_at_loop(dirfd(d), subdir, NULL, &mount_id, 0);
        if (r < 0)
                r = -errno;

        /* got no handle; make no assumptions, return error */
        if (r_p < 0 && r < 0)
                return r_p;

        /* got both handles; if they differ, it is a mount point */
        if (r_p >= 0 && r >= 0)
                return mount_id_parent != mount_id;

        /* got only one handle; assume different mount points if one
         * of both queries was not supported by the filesystem */
        if (IN_SET(r_p, -ENOSYS, -EOPNOTSUPP) || IN_SET(r, -ENOSYS, -EOPNOTSUPP))
                return true;

        /* return error */
        if (r_p < 0)
                return r_p;
        return r;
}

static DIR* xopendirat_nomod(int dirfd, const char *path) {
        DIR *dir;

        dir = xopendirat(dirfd, path, O_NOFOLLOW|O_NOATIME);
        if (dir)
                return dir;

        log_debug_errno(errno, "Cannot open %sdirectory \"%s\": %m", dirfd == AT_FDCWD ? "" : "sub", path);
        if (errno != EPERM)
                return NULL;

        dir = xopendirat(dirfd, path, O_NOFOLLOW);
        if (!dir)
                log_debug_errno(errno, "Cannot open %sdirectory \"%s\": %m", dirfd == AT_FDCWD ? "" : "sub", path);

        return dir;
}

static DIR* opendir_nomod(const char *path) {
        return xopendirat_nomod(AT_FDCWD, path);
}

static int dir_cleanup(
                Item *i,
                const char *p,
                DIR *d,
                const struct stat *ds,
                usec_t cutoff,
                dev_t rootdev,
                bool mountpoint,
                int maxdepth,
                bool keep_this_level) {

        struct dirent *dent;
        struct timespec times[2];
        bool deleted = false;
        int r = 0;

        FOREACH_DIRENT_ALL(dent, d, break) {
                struct stat s;
                usec_t age;
                _cleanup_free_ char *sub_path = NULL;

                if (dot_or_dot_dot(dent->d_name))
                        continue;

                if (fstatat(dirfd(d), dent->d_name, &s, AT_SYMLINK_NOFOLLOW) < 0) {
                        if (errno == ENOENT)
                                continue;

                        /* FUSE, NFS mounts, SELinux might return EACCES */
                        r = log_full_errno(errno == EACCES ? LOG_DEBUG : LOG_ERR, errno,
                                           "stat(%s/%s) failed: %m", p, dent->d_name);
                        continue;
                }

                /* Stay on the same filesystem */
                if (s.st_dev != rootdev) {
                        log_debug("Ignoring \"%s/%s\": different filesystem.", p, dent->d_name);
                        continue;
                }

                /* Try to detect bind mounts of the same filesystem instance; they
                 * do not differ in device major/minors. This type of query is not
                 * supported on all kernels or filesystem types though. */
                if (S_ISDIR(s.st_mode) && dir_is_mount_point(d, dent->d_name) > 0) {
                        log_debug("Ignoring \"%s/%s\": different mount of the same filesystem.",
                                  p, dent->d_name);
                        continue;
                }

                sub_path = strjoin(p, "/", dent->d_name);
                if (!sub_path) {
                        r = log_oom();
                        goto finish;
                }

                /* Is there an item configured for this path? */
                if (ordered_hashmap_get(items, sub_path)) {
                        log_debug("Ignoring \"%s\": a separate entry exists.", sub_path);
                        continue;
                }

                if (find_glob(globs, sub_path)) {
                        log_debug("Ignoring \"%s\": a separate glob exists.", sub_path);
                        continue;
                }

                if (S_ISDIR(s.st_mode)) {

                        if (mountpoint &&
                            streq(dent->d_name, "lost+found") &&
                            s.st_uid == 0) {
                                log_debug("Ignoring \"%s\".", sub_path);
                                continue;
                        }

                        if (maxdepth <= 0)
                                log_warning("Reached max depth on \"%s\".", sub_path);
                        else {
                                _cleanup_closedir_ DIR *sub_dir;
                                int q;

                                sub_dir = xopendirat_nomod(dirfd(d), dent->d_name);
                                if (!sub_dir) {
                                        if (errno != ENOENT)
                                                r = log_error_errno(errno, "opendir(%s) failed: %m", sub_path);

                                        continue;
                                }

                                q = dir_cleanup(i, sub_path, sub_dir, &s, cutoff, rootdev, false, maxdepth-1, false);
                                if (q < 0)
                                        r = q;
                        }

                        /* Note: if you are wondering why we don't
                         * support the sticky bit for excluding
                         * directories from cleaning like we do it for
                         * other file system objects: well, the sticky
                         * bit already has a meaning for directories,
                         * so we don't want to overload that. */

                        if (keep_this_level) {
                                log_debug("Keeping \"%s\".", sub_path);
                                continue;
                        }

                        /* Ignore ctime, we change it when deleting */
                        age = timespec_load(&s.st_mtim);
                        if (age >= cutoff) {
                                char a[FORMAT_TIMESTAMP_MAX];
                                /* Follows spelling in stat(1). */
                                log_debug("Directory \"%s\": modify time %s is too new.",
                                          sub_path,
                                          format_timestamp_us(a, sizeof(a), age));
                                continue;
                        }

                        age = timespec_load(&s.st_atim);
                        if (age >= cutoff) {
                                char a[FORMAT_TIMESTAMP_MAX];
                                log_debug("Directory \"%s\": access time %s is too new.",
                                          sub_path,
                                          format_timestamp_us(a, sizeof(a), age));
                                continue;
                        }

                        log_debug("Removing directory \"%s\".", sub_path);
                        if (unlinkat(dirfd(d), dent->d_name, AT_REMOVEDIR) < 0)
                                if (!IN_SET(errno, ENOENT, ENOTEMPTY))
                                        r = log_error_errno(errno, "rmdir(%s): %m", sub_path);

                } else {
                        /* Skip files for which the sticky bit is
                         * set. These are semantics we define, and are
                         * unknown elsewhere. See XDG_RUNTIME_DIR
                         * specification for details. */
                        if (s.st_mode & S_ISVTX) {
                                log_debug("Skipping \"%s\": sticky bit set.", sub_path);
                                continue;
                        }

                        if (mountpoint && S_ISREG(s.st_mode))
                                if (s.st_uid == 0 && STR_IN_SET(dent->d_name,
                                                                ".journal",
                                                                "aquota.user",
                                                                "aquota.group")) {
                                        log_debug("Skipping \"%s\".", sub_path);
                                        continue;
                                }

                        /* Ignore sockets that are listed in /proc/net/unix */
                        if (S_ISSOCK(s.st_mode) && unix_socket_alive(sub_path)) {
                                log_debug("Skipping \"%s\": live socket.", sub_path);
                                continue;
                        }

                        /* Ignore device nodes */
                        if (S_ISCHR(s.st_mode) || S_ISBLK(s.st_mode)) {
                                log_debug("Skipping \"%s\": a device.", sub_path);
                                continue;
                        }

                        /* Keep files on this level around if this is
                         * requested */
                        if (keep_this_level) {
                                log_debug("Keeping \"%s\".", sub_path);
                                continue;
                        }

                        age = timespec_load(&s.st_mtim);
                        if (age >= cutoff) {
                                char a[FORMAT_TIMESTAMP_MAX];
                                /* Follows spelling in stat(1). */
                                log_debug("File \"%s\": modify time %s is too new.",
                                          sub_path,
                                          format_timestamp_us(a, sizeof(a), age));
                                continue;
                        }

                        age = timespec_load(&s.st_atim);
                        if (age >= cutoff) {
                                char a[FORMAT_TIMESTAMP_MAX];
                                log_debug("File \"%s\": access time %s is too new.",
                                          sub_path,
                                          format_timestamp_us(a, sizeof(a), age));
                                continue;
                        }

                        age = timespec_load(&s.st_ctim);
                        if (age >= cutoff) {
                                char a[FORMAT_TIMESTAMP_MAX];
                                log_debug("File \"%s\": change time %s is too new.",
                                          sub_path,
                                          format_timestamp_us(a, sizeof(a), age));
                                continue;
                        }

                        log_debug("unlink \"%s\"", sub_path);

                        if (unlinkat(dirfd(d), dent->d_name, 0) < 0)
                                if (errno != ENOENT)
                                        r = log_error_errno(errno, "unlink(%s): %m", sub_path);

                        deleted = true;
                }
        }

finish:
        if (deleted) {
                usec_t age1, age2;
                char a[FORMAT_TIMESTAMP_MAX], b[FORMAT_TIMESTAMP_MAX];

                /* Restore original directory timestamps */
                times[0] = ds->st_atim;
                times[1] = ds->st_mtim;

                age1 = timespec_load(&ds->st_atim);
                age2 = timespec_load(&ds->st_mtim);
                log_debug("Restoring access and modification time on \"%s\": %s, %s",
                          p,
                          format_timestamp_us(a, sizeof(a), age1),
                          format_timestamp_us(b, sizeof(b), age2));
                if (futimens(dirfd(d), times) < 0)
                        log_error_errno(errno, "utimensat(%s): %m", p);
        }

        return r;
}

static bool dangerous_hardlinks(void) {
        _cleanup_free_ char *value = NULL;
        static int cached = -1;
        int r;

        /* Check whether the fs.protected_hardlinks sysctl is on. If we can't determine it we assume its off, as that's
         * what the upstream default is. */

        if (cached >= 0)
                return cached;

        r = read_one_line_file("/proc/sys/fs/protected_hardlinks", &value);
        if (r < 0) {
                log_debug_errno(r, "Failed to read fs.protected_hardlinks sysctl: %m");
                return true;
        }

        r = parse_boolean(value);
        if (r < 0) {
                log_debug_errno(r, "Failed to parse fs.protected_hardlinks sysctl: %m");
                return true;
        }

        cached = r == 0;
        return cached;
}

static bool hardlink_vulnerable(const struct stat *st) {
        assert(st);

        return !S_ISDIR(st->st_mode) && st->st_nlink > 1 && dangerous_hardlinks();
}

static int fd_set_perms(Item *i, int fd, const struct stat *st) {
        _cleanup_free_ char *path = NULL;
        int r;

        assert(i);
        assert(fd);

        r = fd_get_path(fd, &path);
        if (r < 0)
                return r;

        if (!i->mode_set && !i->uid_set && !i->gid_set)
                goto shortcut;

        if (hardlink_vulnerable(st)) {
                log_error("Refusing to set permissions on hardlinked file %s while the fs.protected_hardlinks sysctl is turned off.", path);
                return -EPERM;
        }

        if (i->mode_set) {
                if (S_ISLNK(st->st_mode))
                        log_debug("Skipping mode fix for symlink %s.", path);
                else {
                        mode_t m = i->mode;

                        if (i->mask_perms) {
                                if (!(st->st_mode & 0111))
                                        m &= ~0111;
                                if (!(st->st_mode & 0222))
                                        m &= ~0222;
                                if (!(st->st_mode & 0444))
                                        m &= ~0444;
                                if (!S_ISDIR(st->st_mode))
                                        m &= ~07000; /* remove sticky/sgid/suid bit, unless directory */
                        }

                        if (m == (st->st_mode & 07777))
                                log_debug("\"%s\" has correct mode %o already.", path, st->st_mode);
                        else {
                                log_debug("Changing \"%s\" to mode %o.", path, m);
                                if (fchmod_opath(fd, m) < 0)
                                        return log_error_errno(errno, "fchmod() of %s failed: %m", path);
                        }
                }
        }

        if ((i->uid_set && i->uid != st->st_uid) ||
            (i->gid_set && i->gid != st->st_gid)) {
                log_debug("Changing \"%s\" to owner "UID_FMT":"GID_FMT,
                          path,
                          i->uid_set ? i->uid : UID_INVALID,
                          i->gid_set ? i->gid : GID_INVALID);

                if (fchownat(fd,
                             "",
                             i->uid_set ? i->uid : UID_INVALID,
                             i->gid_set ? i->gid : GID_INVALID,
                             AT_EMPTY_PATH) < 0)
                        return log_error_errno(errno, "fchownat() of %s failed: %m", path);
        }

shortcut:
        return label_fix(path, 0);
}

static int path_set_perms(Item *i, const char *path) {
        _cleanup_close_ int fd = -1;
        struct stat st;

        assert(i);
        assert(path);

        fd = open(path, O_NOFOLLOW|O_CLOEXEC|O_PATH);
        if (fd < 0) {
                int level = LOG_ERR, r = -errno;

                /* Option "e" operates only on existing objects. Do not
                 * print errors about non-existent files or directories */
                if (i->type == EMPTY_DIRECTORY && errno == ENOENT) {
                        level = LOG_DEBUG;
                        r = 0;
                }

                log_full_errno(level, errno, "Adjusting owner and mode for %s failed: %m", path);
                return r;
        }

        if (fstat(fd, &st) < 0)
                return log_error_errno(errno, "Failed to fstat() file %s: %m", path);

        if (i->type == EMPTY_DIRECTORY && !S_ISDIR(st.st_mode))
                return log_error_errno(EEXIST, "'%s' already exists and is not a directory. ", path);

        return fd_set_perms(i, fd, &st);
}

static int parse_xattrs_from_arg(Item *i) {
        const char *p;
        int r;

        assert(i);
        assert(i->argument);

        p = i->argument;

        for (;;) {
                _cleanup_free_ char *name = NULL, *value = NULL, *xattr = NULL;

                r = extract_first_word(&p, &xattr, NULL, EXTRACT_QUOTES|EXTRACT_CUNESCAPE);
                if (r < 0)
                        log_warning_errno(r, "Failed to parse extended attribute '%s', ignoring: %m", p);
                if (r <= 0)
                        break;

                r = split_pair(xattr, "=", &name, &value);
                if (r < 0) {
                        log_warning_errno(r, "Failed to parse extended attribute, ignoring: %s", xattr);
                        continue;
                }

                if (isempty(name) || isempty(value)) {
                        log_warning("Malformed extended attribute found, ignoring: %s", xattr);
                        continue;
                }

                if (strv_push_pair(&i->xattrs, name, value) < 0)
                        return log_oom();

                name = value = NULL;
        }

        return 0;
}

static int fd_set_xattrs(Item *i, int fd, const struct stat *st) {
        char procfs_path[STRLEN("/proc/self/fd/") + DECIMAL_STR_MAX(int)];
        _cleanup_free_ char *path = NULL;
        char **name, **value;
        int r;

        assert(i);
        assert(fd);

        r = fd_get_path(fd, &path);
        if (r < 0)
                return r;

        xsprintf(procfs_path, "/proc/self/fd/%i", fd);

        STRV_FOREACH_PAIR(name, value, i->xattrs) {
                log_debug("Setting extended attribute '%s=%s' on %s.", *name, *value, path);
                if (setxattr(procfs_path, *name, *value, strlen(*value), 0) < 0)
                        return log_error_errno(errno, "Setting extended attribute %s=%s on %s failed: %m",
                                               *name, *value, path);
        }
        return 0;
}

static int path_set_xattrs(Item *i, const char *path) {
        _cleanup_close_ int fd = -1;

        assert(i);
        assert(path);

        fd = open(path, O_CLOEXEC|O_NOFOLLOW|O_PATH);
        if (fd < 0)
                return log_error_errno(errno, "Cannot open '%s': %m", path);

        return fd_set_xattrs(i, fd, NULL);
}

static int parse_acls_from_arg(Item *item) {
#if HAVE_ACL
        int r;

        assert(item);

        /* If force (= modify) is set, we will not modify the acl
         * afterwards, so the mask can be added now if necessary. */

        r = parse_acl(item->argument, &item->acl_access, &item->acl_default, !item->force);
        if (r < 0)
                log_warning_errno(r, "Failed to parse ACL \"%s\": %m. Ignoring", item->argument);
#else
        log_warning_errno(ENOSYS, "ACLs are not supported. Ignoring");
#endif

        return 0;
}

#if HAVE_ACL
static int path_set_acl(const char *path, const char *pretty, acl_type_t type, acl_t acl, bool modify) {
        _cleanup_(acl_free_charpp) char *t = NULL;
        _cleanup_(acl_freep) acl_t dup = NULL;
        int r;

        /* Returns 0 for success, positive error if already warned,
         * negative error otherwise. */

        if (modify) {
                r = acls_for_file(path, type, acl, &dup);
                if (r < 0)
                        return r;

                r = calc_acl_mask_if_needed(&dup);
                if (r < 0)
                        return r;
        } else {
                dup = acl_dup(acl);
                if (!dup)
                        return -errno;

                /* the mask was already added earlier if needed */
        }

        r = add_base_acls_if_needed(&dup, path);
        if (r < 0)
                return r;

        t = acl_to_any_text(dup, NULL, ',', TEXT_ABBREVIATE);
        log_debug("Setting %s ACL %s on %s.",
                  type == ACL_TYPE_ACCESS ? "access" : "default",
                  strna(t), pretty);

        r = acl_set_file(path, type, dup);
        if (r < 0)
                /* Return positive to indicate we already warned */
                return -log_error_errno(errno,
                                        "Setting %s ACL \"%s\" on %s failed: %m",
                                        type == ACL_TYPE_ACCESS ? "access" : "default",
                                        strna(t), pretty);

        return 0;
}
#endif

static int fd_set_acls(Item *item, int fd, const struct stat *st) {
        int r = 0;
#if HAVE_ACL
        char procfs_path[STRLEN("/proc/self/fd/") + DECIMAL_STR_MAX(int)];
        _cleanup_free_ char *path = NULL;

        assert(item);
        assert(fd);
        assert(st);

        r = fd_get_path(fd, &path);
        if (r < 0)
                return r;

        if (hardlink_vulnerable(st)) {
                log_error("Refusing to set ACLs on hardlinked file %s while the fs.protected_hardlinks sysctl is turned off.", path);
                return -EPERM;
        }

        if (S_ISLNK(st->st_mode)) {
                log_debug("Skipping ACL fix for symlink %s.", path);
                return 0;
        }

        xsprintf(procfs_path, "/proc/self/fd/%i", fd);

        if (item->acl_access)
                r = path_set_acl(procfs_path, path, ACL_TYPE_ACCESS, item->acl_access, item->force);

        if (r == 0 && item->acl_default)
                r = path_set_acl(procfs_path, path, ACL_TYPE_DEFAULT, item->acl_default, item->force);

        if (r > 0)
                return -r; /* already warned */
        if (r == -EOPNOTSUPP) {
                log_debug_errno(r, "ACLs not supported by file system at %s", path);
                return 0;
        }
        if (r < 0)
                return log_error_errno(r, "ACL operation on \"%s\" failed: %m", path);
#endif
        return r;
}

static int path_set_acls(Item *item, const char *path) {
        int r = 0;
#ifdef HAVE_ACL
        _cleanup_close_ int fd = -1;
        struct stat st;

        assert(item);
        assert(path);

        fd = open(path, O_NOFOLLOW|O_CLOEXEC|O_PATH);
        if (fd < 0)
                return log_error_errno(errno, "Adjusting ACL of %s failed: %m", path);

        if (fstat(fd, &st) < 0)
                return log_error_errno(errno, "Failed to fstat() file %s: %m", path);

        r = fd_set_acls(item, fd, &st);
 #endif
         return r;
 }

#define ATTRIBUTES_ALL                          \
        (FS_NOATIME_FL      |                   \
         FS_SYNC_FL         |                   \
         FS_DIRSYNC_FL      |                   \
         FS_APPEND_FL       |                   \
         FS_COMPR_FL        |                   \
         FS_NODUMP_FL       |                   \
         FS_EXTENT_FL       |                   \
         FS_IMMUTABLE_FL    |                   \
         FS_JOURNAL_DATA_FL |                   \
         FS_SECRM_FL        |                   \
         FS_UNRM_FL         |                   \
         FS_NOTAIL_FL       |                   \
         FS_TOPDIR_FL       |                   \
         FS_NOCOW_FL)

static int parse_attribute_from_arg(Item *item) {

        static const struct {
                char character;
                unsigned value;
        } attributes[] = {
                { 'A', FS_NOATIME_FL },      /* do not update atime */
                { 'S', FS_SYNC_FL },         /* Synchronous updates */
                { 'D', FS_DIRSYNC_FL },      /* dirsync behaviour (directories only) */
                { 'a', FS_APPEND_FL },       /* writes to file may only append */
                { 'c', FS_COMPR_FL },        /* Compress file */
                { 'd', FS_NODUMP_FL },       /* do not dump file */
                { 'e', FS_EXTENT_FL },       /* Extents */
                { 'i', FS_IMMUTABLE_FL },    /* Immutable file */
                { 'j', FS_JOURNAL_DATA_FL }, /* Reserved for ext3 */
                { 's', FS_SECRM_FL },        /* Secure deletion */
                { 'u', FS_UNRM_FL },         /* Undelete */
                { 't', FS_NOTAIL_FL },       /* file tail should not be merged */
                { 'T', FS_TOPDIR_FL },       /* Top of directory hierarchies */
                { 'C', FS_NOCOW_FL },        /* Do not cow file */
        };

        enum {
                MODE_ADD,
                MODE_DEL,
                MODE_SET
        } mode = MODE_ADD;

        unsigned value = 0, mask = 0;
        const char *p;

        assert(item);

        p = item->argument;
        if (p) {
                if (*p == '+') {
                        mode = MODE_ADD;
                        p++;
                } else if (*p == '-') {
                        mode = MODE_DEL;
                        p++;
                } else  if (*p == '=') {
                        mode = MODE_SET;
                        p++;
                }
        }

        if (isempty(p) && mode != MODE_SET) {
                log_error("Setting file attribute on '%s' needs an attribute specification.", item->path);
                return -EINVAL;
        }

        for (; p && *p ; p++) {
                unsigned i, v;

                for (i = 0; i < ELEMENTSOF(attributes); i++)
                        if (*p == attributes[i].character)
                                break;

                if (i >= ELEMENTSOF(attributes)) {
                        log_error("Unknown file attribute '%c' on '%s'.", *p, item->path);
                        return -EINVAL;
                }

                v = attributes[i].value;

                SET_FLAG(value, v, IN_SET(mode, MODE_ADD, MODE_SET));

                mask |= v;
        }

        if (mode == MODE_SET)
                mask |= ATTRIBUTES_ALL;

        assert(mask != 0);

        item->attribute_mask = mask;
        item->attribute_value = value;
        item->attribute_set = true;

        return 0;
}

static int fd_set_attribute(Item *item, int fd, const struct stat *st) {
        _cleanup_close_ int procfs_fd = -1;
        _cleanup_free_ char *path = NULL;
        unsigned f;
        int r;

        if (!item->attribute_set || item->attribute_mask == 0)
                return 0;

        r = fd_get_path(fd, &path);
        if (r < 0)
                return r;

        /* Issuing the file attribute ioctls on device nodes is not
         * safe, as that will be delivered to the drivers, not the
         * file system containing the device node. */
        if (!S_ISREG(st->st_mode) && !S_ISDIR(st->st_mode)) {
                log_error("Setting file flags is only supported on regular files and directories, cannot set on '%s'.", path);
                return -EINVAL;
        }

        f = item->attribute_value & item->attribute_mask;

        /* Mask away directory-specific flags */
        if (!S_ISDIR(st->st_mode))
                f &= ~FS_DIRSYNC_FL;

        procfs_fd = fd_reopen(fd, O_RDONLY|O_CLOEXEC|O_NOATIME);
        if (procfs_fd < 0)
                return -errno;

        r = chattr_fd(procfs_fd, f, item->attribute_mask);
        if (r < 0)
                log_full_errno(IN_SET(r, -ENOTTY, -EOPNOTSUPP) ? LOG_DEBUG : LOG_WARNING,
                               r,
                               "Cannot set file attribute for '%s', value=0x%08x, mask=0x%08x: %m",
                               path, item->attribute_value, item->attribute_mask);

        return 0;
}

static int path_set_attribute(Item *item, const char *path) {
        _cleanup_close_ int fd = -1;
        struct stat st;

        if (!item->attribute_set || item->attribute_mask == 0)
                return 0;

        fd = open(path, O_CLOEXEC|O_NOFOLLOW|O_PATH);
        if (fd < 0)
                return log_error_errno(errno, "Cannot open '%s': %m", path);

        if (fstat(fd, &st) < 0)
                return log_error_errno(errno, "Cannot stat '%s': %m", path);

        return fd_set_attribute(item, fd, &st);
}

static int write_one_file(Item *i, const char *path) {
        _cleanup_close_ int fd = -1;
        int flags, r = 0;
        struct stat st;

        assert(i);
        assert(path);

        flags = i->type == CREATE_FILE ? O_CREAT|O_EXCL|O_NOFOLLOW :
                i->type == TRUNCATE_FILE ? O_CREAT|O_TRUNC|O_NOFOLLOW : 0;

        RUN_WITH_UMASK(0000) {
                mac_selinux_create_file_prepare(path, S_IFREG);
                fd = open(path, flags|O_NONBLOCK|O_CLOEXEC|O_WRONLY|O_NOCTTY, i->mode);
                mac_selinux_create_file_clear();
        }

        if (fd < 0) {
                if (i->type == WRITE_FILE && errno == ENOENT) {
                        log_debug_errno(errno, "Not writing missing file \"%s\": %m", path);
                        return 0;
                }
                if (i->type == CREATE_FILE && errno == EEXIST) {
                        log_debug_errno(errno, "Not writing to pre-existing file \"%s\": %m", path);
                        goto done;
                }

                r = -errno;
                if (!i->argument && errno == EROFS && stat(path, &st) == 0 &&
                    (i->type == CREATE_FILE || st.st_size == 0))
                        goto check_mode;

                return log_error_errno(r, "Failed to create file %s: %m", path);
        }

        if (i->argument) {
                log_debug("%s to \"%s\".", i->type == CREATE_FILE ? "Appending" : "Writing", path);

                r = loop_write(fd, i->argument, strlen(i->argument), false);
                if (r < 0)
                        return log_error_errno(r, "Failed to write file \"%s\": %m", path);
        } else
                log_debug("\"%s\" has been created.", path);

        fd = safe_close(fd);

 done:
        if (stat(path, &st) < 0)
                return log_error_errno(errno, "stat(%s) failed: %m", path);

 check_mode:
        if (!S_ISREG(st.st_mode)) {
                log_error("%s is not a file.", path);
                return -EEXIST;
        }

        r = path_set_perms(i, path);
        if (r < 0)
                return r;

        return 0;
}

typedef int (*action_t)(Item *, const char *);
typedef int (*fdaction_t)(Item *, int fd, const struct stat *st);

static int item_do(Item *i, int fd, const struct stat *st, fdaction_t action) {
        int r = 0, q;

        assert(i);
        assert(fd >= 0);
        assert(st);

        /* This returns the first error we run into, but nevertheless
         * tries to go on */
        r = action(i, fd, st);

        if (S_ISDIR(st->st_mode)) {
                char procfs_path[STRLEN("/proc/self/fd/") + DECIMAL_STR_MAX(int)];
                _cleanup_closedir_ DIR *d = NULL;
                struct dirent *de;

                /* The passed 'fd' was opened with O_PATH. We need to convert
                 * it into a 'regular' fd before reading the directory content. */
                xsprintf(procfs_path, "/proc/self/fd/%i", fd);

                d = opendir(procfs_path);
                if (!d) {
                        r = r ?: -errno;
                        goto finish;
                }

                FOREACH_DIRENT_ALL(de, d, q = -errno; goto finish) {
                        struct stat de_st;
                        int de_fd;

                        if (dot_or_dot_dot(de->d_name))
                                continue;

                        de_fd = openat(fd, de->d_name, O_NOFOLLOW|O_CLOEXEC|O_PATH);
                        if (de_fd >= 0 && fstat(de_fd, &de_st) >= 0)
                                /* pass ownership of dirent fd over  */
                                q = item_do(i, de_fd, &de_st, action);
                        else
                                q = -errno;

                        if (q < 0 && r == 0)
                                r = q;
                }
        }
finish:
        safe_close(fd);
        return r;
}

static int glob_item(Item *i, action_t action) {
        _cleanup_globfree_ glob_t g = {
                .gl_opendir = (void *(*)(const char *)) opendir_nomod,
        };
        int r = 0, k;
        char **fn;

        k = safe_glob(i->path, GLOB_NOSORT|GLOB_BRACE, &g);
        if (k < 0 && k != -ENOENT)
                return log_error_errno(k, "glob(%s) failed: %m", i->path);

        STRV_FOREACH(fn, g.gl_pathv) {
                k = action(i, *fn);
                if (k < 0 && r == 0)
                        r = k;
        }

        return r;
}

static int glob_item_recursively(Item *i, fdaction_t action) {
        _cleanup_globfree_ glob_t g = {
                .gl_opendir = (void *(*)(const char *)) opendir_nomod,
        };
        int r = 0, k;
        char **fn;

        k = safe_glob(i->path, GLOB_NOSORT|GLOB_BRACE, &g);
        if (k < 0 && k != -ENOENT)
                return log_error_errno(k, "glob(%s) failed: %m", i->path);

        STRV_FOREACH(fn, g.gl_pathv) {
                _cleanup_close_ int fd = -1;
                struct stat st;

                /* Make sure we won't trigger/follow file object (such as
                 * device nodes, automounts, ...) pointed out by 'fn' with
                 * O_PATH. Note, when O_PATH is used, flags other than
                 * O_CLOEXEC, O_DIRECTORY, and O_NOFOLLOW are ignored. */

                fd = open(*fn, O_CLOEXEC|O_NOFOLLOW|O_PATH);
                if (fd < 0) {
                        r = r ?: -errno;
                        continue;
                }

                if (fstat(fd, &st) < 0) {
                        r = r ?: -errno;
                        continue;
                }

                k = item_do(i, fd, &st, action);
                if (k < 0 && r == 0)
                        r = k;

                /* we passed fd ownership to the previous call */
                fd = -1;
        }

        return r;
}

typedef enum {
        CREATION_NORMAL,
        CREATION_EXISTING,
        CREATION_FORCE,
        _CREATION_MODE_MAX,
        _CREATION_MODE_INVALID = -1
} CreationMode;

static const char *creation_mode_verb_table[_CREATION_MODE_MAX] = {
        [CREATION_NORMAL] = "Created",
        [CREATION_EXISTING] = "Found existing",
        [CREATION_FORCE] = "Created replacement",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_TO_STRING(creation_mode_verb, CreationMode);

static int create_item(Item *i) {
        struct stat st;
        int r = 0;
        int q = 0;
        CreationMode creation;

        assert(i);

        log_debug("Running create action for entry %c %s", (char) i->type, i->path);

        switch (i->type) {

        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case REMOVE_PATH:
        case RECURSIVE_REMOVE_PATH:
                return 0;

        case CREATE_FILE:
        case TRUNCATE_FILE:
                RUN_WITH_UMASK(0000)
                        (void) mkdir_parents_label(i->path, 0755);

                r = write_one_file(i, i->path);
                if (r < 0)
                        return r;
                break;

        case COPY_FILES:
                RUN_WITH_UMASK(0000)
                        (void) mkdir_parents_label(i->path, 0755);

                log_debug("Copying tree \"%s\" to \"%s\".", i->argument, i->path);
                r = copy_tree(i->argument, i->path,
                              i->uid_set ? i->uid : UID_INVALID,
                              i->gid_set ? i->gid : GID_INVALID,
                              COPY_REFLINK);

                if (r == -EROFS && stat(i->path, &st) == 0)
                        r = -EEXIST;

                if (r < 0) {
                        struct stat a, b;

                        if (r != -EEXIST)
                                return log_error_errno(r, "Failed to copy files to %s: %m", i->path);

                        if (stat(i->argument, &a) < 0)
                                return log_error_errno(errno, "stat(%s) failed: %m", i->argument);

                        if (stat(i->path, &b) < 0)
                                return log_error_errno(errno, "stat(%s) failed: %m", i->path);

                        if ((a.st_mode ^ b.st_mode) & S_IFMT) {
                                log_debug("Can't copy to %s, file exists already and is of different type", i->path);
                                return 0;
                        }
                }

                r = path_set_perms(i, i->path);
                if (r < 0)
                        return r;

                break;

        case WRITE_FILE:
                r = glob_item(i, write_one_file);
                if (r < 0)
                        return r;

                break;

        case CREATE_DIRECTORY:
        case TRUNCATE_DIRECTORY:
        case CREATE_SUBVOLUME:
        case CREATE_SUBVOLUME_INHERIT_QUOTA:
        case CREATE_SUBVOLUME_NEW_QUOTA:
                RUN_WITH_UMASK(0000)
                        (void) mkdir_parents_label(i->path, 0755);

                if (IN_SET(i->type, CREATE_SUBVOLUME, CREATE_SUBVOLUME_INHERIT_QUOTA, CREATE_SUBVOLUME_NEW_QUOTA)) {

                        if (btrfs_is_subvol(empty_to_root(arg_root)) <= 0)

                                /* Don't create a subvolume unless the
                                 * root directory is one, too. We do
                                 * this under the assumption that if
                                 * the root directory is just a plain
                                 * directory (i.e. very light-weight),
                                 * we shouldn't try to split it up
                                 * into subvolumes (i.e. more
                                 * heavy-weight). Thus, chroot()
                                 * environments and suchlike will get
                                 * a full brtfs subvolume set up below
                                 * their tree only if they
                                 * specifically set up a btrfs
                                 * subvolume for the root dir too. */

                                r = -ENOTTY;
                        else {
                                RUN_WITH_UMASK((~i->mode) & 0777)
                                        r = btrfs_subvol_make(i->path);
                        }
                } else
                        r = 0;

                if (IN_SET(i->type, CREATE_DIRECTORY, TRUNCATE_DIRECTORY) || r == -ENOTTY)
                        RUN_WITH_UMASK(0000)
                                r = mkdir_label(i->path, i->mode);

                if (r < 0) {
                        int k;

                        if (!IN_SET(r, -EEXIST, -EROFS))
                                return log_error_errno(r, "Failed to create directory or subvolume \"%s\": %m", i->path);

                        k = is_dir(i->path, false);
                        if (k == -ENOENT && r == -EROFS)
                                return log_error_errno(r, "%s does not exist and cannot be created as the file system is read-only.", i->path);
                        if (k < 0)
                                return log_error_errno(k, "Failed to check if %s exists: %m", i->path);
                        if (!k) {
                                log_warning("\"%s\" already exists and is not a directory.", i->path);
                                return 0;
                        }

                        creation = CREATION_EXISTING;
                } else
                        creation = CREATION_NORMAL;

                log_debug("%s directory \"%s\".", creation_mode_verb_to_string(creation), i->path);

                if (IN_SET(i->type, CREATE_SUBVOLUME_NEW_QUOTA, CREATE_SUBVOLUME_INHERIT_QUOTA)) {
                        r = btrfs_subvol_auto_qgroup(i->path, 0, i->type == CREATE_SUBVOLUME_NEW_QUOTA);
                        if (r == -ENOTTY)
                                log_debug_errno(r, "Couldn't adjust quota for subvolume \"%s\" (unsupported fs or dir not a subvolume): %m", i->path);
                        else if (r == -EROFS)
                                log_debug_errno(r, "Couldn't adjust quota for subvolume \"%s\" (fs is read-only).", i->path);
                        else if (r == -ENOPROTOOPT)
                                log_debug_errno(r, "Couldn't adjust quota for subvolume \"%s\" (quota support is disabled).", i->path);
                        else if (r < 0)
                                q = log_error_errno(r, "Failed to adjust quota for subvolume \"%s\": %m", i->path);
                        else if (r > 0)
                                log_debug("Adjusted quota for subvolume \"%s\".", i->path);
                        else if (r == 0)
                                log_debug("Quota for subvolume \"%s\" already in place, no change made.", i->path);
                }

                _fallthrough_;
        case EMPTY_DIRECTORY:
                r = glob_item(i, path_set_perms);
                if (q < 0)
                        return q;
                if (r < 0)
                        return r;

                break;

        case CREATE_FIFO:
                RUN_WITH_UMASK(0000) {
                        (void) mkdir_parents_label(i->path, 0755);

                        mac_selinux_create_file_prepare(i->path, S_IFIFO);
                        r = mkfifo(i->path, i->mode);
                        mac_selinux_create_file_clear();
                }

                if (r < 0) {
                        if (errno != EEXIST)
                                return log_error_errno(errno, "Failed to create fifo %s: %m", i->path);

                        if (lstat(i->path, &st) < 0)
                                return log_error_errno(errno, "stat(%s) failed: %m", i->path);

                        if (!S_ISFIFO(st.st_mode)) {

                                if (i->force) {
                                        RUN_WITH_UMASK(0000) {
                                                mac_selinux_create_file_prepare(i->path, S_IFIFO);
                                                r = mkfifo_atomic(i->path, i->mode);
                                                mac_selinux_create_file_clear();
                                        }

                                        if (r < 0)
                                                return log_error_errno(r, "Failed to create fifo %s: %m", i->path);
                                        creation = CREATION_FORCE;
                                } else {
                                        log_warning("\"%s\" already exists and is not a fifo.", i->path);
                                        return 0;
                                }
                        } else
                                creation = CREATION_EXISTING;
                } else
                        creation = CREATION_NORMAL;
                log_debug("%s fifo \"%s\".", creation_mode_verb_to_string(creation), i->path);

                r = path_set_perms(i, i->path);
                if (r < 0)
                        return r;

                break;

        case CREATE_SYMLINK: {
                RUN_WITH_UMASK(0000)
                        (void) mkdir_parents_label(i->path, 0755);

                mac_selinux_create_file_prepare(i->path, S_IFLNK);
                r = symlink(i->argument, i->path);
                mac_selinux_create_file_clear();

                if (r < 0) {
                        _cleanup_free_ char *x = NULL;

                        if (errno != EEXIST)
                                return log_error_errno(errno, "symlink(%s, %s) failed: %m", i->argument, i->path);

                        r = readlink_malloc(i->path, &x);
                        if (r < 0 || !streq(i->argument, x)) {

                                if (i->force) {
                                        mac_selinux_create_file_prepare(i->path, S_IFLNK);
                                        r = symlink_atomic(i->argument, i->path);
                                        mac_selinux_create_file_clear();

                                        if (IN_SET(r, -EISDIR, -EEXIST, -ENOTEMPTY)) {
                                                r = rm_rf(i->path, REMOVE_ROOT|REMOVE_PHYSICAL);
                                                if (r < 0)
                                                        return log_error_errno(r, "rm -fr %s failed: %m", i->path);

                                                mac_selinux_create_file_prepare(i->path, S_IFLNK);
                                                r = symlink(i->argument, i->path) < 0 ? -errno : 0;
                                                mac_selinux_create_file_clear();
                                        }
                                        if (r < 0)
                                                return log_error_errno(r, "symlink(%s, %s) failed: %m", i->argument, i->path);

                                        creation = CREATION_FORCE;
                                } else {
                                        log_debug("\"%s\" is not a symlink or does not point to the correct path.", i->path);
                                        return 0;
                                }
                        } else
                                creation = CREATION_EXISTING;
                } else

                        creation = CREATION_NORMAL;
                log_debug("%s symlink \"%s\".", creation_mode_verb_to_string(creation), i->path);
                break;
        }

        case CREATE_BLOCK_DEVICE:
        case CREATE_CHAR_DEVICE: {
                mode_t file_type;

                if (have_effective_cap(CAP_MKNOD) == 0) {
                        /* In a container we lack CAP_MKNOD. We
                        shouldn't attempt to create the device node in
                        that case to avoid noise, and we don't support
                        virtualized devices in containers anyway. */

                        log_debug("We lack CAP_MKNOD, skipping creation of device node %s.", i->path);
                        return 0;
                }

                RUN_WITH_UMASK(0000)
                        (void) mkdir_parents_label(i->path, 0755);

                file_type = i->type == CREATE_BLOCK_DEVICE ? S_IFBLK : S_IFCHR;

                RUN_WITH_UMASK(0000) {
                        mac_selinux_create_file_prepare(i->path, file_type);
                        r = mknod(i->path, i->mode | file_type, i->major_minor);
                        mac_selinux_create_file_clear();
                }

                if (r < 0) {
                        if (errno == EPERM) {
                                log_debug("We lack permissions, possibly because of cgroup configuration; "
                                          "skipping creation of device node %s.", i->path);
                                return 0;
                        }

                        if (errno != EEXIST)
                                return log_error_errno(errno, "Failed to create device node %s: %m", i->path);

                        if (lstat(i->path, &st) < 0)
                                return log_error_errno(errno, "stat(%s) failed: %m", i->path);

                        if ((st.st_mode & S_IFMT) != file_type) {

                                if (i->force) {

                                        RUN_WITH_UMASK(0000) {
                                                mac_selinux_create_file_prepare(i->path, file_type);
                                                r = mknod_atomic(i->path, i->mode | file_type, i->major_minor);
                                                mac_selinux_create_file_clear();
                                        }

                                        if (r < 0)
                                                return log_error_errno(r, "Failed to create device node \"%s\": %m", i->path);
                                        creation = CREATION_FORCE;
                                } else {
                                        log_debug("%s is not a device node.", i->path);
                                        return 0;
                                }
                        } else
                                creation = CREATION_EXISTING;
                } else
                        creation = CREATION_NORMAL;

                log_debug("%s %s device node \"%s\" %u:%u.",
                          creation_mode_verb_to_string(creation),
                          i->type == CREATE_BLOCK_DEVICE ? "block" : "char",
                          i->path, major(i->mode), minor(i->mode));

                r = path_set_perms(i, i->path);
                if (r < 0)
                        return r;

                break;
        }

        case ADJUST_MODE:
        case RELABEL_PATH:
                r = glob_item(i, path_set_perms);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_RELABEL_PATH:
                r = glob_item_recursively(i, fd_set_perms);
                if (r < 0)
                        return r;
                break;

        case SET_XATTR:
                r = glob_item(i, path_set_xattrs);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_SET_XATTR:
                r = glob_item_recursively(i, fd_set_xattrs);
                if (r < 0)
                        return r;
                break;

        case SET_ACL:
                r = glob_item(i, path_set_acls);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_SET_ACL:
                r = glob_item_recursively(i, fd_set_acls);
                if (r < 0)
                        return r;
                break;

        case SET_ATTRIBUTE:
                r = glob_item(i, path_set_attribute);
                if (r < 0)
                        return r;
                break;

        case RECURSIVE_SET_ATTRIBUTE:
                r = glob_item_recursively(i, fd_set_attribute);
                if (r < 0)
                        return r;
                break;
        }

        return 0;
}

static int remove_item_instance(Item *i, const char *instance) {
        int r;

        assert(i);

        switch (i->type) {

        case REMOVE_PATH:
                if (remove(instance) < 0 && errno != ENOENT)
                        return log_error_errno(errno, "rm(%s): %m", instance);

                break;

        case TRUNCATE_DIRECTORY:
        case RECURSIVE_REMOVE_PATH:
                /* FIXME: we probably should use dir_cleanup() here
                 * instead of rm_rf() so that 'x' is honoured. */
                log_debug("rm -rf \"%s\"", instance);
                r = rm_rf(instance, (i->type == RECURSIVE_REMOVE_PATH ? REMOVE_ROOT|REMOVE_SUBVOLUME : 0) | REMOVE_PHYSICAL);
                if (r < 0 && r != -ENOENT)
                        return log_error_errno(r, "rm_rf(%s): %m", instance);

                break;

        default:
                assert_not_reached("wut?");
        }

        return 0;
}

static int remove_item(Item *i) {
        assert(i);

        log_debug("Running remove action for entry %c %s", (char) i->type, i->path);

        switch (i->type) {

        case REMOVE_PATH:
        case TRUNCATE_DIRECTORY:
        case RECURSIVE_REMOVE_PATH:
                return glob_item(i, remove_item_instance);

        default:
                return 0;
        }
}

static int clean_item_instance(Item *i, const char* instance) {
        _cleanup_closedir_ DIR *d = NULL;
        struct stat s, ps;
        bool mountpoint;
        usec_t cutoff, n;
        char timestamp[FORMAT_TIMESTAMP_MAX];

        assert(i);

        if (!i->age_set)
                return 0;

        n = now(CLOCK_REALTIME);
        if (n < i->age)
                return 0;

        cutoff = n - i->age;

        d = opendir_nomod(instance);
        if (!d) {
                if (IN_SET(errno, ENOENT, ENOTDIR)) {
                        log_debug_errno(errno, "Directory \"%s\": %m", instance);
                        return 0;
                }

                return log_error_errno(errno, "Failed to open directory %s: %m", instance);
        }

        if (fstat(dirfd(d), &s) < 0)
                return log_error_errno(errno, "stat(%s) failed: %m", i->path);

        if (!S_ISDIR(s.st_mode)) {
                log_error("%s is not a directory.", i->path);
                return -ENOTDIR;
        }

        if (fstatat(dirfd(d), "..", &ps, AT_SYMLINK_NOFOLLOW) != 0)
                return log_error_errno(errno, "stat(%s/..) failed: %m", i->path);

        mountpoint = s.st_dev != ps.st_dev || s.st_ino == ps.st_ino;

        log_debug("Cleanup threshold for %s \"%s\" is %s",
                  mountpoint ? "mount point" : "directory",
                  instance,
                  format_timestamp_us(timestamp, sizeof(timestamp), cutoff));

        return dir_cleanup(i, instance, d, &s, cutoff, s.st_dev, mountpoint,
                           MAX_DEPTH, i->keep_first_level);
}

static int clean_item(Item *i) {
        assert(i);

        log_debug("Running clean action for entry %c %s", (char) i->type, i->path);

        switch (i->type) {
        case CREATE_DIRECTORY:
        case CREATE_SUBVOLUME:
        case CREATE_SUBVOLUME_INHERIT_QUOTA:
        case CREATE_SUBVOLUME_NEW_QUOTA:
        case TRUNCATE_DIRECTORY:
        case IGNORE_PATH:
        case COPY_FILES:
                clean_item_instance(i, i->path);
                return 0;
        case EMPTY_DIRECTORY:
        case IGNORE_DIRECTORY_PATH:
                return glob_item(i, clean_item_instance);
        default:
                return 0;
        }
}

static int process_item_array(ItemArray *array);

static int process_item(Item *i) {
        int r, q, p, t = 0;
        _cleanup_free_ char *prefix = NULL;

        assert(i);

        if (i->done)
                return 0;

        i->done = true;

        prefix = malloc(strlen(i->path) + 1);
        if (!prefix)
                return log_oom();

        PATH_FOREACH_PREFIX(prefix, i->path) {
                ItemArray *j;

                j = ordered_hashmap_get(items, prefix);
                if (j) {
                        int s;

                        s = process_item_array(j);
                        if (s < 0 && t == 0)
                                t = s;
                }
        }

        if (chase_symlinks(i->path, NULL, CHASE_NO_AUTOFS, NULL) == -EREMOTE)
                return t;

        r = arg_create ? create_item(i) : 0;
        q = arg_remove ? remove_item(i) : 0;
        p = arg_clean ? clean_item(i) : 0;

        return t < 0 ? t :
                r < 0 ? r :
                q < 0 ? q :
                p;
}

static int process_item_array(ItemArray *array) {
        unsigned n;
        int r = 0, k;

        assert(array);

        for (n = 0; n < array->count; n++) {
                k = process_item(array->items + n);
                if (k < 0 && r == 0)
                        r = k;
        }

        return r;
}

static void item_free_contents(Item *i) {
        assert(i);
        free(i->path);
        free(i->argument);
        strv_free(i->xattrs);

#if HAVE_ACL
        acl_free(i->acl_access);
        acl_free(i->acl_default);
#endif
}

static void item_array_free(ItemArray *a) {
        unsigned n;

        if (!a)
                return;

        for (n = 0; n < a->count; n++)
                item_free_contents(a->items + n);
        free(a->items);
        free(a);
}

static int item_compare(const void *a, const void *b) {
        const Item *x = a, *y = b;

        /* Make sure that the ownership taking item is put first, so
         * that we first create the node, and then can adjust it */

        if (takes_ownership(x->type) && !takes_ownership(y->type))
                return -1;
        if (!takes_ownership(x->type) && takes_ownership(y->type))
                return 1;

        return (int) x->type - (int) y->type;
}

static bool item_compatible(Item *a, Item *b) {
        assert(a);
        assert(b);
        assert(streq(a->path, b->path));

        if (takes_ownership(a->type) && takes_ownership(b->type))
                /* check if the items are the same */
                return  streq_ptr(a->argument, b->argument) &&

                        a->uid_set == b->uid_set &&
                        a->uid == b->uid &&

                        a->gid_set == b->gid_set &&
                        a->gid == b->gid &&

                        a->mode_set == b->mode_set &&
                        a->mode == b->mode &&

                        a->age_set == b->age_set &&
                        a->age == b->age &&

                        a->mask_perms == b->mask_perms &&

                        a->keep_first_level == b->keep_first_level &&

                        a->major_minor == b->major_minor;

        return true;
}

static bool should_include_path(const char *path) {
        char **prefix;

        STRV_FOREACH(prefix, arg_exclude_prefixes)
                if (path_startswith(path, *prefix)) {
                        log_debug("Entry \"%s\" matches exclude prefix \"%s\", skipping.",
                                  path, *prefix);
                        return false;
                }

        STRV_FOREACH(prefix, arg_include_prefixes)
                if (path_startswith(path, *prefix)) {
                        log_debug("Entry \"%s\" matches include prefix \"%s\".", path, *prefix);
                        return true;
                }

        /* no matches, so we should include this path only if we
         * have no whitelist at all */
        if (strv_isempty(arg_include_prefixes))
                return true;

        log_debug("Entry \"%s\" does not match any include prefix, skipping.", path);
        return false;
}

static int specifier_expansion_from_arg(Item *i) {
        _cleanup_free_ char *unescaped = NULL, *resolved = NULL;
        char **xattr;
        int r;

        assert(i);

        if (i->argument == NULL)
                return 0;

        switch (i->type) {
        case COPY_FILES:
        case CREATE_SYMLINK:
        case CREATE_FILE:
        case TRUNCATE_FILE:
        case WRITE_FILE:
                r = cunescape(i->argument, 0, &unescaped);
                if (r < 0)
                        return log_error_errno(r, "Failed to unescape parameter to write: %s", i->argument);

                r = specifier_printf(unescaped, specifier_table, NULL, &resolved);
                if (r < 0)
                        return r;

                free_and_replace(i->argument, resolved);
                break;

        case SET_XATTR:
        case RECURSIVE_SET_XATTR:
                assert(i->xattrs);

                STRV_FOREACH (xattr, i->xattrs) {
                        r = specifier_printf(*xattr, specifier_table, NULL, &resolved);
                        if (r < 0)
                                return r;

                        free_and_replace(*xattr, resolved);
                }
                break;

        default:
                break;
        }
        return 0;
}

static int patch_var_run(const char *fname, unsigned line, char **path) {
        const char *k;
        char *n;

        assert(path);
        assert(*path);

        /* Optionally rewrites lines referencing /var/run/, to use /run/ instead. Why bother? tmpfiles merges lines in
         * some cases and detects conflicts in others. If files/directories are specified through two equivalent lines
         * this is problematic as neither case will be detected. Ideally we'd detect these cases by resolving symlinks
         * early, but that's precisely not what we can do here as this code very likely is running very early on, at a
         * time where the paths in question are not available yet, or even more importantly, our own tmpfiles rules
         * might create the paths that are intermediary to the listed paths. We can't really cover the generic case,
         * but the least we can do is cover the specific case of /var/run vs. /run, as /var/run is a legacy name for
         * /run only, and we explicitly document that and require that on systemd systems the former is a symlink to
         * the latter. Moreover files below this path are by far the primary usecase for tmpfiles.d/. */

        k = path_startswith(*path, "/var/run/");
        if (isempty(k)) /* Don't complain about other paths than /var/run, and not about /var/run itself either. */
                return 0;

        n = strjoin("/run/", k);
        if (!n)
                return log_oom();

        /* Also log about this briefly. We do so at LOG_NOTICE level, as we fixed up the situation automatically, hence
         * there's no immediate need for action by the user. However, in the interest of making things less confusing
         * to the user, let's still inform the user that these snippets should really be updated. */

        log_notice("[%s:%u] Line references path below legacy directory /var/run/, updating %s → %s; please update the tmpfiles.d/ drop-in file accordingly.", fname, line, *path, n);

        free(*path);
        *path = n;

        return 0;
}

static int parse_line(const char *fname, unsigned line, const char *buffer, bool *invalid_config) {

        _cleanup_free_ char *action = NULL, *mode = NULL, *user = NULL, *group = NULL, *age = NULL, *path = NULL;
        _cleanup_(item_free_contents) Item i = {};
        ItemArray *existing;
        OrderedHashmap *h;
        int r, pos;
        bool force = false, boot = false;

        assert(fname);
        assert(line >= 1);
        assert(buffer);

        r = extract_many_words(
                        &buffer,
                        NULL,
                        EXTRACT_QUOTES,
                        &action,
                        &path,
                        &mode,
                        &user,
                        &group,
                        &age,
                        NULL);
        if (r < 0) {
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        /* invalid quoting and such or an unknown specifier */
                        *invalid_config = true;
                return log_error_errno(r, "[%s:%u] Failed to parse line: %m", fname, line);
        } else if (r < 2) {
                *invalid_config = true;
                log_error("[%s:%u] Syntax error.", fname, line);
                return -EIO;
        }

        if (!isempty(buffer) && !streq(buffer, "-")) {
                i.argument = strdup(buffer);
                if (!i.argument)
                        return log_oom();
        }

        if (isempty(action)) {
                *invalid_config = true;
                log_error("[%s:%u] Command too short '%s'.", fname, line, action);
                return -EINVAL;
        }

        for (pos = 1; action[pos]; pos++) {
                if (action[pos] == '!' && !boot)
                        boot = true;
                else if (action[pos] == '+' && !force)
                        force = true;
                else {
                        *invalid_config = true;
                        log_error("[%s:%u] Unknown modifiers in command '%s'",
                                  fname, line, action);
                        return -EINVAL;
                }
        }

        if (boot && !arg_boot) {
                log_debug("Ignoring entry %s \"%s\" because --boot is not specified.",
                          action, path);
                return 0;
        }

        i.type = action[0];
        i.force = force;

        r = specifier_printf(path, specifier_table, NULL, &i.path);
        if (r == -ENXIO)
                return log_unresolvable_specifier(fname, line);
        if (r < 0) {
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        *invalid_config = true;
                return log_error_errno(r, "[%s:%u] Failed to replace specifiers: %s", fname, line, path);
        }

        r = patch_var_run(fname, line, &i.path);
        if (r < 0)
                return r;

        switch (i.type) {

        case CREATE_DIRECTORY:
        case CREATE_SUBVOLUME:
        case CREATE_SUBVOLUME_INHERIT_QUOTA:
        case CREATE_SUBVOLUME_NEW_QUOTA:
        case EMPTY_DIRECTORY:
        case TRUNCATE_DIRECTORY:
        case CREATE_FIFO:
        case IGNORE_PATH:
        case IGNORE_DIRECTORY_PATH:
        case REMOVE_PATH:
        case RECURSIVE_REMOVE_PATH:
        case ADJUST_MODE:
        case RELABEL_PATH:
        case RECURSIVE_RELABEL_PATH:
                if (i.argument)
                        log_warning("[%s:%u] %c lines don't take argument fields, ignoring.", fname, line, i.type);

                break;

        case CREATE_FILE:
        case TRUNCATE_FILE:
                break;

        case CREATE_SYMLINK:
                if (!i.argument) {
                        i.argument = strappend("/usr/share/factory/", i.path);
                        if (!i.argument)
                                return log_oom();
                }
                break;

        case WRITE_FILE:
                if (!i.argument) {
                        *invalid_config = true;
                        log_error("[%s:%u] Write file requires argument.", fname, line);
                        return -EBADMSG;
                }
                break;

        case COPY_FILES:
                if (!i.argument) {
                        i.argument = strappend("/usr/share/factory/", i.path);
                        if (!i.argument)
                                return log_oom();
                } else if (!path_is_absolute(i.argument)) {
                        *invalid_config = true;
                        log_error("[%s:%u] Source path is not absolute.", fname, line);
                        return -EBADMSG;
                }

                path_simplify(i.argument, false);
                break;

        case CREATE_CHAR_DEVICE:
        case CREATE_BLOCK_DEVICE: {
                unsigned major, minor;

                if (!i.argument) {
                        *invalid_config = true;
                        log_error("[%s:%u] Device file requires argument.", fname, line);
                        return -EBADMSG;
                }

                if (sscanf(i.argument, "%u:%u", &major, &minor) != 2) {
                        *invalid_config = true;
                        log_error("[%s:%u] Can't parse device file major/minor '%s'.", fname, line, i.argument);
                        return -EBADMSG;
                }

                i.major_minor = makedev(major, minor);
                break;
        }

        case SET_XATTR:
        case RECURSIVE_SET_XATTR:
                if (!i.argument) {
                        *invalid_config = true;
                        log_error("[%s:%u] Set extended attribute requires argument.", fname, line);
                        return -EBADMSG;
                }
                r = parse_xattrs_from_arg(&i);
                if (r < 0)
                        return r;
                break;

        case SET_ACL:
        case RECURSIVE_SET_ACL:
                if (!i.argument) {
                        *invalid_config = true;
                        log_error("[%s:%u] Set ACLs requires argument.", fname, line);
                        return -EBADMSG;
                }
                r = parse_acls_from_arg(&i);
                if (r < 0)
                        return r;
                break;

        case SET_ATTRIBUTE:
        case RECURSIVE_SET_ATTRIBUTE:
                if (!i.argument) {
                        *invalid_config = true;
                        log_error("[%s:%u] Set file attribute requires argument.", fname, line);
                        return -EBADMSG;
                }
                r = parse_attribute_from_arg(&i);
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        *invalid_config = true;
                if (r < 0)
                        return r;
                break;

        default:
                log_error("[%s:%u] Unknown command type '%c'.", fname, line, (char) i.type);
                *invalid_config = true;
                return -EBADMSG;
        }

        if (!path_is_absolute(i.path)) {
                log_error("[%s:%u] Path '%s' not absolute.", fname, line, i.path);
                *invalid_config = true;
                return -EBADMSG;
        }

        path_simplify(i.path, false);

        if (!should_include_path(i.path))
                return 0;

        r = specifier_expansion_from_arg(&i);
        if (r == -ENXIO)
                return log_unresolvable_specifier(fname, line);
        if (r < 0) {
                if (IN_SET(r, -EINVAL, -EBADSLT))
                        *invalid_config = true;
                return log_error_errno(r, "[%s:%u] Failed to substitute specifiers in argument: %m",
                                       fname, line);
        }

        if (arg_root) {
                char *p;

                p = prefix_root(arg_root, i.path);
                if (!p)
                        return log_oom();

                free(i.path);
                i.path = p;
        }

        if (!isempty(user) && !streq(user, "-")) {
                const char *u = user;

                r = get_user_creds(&u, &i.uid, NULL, NULL, NULL);
                if (r < 0) {
                        *invalid_config = true;
                        return log_error_errno(r, "[%s:%u] Unknown user '%s'.", fname, line, user);
                }

                i.uid_set = true;
        }

        if (!isempty(group) && !streq(group, "-")) {
                const char *g = group;

                r = get_group_creds(&g, &i.gid);
                if (r < 0) {
                        *invalid_config = true;
                        log_error("[%s:%u] Unknown group '%s'.", fname, line, group);
                        return r;
                }

                i.gid_set = true;
        }

        if (!isempty(mode) && !streq(mode, "-")) {
                const char *mm = mode;
                unsigned m;

                if (*mm == '~') {
                        i.mask_perms = true;
                        mm++;
                }

                if (parse_mode(mm, &m) < 0) {
                        *invalid_config = true;
                        log_error("[%s:%u] Invalid mode '%s'.", fname, line, mode);
                        return -EBADMSG;
                }

                i.mode = m;
                i.mode_set = true;
        } else
                i.mode = IN_SET(i.type, CREATE_DIRECTORY, TRUNCATE_DIRECTORY, CREATE_SUBVOLUME, CREATE_SUBVOLUME_INHERIT_QUOTA, CREATE_SUBVOLUME_NEW_QUOTA) ? 0755 : 0644;

        if (!isempty(age) && !streq(age, "-")) {
                const char *a = age;

                if (*a == '~') {
                        i.keep_first_level = true;
                        a++;
                }

                if (parse_sec(a, &i.age) < 0) {
                        *invalid_config = true;
                        log_error("[%s:%u] Invalid age '%s'.", fname, line, age);
                        return -EBADMSG;
                }

                i.age_set = true;
        }

        h = needs_glob(i.type) ? globs : items;

        existing = ordered_hashmap_get(h, i.path);
        if (existing) {
                unsigned n;

                for (n = 0; n < existing->count; n++) {
                        if (!item_compatible(existing->items + n, &i)) {
                                log_notice("[%s:%u] Duplicate line for path \"%s\", ignoring.",
                                           fname, line, i.path);
                                return 0;
                        }
                }
        } else {
                existing = new0(ItemArray, 1);
                if (!existing)
                        return log_oom();

                r = ordered_hashmap_put(h, i.path, existing);
                if (r < 0)
                        return log_oom();
        }

        if (!GREEDY_REALLOC(existing->items, existing->size, existing->count + 1))
                return log_oom();

        memcpy(existing->items + existing->count++, &i, sizeof(i));

        /* Sort item array, to enforce stable ordering of application */
        qsort_safe(existing->items, existing->count, sizeof(Item), item_compare);

        zero(i);
        return 0;
}

static int cat_config(char **config_dirs, char **args) {
        _cleanup_strv_free_ char **files = NULL;
        int r;

        r = conf_files_list_with_replacement(arg_root, config_dirs, arg_replace, &files, NULL);
        if (r < 0)
                return r;

        return cat_files(NULL, files, 0);
}

static void help(void) {
        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n\n"
               "Creates, deletes and cleans up volatile and temporary files and directories.\n\n"
               "  -h --help                 Show this help\n"
               "     --user                 Execute user configuration\n"
               "     --version              Show package version\n"
               "     --cat-config           Show configuration files\n"
               "     --create               Create marked files/directories\n"
               "     --clean                Clean up marked directories\n"
               "     --remove               Remove marked files/directories\n"
               "     --boot                 Execute actions only safe at boot\n"
               "     --prefix=PATH          Only apply rules with the specified prefix\n"
               "     --exclude-prefix=PATH  Ignore rules with the specified prefix\n"
               "     --root=PATH            Operate on an alternate filesystem root\n"
               "     --replace=PATH         Treat arguments as replacement for PATH\n"
               "     --no-pager             Do not pipe output into a pager\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_CAT_CONFIG,
                ARG_USER,
                ARG_CREATE,
                ARG_CLEAN,
                ARG_REMOVE,
                ARG_BOOT,
                ARG_PREFIX,
                ARG_EXCLUDE_PREFIX,
                ARG_ROOT,
                ARG_REPLACE,
                ARG_NO_PAGER,
        };

        static const struct option options[] = {
                { "help",           no_argument,         NULL, 'h'                },
                { "user",           no_argument,         NULL, ARG_USER           },
                { "version",        no_argument,         NULL, ARG_VERSION        },
                { "cat-config",     no_argument,         NULL, ARG_CAT_CONFIG     },
                { "create",         no_argument,         NULL, ARG_CREATE         },
                { "clean",          no_argument,         NULL, ARG_CLEAN          },
                { "remove",         no_argument,         NULL, ARG_REMOVE         },
                { "boot",           no_argument,         NULL, ARG_BOOT           },
                { "prefix",         required_argument,   NULL, ARG_PREFIX         },
                { "exclude-prefix", required_argument,   NULL, ARG_EXCLUDE_PREFIX },
                { "root",           required_argument,   NULL, ARG_ROOT           },
                { "replace",        required_argument,   NULL, ARG_REPLACE        },
                { "no-pager",       no_argument,         NULL, ARG_NO_PAGER       },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_CAT_CONFIG:
                        arg_cat_config = true;
                        break;

                case ARG_USER:
                        arg_user = true;
                        break;

                case ARG_CREATE:
                        arg_create = true;
                        break;

                case ARG_CLEAN:
                        arg_clean = true;
                        break;

                case ARG_REMOVE:
                        arg_remove = true;
                        break;

                case ARG_BOOT:
                        arg_boot = true;
                        break;

                case ARG_PREFIX:
                        if (strv_push(&arg_include_prefixes, optarg) < 0)
                                return log_oom();
                        break;

                case ARG_EXCLUDE_PREFIX:
                        if (strv_push(&arg_exclude_prefixes, optarg) < 0)
                                return log_oom();
                        break;

                case ARG_ROOT:
                        r = parse_path_argument_and_warn(optarg, true, &arg_root);
                        if (r < 0)
                                return r;
                        break;

                case ARG_REPLACE:
                        if (!path_is_absolute(optarg) ||
                            !endswith(optarg, ".conf")) {
                                log_error("The argument to --replace= must an absolute path to a config file");
                                return -EINVAL;
                        }

                        arg_replace = optarg;
                        break;

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (!arg_clean && !arg_create && !arg_remove && !arg_cat_config) {
                log_error("You need to specify at least one of --clean, --create or --remove.");
                return -EINVAL;
        }

        if (arg_replace && arg_cat_config) {
                log_error("Option --replace= is not supported with --cat-config");
                return -EINVAL;
        }

        if (arg_replace && optind >= argc) {
                log_error("When --replace= is given, some configuration items must be specified");
                return -EINVAL;
        }

        return 1;
}

static int read_config_file(char **config_dirs, const char *fn, bool ignore_enoent, bool *invalid_config) {
        _cleanup_fclose_ FILE *_f = NULL;
        FILE *f;
        char line[LINE_MAX];
        Iterator iterator;
        unsigned v = 0;
        ItemArray *ia;
        int r = 0;

        assert(fn);

        if (streq(fn, "-")) {
                log_debug("Reading config from stdin…");
                fn = "<stdin>";
                f = stdin;
        } else {
                r = search_and_fopen(fn, "re", arg_root, (const char**) config_dirs, &_f);
                if (r < 0) {
                        if (ignore_enoent && r == -ENOENT) {
                                log_debug_errno(r, "Failed to open \"%s\", ignoring: %m", fn);
                                return 0;
                        }

                        return log_error_errno(r, "Failed to open '%s': %m", fn);
                }
                log_debug("Reading config file \"%s\"…", fn);
                f = _f;
        }

        FOREACH_LINE(line, f, break) {
                char *l;
                int k;
                bool invalid_line = false;

                v++;

                l = strstrip(line);
                if (IN_SET(*l, 0, '#'))
                        continue;

                k = parse_line(fn, v, l, &invalid_line);
                if (k < 0) {
                        if (invalid_line)
                                /* Allow reporting with a special code if the caller requested this */
                                *invalid_config = true;
                        else if (r == 0)
                                /* The first error becomes our return value */
                                r = k;
                }
        }

        /* we have to determine age parameter for each entry of type X */
        ORDERED_HASHMAP_FOREACH(ia, globs, iterator)
                for (size_t ni = 0; ni < ia->count; ni++) {
                        Iterator iter;
                        ItemArray *ja;
                        Item *i = ia->items + ni, *candidate_item = NULL;

                        if (i->type != IGNORE_DIRECTORY_PATH)
                                continue;

                        ORDERED_HASHMAP_FOREACH(ja, items, iter)
                                for (size_t nj = 0; nj < ja->count; nj++) {
                                        Item *j = ja->items + nj;

                                        if (!IN_SET(j->type, CREATE_DIRECTORY,
                                                             TRUNCATE_DIRECTORY,
                                                             CREATE_SUBVOLUME,
                                                             CREATE_SUBVOLUME_INHERIT_QUOTA,
                                                             CREATE_SUBVOLUME_NEW_QUOTA))
                                                continue;

                                        if (path_equal(j->path, i->path)) {
                                                candidate_item = j;
                                                break;
                                        }

                                        if (candidate_item
                                            ? (path_startswith(j->path, candidate_item->path) && fnmatch(i->path, j->path, FNM_PATHNAME | FNM_PERIOD) == 0)
                                            : path_startswith(i->path, j->path) != NULL)
                                                candidate_item = j;
                                }

                        if (candidate_item && candidate_item->age_set) {
                                i->age = candidate_item->age;
                                i->age_set = true;
                        }
                }

        if (ferror(f)) {
                log_error_errno(errno, "Failed to read from file %s: %m", fn);
                if (r == 0)
                        r = -EIO;
        }

        return r;
}

static int parse_arguments(char **config_dirs, char **args, bool *invalid_config) {
        char **arg;
        int r;

        STRV_FOREACH(arg, args) {
                r = read_config_file(config_dirs, *arg, false, invalid_config);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int read_config_files(char **config_dirs, char **args, bool *invalid_config) {
        _cleanup_strv_free_ char **files = NULL;
        _cleanup_free_ char *p = NULL;
        char **f;
        int r;

        r = conf_files_list_with_replacement(arg_root, config_dirs, arg_replace, &files, &p);
        if (r < 0)
                return r;

        STRV_FOREACH(f, files)
                if (p && path_equal(*f, p)) {
                        log_debug("Parsing arguments at position \"%s\"…", *f);

                        r = parse_arguments(config_dirs, args, invalid_config);
                        if (r < 0)
                                return r;
                } else
                        /* Just warn, ignore result otherwise.
                         * read_config_file() has some debug output, so no need to print anything. */
                        (void) read_config_file(config_dirs, *f, true, invalid_config);

        return 0;
}

int main(int argc, char *argv[]) {
        int r, k, r_process = 0;
        ItemArray *a;
        Iterator iterator;
        _cleanup_strv_free_ char **config_dirs = NULL;
        bool invalid_config = false;

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        if (arg_user) {
                r = user_config_paths(&config_dirs);
                if (r < 0) {
                        log_error_errno(r, "Failed to initialize configuration directory list: %m");
                        goto finish;
                }
        } else {
                config_dirs = strv_split_nulstr(CONF_PATHS_NULSTR("tmpfiles.d"));
                if (!config_dirs) {
                        r = log_oom();
                        goto finish;
                }
        }

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *t = NULL;

                t = strv_join(config_dirs, "\n\t");
                if (t)
                        log_debug("Looking for configuration files in (higher priority first):\n\t%s", t);
        }

        if (arg_cat_config) {
                (void) pager_open(arg_no_pager, false);

                r = cat_config(config_dirs, argv + optind);
                goto finish;
        }

        umask(0022);

        mac_selinux_init();

        items = ordered_hashmap_new(&string_hash_ops);
        globs = ordered_hashmap_new(&string_hash_ops);

        if (!items || !globs) {
                r = log_oom();
                goto finish;
        }

        /* If command line arguments are specified along with --replace, read all
         * configuration files and insert the positional arguments at the specified
         * place. Otherwise, if command line arguments are specified, execute just
         * them, and finally, without --replace= or any positional arguments, just
         * read configuration and execute it.
         */
        if (arg_replace || optind >= argc)
                r = read_config_files(config_dirs, argv + optind, &invalid_config);
        else
                r = parse_arguments(config_dirs, argv + optind, &invalid_config);
        if (r < 0)
                goto finish;

        /* The non-globbing ones usually create things, hence we apply
         * them first */
        ORDERED_HASHMAP_FOREACH(a, items, iterator) {
                k = process_item_array(a);
                if (k < 0 && r_process == 0)
                        r_process = k;
        }

        /* The globbing ones usually alter things, hence we apply them
         * second. */
        ORDERED_HASHMAP_FOREACH(a, globs, iterator) {
                k = process_item_array(a);
                if (k < 0 && r_process == 0)
                        r_process = k;
        }

finish:
        pager_close();

        ordered_hashmap_free_with_destructor(items, item_array_free);
        ordered_hashmap_free_with_destructor(globs, item_array_free);

        free(arg_include_prefixes);
        free(arg_exclude_prefixes);
        free(arg_root);

        set_free_free(unix_sockets);

        mac_selinux_finish();

        if (r < 0 || ERRNO_IS_RESOURCE(-r_process))
                return EXIT_FAILURE;
        else if (invalid_config)
                return EX_DATAERR;
        else if (r_process < 0)
                return EX_CANTCREAT;
        else
                return EXIT_SUCCESS;
}
