/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  Copyright © 2010 ProFUSION embedded systems
***/

#include <errno.h>
#include <fcntl.h>
#include <linux/major.h>
#include <linux/raid/md_u.h>
#include <linux/loop.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/swap.h>

/* This needs to be after sys/mount.h :( */
#include <libmount.h>

#include "libudev.h"

#include "alloc-util.h"
#include "blockdev-util.h"
#include "def.h"
#include "escape.h"
#include "fd-util.h"
#include "fstab-util.h"
#include "linux-3.13/dm-ioctl.h"
#include "mount-setup.h"
#include "mount-util.h"
#include "path-util.h"
#include "process-util.h"
#include "signal-util.h"
#include "string-util.h"
#include "udev-util.h"
#include "umount.h"
#include "util.h"
#include "virt.h"

DEFINE_TRIVIAL_CLEANUP_FUNC(struct libmnt_table*, mnt_free_table);
DEFINE_TRIVIAL_CLEANUP_FUNC(struct libmnt_iter*, mnt_free_iter);

static void mount_point_free(MountPoint **head, MountPoint *m) {
        assert(head);
        assert(m);

        LIST_REMOVE(mount_point, *head, m);

        free(m->path);
        free(m->remount_options);
        free(m);
}

void mount_points_list_free(MountPoint **head) {
        assert(head);

        while (*head)
                mount_point_free(head, *head);
}

int mount_points_list_get(const char *mountinfo, MountPoint **head) {
        _cleanup_(mnt_free_tablep) struct libmnt_table *table = NULL;
        _cleanup_(mnt_free_iterp) struct libmnt_iter *iter = NULL;
        int r;

        assert(head);

        table = mnt_new_table();
        iter = mnt_new_iter(MNT_ITER_FORWARD);
        if (!table || !iter)
                return log_oom();

        r = mnt_table_parse_mtab(table, mountinfo);
        if (r < 0)
                return log_error_errno(r, "Failed to parse %s: %m", mountinfo);

        for (;;) {
                struct libmnt_fs *fs;
                const char *path, *options, *fstype;
                _cleanup_free_ char *p = NULL;
                unsigned long remount_flags = 0u;
                _cleanup_free_ char *remount_options = NULL;
                bool try_remount_ro;
                MountPoint *m;

                r = mnt_table_next_fs(table, iter, &fs);
                if (r == 1)
                        break;
                if (r < 0)
                        return log_error_errno(r, "Failed to get next entry from %s: %m", mountinfo);

                path = mnt_fs_get_target(fs);
                if (!path)
                        continue;

                if (cunescape(path, UNESCAPE_RELAX, &p) < 0)
                        return log_oom();

                options = mnt_fs_get_options(fs);
                fstype = mnt_fs_get_fstype(fs);

                /* Ignore mount points we can't unmount because they
                 * are API or because we are keeping them open (like
                 * /dev/console). Also, ignore all mounts below API
                 * file systems, since they are likely virtual too,
                 * and hence not worth spending time on. Also, in
                 * unprivileged containers we might lack the rights to
                 * unmount these things, hence don't bother. */
                if (mount_point_is_api(p) ||
                    mount_point_ignore(p) ||
                    path_startswith(p, "/dev") ||
                    path_startswith(p, "/sys") ||
                    path_startswith(p, "/proc"))
                        continue;

                /* If we are in a container, don't attempt to
                 * read-only mount anything as that brings no real
                 * benefits, but might confuse the host, as we remount
                 * the superblock here, not the bind mount.
                 *
                 * If the filesystem is a network fs, also skip the
                 * remount. It brings no value (we cannot leave
                 * a "dirty fs") and could hang if the network is down.
                 * Note that umount2() is more careful and will not
                 * hang because of the network being down. */
                try_remount_ro = detect_container() <= 0 &&
                                 !fstype_is_network(fstype) &&
                                 !fstype_is_api_vfs(fstype) &&
                                 !fstype_is_ro(fstype) &&
                                 !fstab_test_yes_no_option(options, "ro\0rw\0");

                if (try_remount_ro) {
                        /* mount(2) states that mount flags and options need to be exactly the same
                         * as they were when the filesystem was mounted, except for the desired
                         * changes. So we reconstruct both here and adjust them for the later
                         * remount call too. */

                        r = mnt_fs_get_propagation(fs, &remount_flags);
                        if (r < 0) {
                                log_warning_errno(r, "mnt_fs_get_propagation() failed for %s, ignoring: %m", path);
                                continue;
                        }

                        r = mount_option_mangle(options, remount_flags, &remount_flags, &remount_options);
                        if (r < 0) {
                                log_warning_errno(r, "mount_option_mangle failed for %s, ignoring: %m", path);
                                continue;
                        }

                        /* MS_BIND is special. If it is provided it will only make the mount-point
                         * read-only. If left out, the super block itself is remounted, which we want. */
                        remount_flags = (remount_flags|MS_REMOUNT|MS_RDONLY) & ~MS_BIND;
                }

                m = new0(MountPoint, 1);
                if (!m)
                        return log_oom();

                free_and_replace(m->path, p);
                free_and_replace(m->remount_options, remount_options);
                m->remount_flags = remount_flags;
                m->try_remount_ro = try_remount_ro;

                LIST_PREPEND(mount_point, *head, m);
        }

        return 0;
}

int swap_list_get(const char *swaps, MountPoint **head) {
        _cleanup_(mnt_free_tablep) struct libmnt_table *t = NULL;
        _cleanup_(mnt_free_iterp) struct libmnt_iter *i = NULL;
        int r;

        assert(head);

        t = mnt_new_table();
        i = mnt_new_iter(MNT_ITER_FORWARD);
        if (!t || !i)
                return log_oom();

        r = mnt_table_parse_swaps(t, swaps);
        if (r < 0)
                return log_error_errno(r, "Failed to parse %s: %m", swaps);

        for (;;) {
                struct libmnt_fs *fs;

                MountPoint *swap;
                const char *source;
                _cleanup_free_ char *d = NULL;

                r = mnt_table_next_fs(t, i, &fs);
                if (r == 1)
                        break;
                if (r < 0)
                        return log_error_errno(r, "Failed to get next entry from %s: %m", swaps);

                source = mnt_fs_get_source(fs);
                if (!source)
                        continue;

                r = cunescape(source, UNESCAPE_RELAX, &d);
                if (r < 0)
                        return r;

                swap = new0(MountPoint, 1);
                if (!swap)
                        return -ENOMEM;

                free_and_replace(swap->path, d);
                LIST_PREPEND(mount_point, *head, swap);
        }

        return 0;
}

static int loopback_list_get(MountPoint **head) {
        _cleanup_(udev_enumerate_unrefp) struct udev_enumerate *e = NULL;
        struct udev_list_entry *item = NULL, *first = NULL;
        _cleanup_(udev_unrefp) struct udev *udev = NULL;
        int r;

        assert(head);

        udev = udev_new();
        if (!udev)
                return -ENOMEM;

        e = udev_enumerate_new(udev);
        if (!e)
                return -ENOMEM;

        r = udev_enumerate_add_match_subsystem(e, "block");
        if (r < 0)
                return r;

        r = udev_enumerate_add_match_sysname(e, "loop*");
        if (r < 0)
                return r;

        r = udev_enumerate_add_match_sysattr(e, "loop/backing_file", NULL);
        if (r < 0)
                return r;

        r = udev_enumerate_scan_devices(e);
        if (r < 0)
                return r;

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_(udev_device_unrefp) struct udev_device *d;
                const char *dn;
                _cleanup_free_ MountPoint *lb = NULL;

                d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                if (!d)
                        return -ENOMEM;

                dn = udev_device_get_devnode(d);
                if (!dn)
                        continue;

                lb = new0(MountPoint, 1);
                if (!lb)
                        return -ENOMEM;

                r = free_and_strdup(&lb->path, dn);
                if (r < 0)
                        return r;

                LIST_PREPEND(mount_point, *head, lb);
                lb = NULL;
        }

        return 0;
}

static int dm_list_get(MountPoint **head) {
        _cleanup_(udev_enumerate_unrefp) struct udev_enumerate *e = NULL;
        struct udev_list_entry *item = NULL, *first = NULL;
        _cleanup_(udev_unrefp) struct udev *udev = NULL;
        int r;

        assert(head);

        udev = udev_new();
        if (!udev)
                return -ENOMEM;

        e = udev_enumerate_new(udev);
        if (!e)
                return -ENOMEM;

        r = udev_enumerate_add_match_subsystem(e, "block");
        if (r < 0)
                return r;

        r = udev_enumerate_add_match_sysname(e, "dm-*");
        if (r < 0)
                return r;

        r = udev_enumerate_scan_devices(e);
        if (r < 0)
                return r;

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_(udev_device_unrefp) struct udev_device *d;
                dev_t devnum;
                const char *dn;
                _cleanup_free_ MountPoint *m = NULL;

                d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                if (!d)
                        return -ENOMEM;

                devnum = udev_device_get_devnum(d);
                dn = udev_device_get_devnode(d);
                if (major(devnum) == 0 || !dn)
                        continue;

                m = new0(MountPoint, 1);
                if (!m)
                        return -ENOMEM;

                m->devnum = devnum;
                r = free_and_strdup(&m->path, dn);
                if (r < 0)
                        return r;

                LIST_PREPEND(mount_point, *head, m);
                m = NULL;
        }

        return 0;
}

static int md_list_get(MountPoint **head) {
        _cleanup_(udev_enumerate_unrefp) struct udev_enumerate *e = NULL;
        struct udev_list_entry *item = NULL, *first = NULL;
        _cleanup_(udev_unrefp) struct udev *udev = NULL;
        int r;

        assert(head);

        udev = udev_new();
        if (!udev)
                return -ENOMEM;

        e = udev_enumerate_new(udev);
        if (!e)
                return -ENOMEM;

        r = udev_enumerate_add_match_subsystem(e, "block");
        if (r < 0)
                return r;

        r = udev_enumerate_add_match_sysname(e, "md*");
        if (r < 0)
                return r;

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_(udev_device_unrefp) struct udev_device *d;
                _cleanup_free_ char *p = NULL;
                const char *dn;
                MountPoint *m;
                dev_t devnum;

                d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                if (!d)
                        return -ENOMEM;

                devnum = udev_device_get_devnum(d);
                dn = udev_device_get_devnode(d);
                if (major(devnum) == 0 || !dn)
                        continue;

                p = strdup(dn);
                if (!p)
                        return -ENOMEM;

                m = new(MountPoint, 1);
                if (!m)
                        return -ENOMEM;

                *m = (MountPoint) {
                        .path = TAKE_PTR(p),
                        .devnum = devnum,
                };

                LIST_PREPEND(mount_point, *head, m);
        }

        return 0;
}

static int delete_loopback(const char *device) {
        _cleanup_close_ int fd = -1;
        int r;

        assert(device);

        fd = open(device, O_RDONLY|O_CLOEXEC);
        if (fd < 0)
                return errno == ENOENT ? 0 : -errno;

        r = ioctl(fd, LOOP_CLR_FD, 0);
        if (r >= 0)
                return 1;

        /* ENXIO: not bound, so no error */
        if (errno == ENXIO)
                return 0;

        return -errno;
}

static int delete_dm(dev_t devnum) {

        struct dm_ioctl dm = {
                .version = {
                        DM_VERSION_MAJOR,
                        DM_VERSION_MINOR,
                        DM_VERSION_PATCHLEVEL
                },
                .data_size = sizeof(dm),
                .dev = devnum,
        };

        _cleanup_close_ int fd = -1;

        assert(major(devnum) != 0);

        fd = open("/dev/mapper/control", O_RDWR|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (ioctl(fd, DM_DEV_REMOVE, &dm) < 0)
                return -errno;

        return 0;
}

static int delete_md(MountPoint *m) {

        _cleanup_close_ int fd = -1;

        assert(major(m->devnum) != 0);
        assert(m->path != 0);

        fd = open(m->path, O_RDONLY|O_CLOEXEC|O_EXCL);
        if (fd < 0)
                return -errno;

        if (ioctl(fd, STOP_ARRAY, NULL) < 0)
                return -errno;

        return 0;
}

static bool nonunmountable_path(const char *path) {
        return path_equal(path, "/")
#if ! HAVE_SPLIT_USR
                || path_equal(path, "/usr")
#endif
                || path_startswith(path, "/run/initramfs");
}

static int remount_with_timeout(MountPoint *m, int umount_log_level) {
        pid_t pid;
        int r;

        BLOCK_SIGNALS(SIGCHLD);

        assert(m);

        /* Due to the possiblity of a remount operation hanging, we
         * fork a child process and set a timeout. If the timeout
         * lapses, the assumption is that that particular remount
         * failed. */
        r = safe_fork("(sd-remount)", FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS|FORK_LOG|FORK_REOPEN_LOG, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                log_info("Remounting '%s' read-only in with options '%s'.", m->path, m->remount_options);

                /* Start the mount operation here in the child */
                r = mount(NULL, m->path, NULL, m->remount_flags, m->remount_options);
                if (r < 0)
                        log_full_errno(umount_log_level, errno, "Failed to remount '%s' read-only: %m", m->path);

                _exit(r < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
        }

        r = wait_for_terminate_with_timeout(pid, DEFAULT_TIMEOUT_USEC);
        if (r == -ETIMEDOUT) {
                log_error_errno(r, "Remounting '%s' timed out, issuing SIGKILL to PID " PID_FMT ".", m->path, pid);
                (void) kill(pid, SIGKILL);
        } else if (r == -EPROTO)
                log_debug_errno(r, "Remounting '%s' failed abnormally, child process " PID_FMT " aborted or exited non-zero.", m->path, pid);
        else if (r < 0)
                log_error_errno(r, "Remounting '%s' failed unexpectedly, couldn't wait for child process " PID_FMT ": %m", m->path, pid);

        return r;
}

static int umount_with_timeout(MountPoint *m, int umount_log_level) {
        pid_t pid;
        int r;

        BLOCK_SIGNALS(SIGCHLD);

        assert(m);

        /* Due to the possiblity of a umount operation hanging, we
         * fork a child process and set a timeout. If the timeout
         * lapses, the assumption is that that particular umount
         * failed. */
        r = safe_fork("(sd-umount)", FORK_RESET_SIGNALS|FORK_CLOSE_ALL_FDS|FORK_LOG|FORK_REOPEN_LOG, &pid);
        if (r < 0)
                return r;
        if (r == 0) {
                log_info("Unmounting '%s'.", m->path);

                /* Start the mount operation here in the child Using MNT_FORCE
                 * causes some filesystems (e.g. FUSE and NFS and other network
                 * filesystems) to abort any pending requests and return -EIO
                 * rather than blocking indefinitely. If the filesysten is
                 * "busy", this may allow processes to die, thus making the
                 * filesystem less busy so the unmount might succeed (rather
                 * then return EBUSY).*/
                r = umount2(m->path, MNT_FORCE);
                if (r < 0)
                        log_full_errno(umount_log_level, errno, "Failed to unmount %s: %m", m->path);

                _exit(r < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
        }

        r = wait_for_terminate_with_timeout(pid, DEFAULT_TIMEOUT_USEC);
        if (r == -ETIMEDOUT) {
                log_error_errno(r, "Unmounting '%s' timed out, issuing SIGKILL to PID " PID_FMT ".", m->path, pid);
                (void) kill(pid, SIGKILL);
        } else if (r == -EPROTO)
                log_debug_errno(r, "Unmounting '%s' failed abnormally, child process " PID_FMT " aborted or exited non-zero.", m->path, pid);
        else if (r < 0)
                log_error_errno(r, "Unmounting '%s' failed unexpectedly, couldn't wait for child process " PID_FMT ": %m", m->path, pid);

        return r;
}

/* This includes remounting readonly, which changes the kernel mount options.
 * Therefore the list passed to this function is invalidated, and should not be reused. */
static int mount_points_list_umount(MountPoint **head, bool *changed, int umount_log_level) {
        MountPoint *m;
        int n_failed = 0;

        assert(head);
        assert(changed);

        LIST_FOREACH(mount_point, m, *head) {
                if (m->try_remount_ro) {
                        /* We always try to remount directories
                         * read-only first, before we go on and umount
                         * them.
                         *
                         * Mount points can be stacked. If a mount
                         * point is stacked below / or /usr, we
                         * cannot umount or remount it directly,
                         * since there is no way to refer to the
                         * underlying mount. There's nothing we can do
                         * about it for the general case, but we can
                         * do something about it if it is aliased
                         * somehwere else via a bind mount. If we
                         * explicitly remount the super block of that
                         * alias read-only we hence should be
                         * relatively safe regarding keeping a dirty fs
                         * we cannot otherwise see.
                         *
                         * Since the remount can hang in the instance of
                         * remote filesystems, we remount asynchronously
                         * and skip the subsequent umount if it fails. */
                        if (remount_with_timeout(m, umount_log_level) < 0) {
                                /* Remount failed, but try unmounting anyway,
                                 * unless this is a mount point we want to skip. */
                                if (nonunmountable_path(m->path)) {
                                        n_failed++;
                                        continue;
                                }
                        }
                }

                /* Skip / and /usr since we cannot unmount that
                 * anyway, since we are running from it. They have
                 * already been remounted ro. */
                if (nonunmountable_path(m->path))
                        continue;

                /* Trying to umount */
                if (umount_with_timeout(m, umount_log_level) < 0)
                        n_failed++;
                else
                        *changed = true;
        }

        return n_failed;
}

static int swap_points_list_off(MountPoint **head, bool *changed) {
        MountPoint *m, *n;
        int n_failed = 0;

        assert(head);
        assert(changed);

        LIST_FOREACH_SAFE(mount_point, m, n, *head) {
                log_info("Deactivating swap %s.", m->path);
                if (swapoff(m->path) == 0) {
                        *changed = true;
                        mount_point_free(head, m);
                } else {
                        log_warning_errno(errno, "Could not deactivate swap %s: %m", m->path);
                        n_failed++;
                }
        }

        return n_failed;
}

static int loopback_points_list_detach(MountPoint **head, bool *changed, int umount_log_level) {
        MountPoint *m, *n;
        int n_failed = 0, k;
        struct stat root_st;

        assert(head);
        assert(changed);

        k = lstat("/", &root_st);

        LIST_FOREACH_SAFE(mount_point, m, n, *head) {
                int r;
                struct stat loopback_st;

                if (k >= 0 &&
                    major(root_st.st_dev) != 0 &&
                    lstat(m->path, &loopback_st) >= 0 &&
                    root_st.st_dev == loopback_st.st_rdev) {
                        n_failed++;
                        continue;
                }

                log_info("Detaching loopback %s.", m->path);
                r = delete_loopback(m->path);
                if (r >= 0) {
                        if (r > 0)
                                *changed = true;

                        mount_point_free(head, m);
                } else {
                        log_full_errno(umount_log_level, errno, "Could not detach loopback %s: %m", m->path);
                        n_failed++;
                }
        }

        return n_failed;
}

static int dm_points_list_detach(MountPoint **head, bool *changed, int umount_log_level) {
        MountPoint *m, *n;
        int n_failed = 0, r;
        dev_t rootdev;

        assert(head);
        assert(changed);

        r = get_block_device("/", &rootdev);
        if (r <= 0)
                rootdev = 0;

        LIST_FOREACH_SAFE(mount_point, m, n, *head) {

                if (major(rootdev) != 0 && rootdev == m->devnum) {
                        n_failed ++;
                        continue;
                }

                log_info("Detaching DM %u:%u.", major(m->devnum), minor(m->devnum));
                r = delete_dm(m->devnum);
                if (r >= 0) {
                        *changed = true;
                        mount_point_free(head, m);
                } else {
                        log_full_errno(umount_log_level, errno, "Could not detach DM %s: %m", m->path);
                        n_failed++;
                }
        }

        return n_failed;
}

static int md_points_list_detach(MountPoint **head, bool *changed, int umount_log_level) {
        MountPoint *m, *n;
        int n_failed = 0, r;
        dev_t rootdev = 0;

        assert(head);
        assert(changed);

        (void) get_block_device("/", &rootdev);

        LIST_FOREACH_SAFE(mount_point, m, n, *head) {
                if (major(rootdev) != 0 && rootdev == m->devnum) {
                        n_failed ++;
                        continue;
                }

                log_info("Stopping MD %s (%u:%u).", m->path, major(m->devnum), minor(m->devnum));
                r = delete_md(m);
                if (r < 0) {
                        log_full_errno(umount_log_level, r, "Could not stop MD %s: %m", m->path);
                        n_failed++;
                        continue;
                }

                *changed = true;
                mount_point_free(head, m);
        }

        return n_failed;
}

static int umount_all_once(bool *changed, int umount_log_level) {
        int r;
        _cleanup_(mount_points_list_free) LIST_HEAD(MountPoint, mp_list_head);

        assert(changed);

        LIST_HEAD_INIT(mp_list_head);
        r = mount_points_list_get(NULL, &mp_list_head);
        if (r < 0)
                return r;

        return mount_points_list_umount(&mp_list_head, changed, umount_log_level);
}

int umount_all(bool *changed, int umount_log_level) {
        bool umount_changed;
        int r;

        assert(changed);

        /* Retry umount, until nothing can be umounted anymore. Mounts are
         * processed in order, newest first. The retries are needed when
         * an old mount has been moved, to a path inside a newer mount. */
        do {
                umount_changed = false;

                r = umount_all_once(&umount_changed, umount_log_level);
                if (umount_changed)
                        *changed = true;
        } while (umount_changed);

        return r;
}

int swapoff_all(bool *changed) {
        _cleanup_(mount_points_list_free) LIST_HEAD(MountPoint, swap_list_head);
        int r;

        assert(changed);

        LIST_HEAD_INIT(swap_list_head);

        r = swap_list_get(NULL, &swap_list_head);
        if (r < 0)
                return r;

        return swap_points_list_off(&swap_list_head, changed);
}

int loopback_detach_all(bool *changed, int umount_log_level) {
        _cleanup_(mount_points_list_free) LIST_HEAD(MountPoint, loopback_list_head);
        int r;

        assert(changed);

        LIST_HEAD_INIT(loopback_list_head);

        r = loopback_list_get(&loopback_list_head);
        if (r < 0)
                return r;

        return loopback_points_list_detach(&loopback_list_head, changed, umount_log_level);
}

int dm_detach_all(bool *changed, int umount_log_level) {
        _cleanup_(mount_points_list_free) LIST_HEAD(MountPoint, dm_list_head);
        int r;

        assert(changed);

        LIST_HEAD_INIT(dm_list_head);

        r = dm_list_get(&dm_list_head);
        if (r < 0)
                return r;

        return dm_points_list_detach(&dm_list_head, changed, umount_log_level);
}

int md_detach_all(bool *changed, int umount_log_level) {
        _cleanup_(mount_points_list_free) LIST_HEAD(MountPoint, md_list_head);
        int r;

        assert(changed);

        LIST_HEAD_INIT(md_list_head);

        r = md_list_get(&md_list_head);
        if (r < 0)
                return r;

        return md_points_list_detach(&md_list_head, changed, umount_log_level);
}
