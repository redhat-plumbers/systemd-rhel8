/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

typedef struct ExecStatus ExecStatus;
typedef struct ExecCommand ExecCommand;
typedef struct ExecContext ExecContext;
typedef struct ExecRuntime ExecRuntime;
typedef struct ExecParameters ExecParameters;
typedef struct Manager Manager;

#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/capability.h>

#include "cgroup-util.h"
#include "cpu-set-util.h"
#include "fdset.h"
#include "list.h"
#include "missing.h"
#include "namespace.h"
#include "nsflags.h"

#define EXEC_STDIN_DATA_MAX (64U*1024U*1024U)

typedef enum ExecUtmpMode {
        EXEC_UTMP_INIT,
        EXEC_UTMP_LOGIN,
        EXEC_UTMP_USER,
        _EXEC_UTMP_MODE_MAX,
        _EXEC_UTMP_MODE_INVALID = -1
} ExecUtmpMode;

typedef enum ExecInput {
        EXEC_INPUT_NULL,
        EXEC_INPUT_TTY,
        EXEC_INPUT_TTY_FORCE,
        EXEC_INPUT_TTY_FAIL,
        EXEC_INPUT_SOCKET,
        EXEC_INPUT_NAMED_FD,
        EXEC_INPUT_DATA,
        EXEC_INPUT_FILE,
        _EXEC_INPUT_MAX,
        _EXEC_INPUT_INVALID = -1
} ExecInput;

typedef enum ExecOutput {
        EXEC_OUTPUT_INHERIT,
        EXEC_OUTPUT_NULL,
        EXEC_OUTPUT_TTY,
        EXEC_OUTPUT_SYSLOG,
        EXEC_OUTPUT_SYSLOG_AND_CONSOLE,
        EXEC_OUTPUT_KMSG,
        EXEC_OUTPUT_KMSG_AND_CONSOLE,
        EXEC_OUTPUT_JOURNAL,
        EXEC_OUTPUT_JOURNAL_AND_CONSOLE,
        EXEC_OUTPUT_SOCKET,
        EXEC_OUTPUT_NAMED_FD,
        EXEC_OUTPUT_FILE,
        EXEC_OUTPUT_FILE_APPEND,
        _EXEC_OUTPUT_MAX,
        _EXEC_OUTPUT_INVALID = -1
} ExecOutput;

typedef enum ExecPreserveMode {
        EXEC_PRESERVE_NO,
        EXEC_PRESERVE_YES,
        EXEC_PRESERVE_RESTART,
        _EXEC_PRESERVE_MODE_MAX,
        _EXEC_PRESERVE_MODE_INVALID = -1
} ExecPreserveMode;

typedef enum ExecKeyringMode {
        EXEC_KEYRING_INHERIT,
        EXEC_KEYRING_PRIVATE,
        EXEC_KEYRING_SHARED,
        _EXEC_KEYRING_MODE_MAX,
        _EXEC_KEYRING_MODE_INVALID = -1,
} ExecKeyringMode;

struct ExecStatus {
        dual_timestamp start_timestamp;
        dual_timestamp exit_timestamp;
        pid_t pid;
        int code;     /* as in siginfo_t::si_code */
        int status;   /* as in sigingo_t::si_status */
};

typedef enum ExecCommandFlags {
        EXEC_COMMAND_IGNORE_FAILURE   = 1 << 0,
        EXEC_COMMAND_FULLY_PRIVILEGED = 1 << 1,
        EXEC_COMMAND_NO_SETUID        = 1 << 2,
        EXEC_COMMAND_AMBIENT_MAGIC    = 1 << 3,
} ExecCommandFlags;

struct ExecCommand {
        char *path;
        char **argv;
        ExecStatus exec_status;
        ExecCommandFlags flags;
        LIST_FIELDS(ExecCommand, command); /* useful for chaining commands */
};

struct ExecRuntime {
        int n_ref;

        Manager *manager;

        /* unit id of the owner */
        char *id;

        char *tmp_dir;
        char *var_tmp_dir;

        /* An AF_UNIX socket pair, that contains a datagram containing a file descriptor referring to the network
         * namespace. */
        int netns_storage_socket[2];
};

typedef enum ExecDirectoryType {
        EXEC_DIRECTORY_RUNTIME = 0,
        EXEC_DIRECTORY_STATE,
        EXEC_DIRECTORY_CACHE,
        EXEC_DIRECTORY_LOGS,
        EXEC_DIRECTORY_CONFIGURATION,
        _EXEC_DIRECTORY_TYPE_MAX,
        _EXEC_DIRECTORY_TYPE_INVALID = -1,
} ExecDirectoryType;

typedef struct ExecDirectory {
        char **paths;
        mode_t mode;
} ExecDirectory;

struct ExecContext {
        char **environment;
        char **environment_files;
        char **pass_environment;
        char **unset_environment;

        struct rlimit *rlimit[_RLIMIT_MAX];
        char *working_directory, *root_directory, *root_image;
        bool working_directory_missing_ok;
        bool working_directory_home;

        mode_t umask;
        int oom_score_adjust;
        int nice;
        int ioprio;
        int cpu_sched_policy;
        int cpu_sched_priority;

        CPUSet cpu_set;
        NUMAPolicy numa_policy;
        bool cpu_affinity_from_numa;

        ExecInput std_input;
        ExecOutput std_output;
        ExecOutput std_error;
        char *stdio_fdname[3];
        char *stdio_file[3];

        void *stdin_data;
        size_t stdin_data_size;

        nsec_t timer_slack_nsec;

        bool stdio_as_fds;

        char *tty_path;

        bool tty_reset;
        bool tty_vhangup;
        bool tty_vt_disallocate;

        bool ignore_sigpipe;

        /* Since resolving these names might involve socket
         * connections and we don't want to deadlock ourselves these
         * names are resolved on execution only and in the child
         * process. */
        char *user;
        char *group;
        char **supplementary_groups;

        char *pam_name;

        char *utmp_id;
        ExecUtmpMode utmp_mode;

        bool selinux_context_ignore;
        char *selinux_context;

        bool apparmor_profile_ignore;
        char *apparmor_profile;

        bool smack_process_label_ignore;
        char *smack_process_label;

        ExecKeyringMode keyring_mode;

        char **read_write_paths, **read_only_paths, **inaccessible_paths;
        unsigned long mount_flags;
        BindMount *bind_mounts;
        size_t n_bind_mounts;
        TemporaryFileSystem *temporary_filesystems;
        size_t n_temporary_filesystems;

        uint64_t capability_bounding_set;
        uint64_t capability_ambient_set;
        int secure_bits;

        int syslog_priority;
        char *syslog_identifier;
        bool syslog_level_prefix;

        int log_level_max;

        struct iovec* log_extra_fields;
        size_t n_log_extra_fields;

        usec_t log_rate_limit_interval_usec;
        unsigned log_rate_limit_burst;

        bool cpu_sched_reset_on_fork;
        bool non_blocking;
        bool private_tmp;
        bool private_network;
        bool private_devices;
        bool private_users;
        bool private_mounts;
        ProtectSystem protect_system;
        ProtectHome protect_home;
        bool protect_kernel_tunables;
        bool protect_kernel_modules;
        bool protect_control_groups;
        bool mount_apivfs;

        bool no_new_privileges;

        bool dynamic_user;
        bool remove_ipc;

        /* This is not exposed to the user but available
         * internally. We need it to make sure that whenever we spawn
         * /usr/bin/mount it is run in the same process group as us so
         * that the autofs logic detects that it belongs to us and we
         * don't enter a trigger loop. */
        bool same_pgrp;
        bool restrict_suid_sgid;

        unsigned long personality;
        bool lock_personality;

        unsigned long restrict_namespaces; /* The CLONE_NEWxyz flags permitted to the unit's processes */

        Hashmap *syscall_filter;
        Set *syscall_archs;
        int syscall_errno;
        bool syscall_whitelist:1;

        Set *address_families;
        bool address_families_whitelist:1;

        ExecPreserveMode runtime_directory_preserve_mode;
        ExecDirectory directories[_EXEC_DIRECTORY_TYPE_MAX];

        bool memory_deny_write_execute;
        bool restrict_realtime;

        bool oom_score_adjust_set:1;
        bool nice_set:1;
        bool ioprio_set:1;
        bool cpu_sched_set:1;
};

static inline bool exec_context_restrict_namespaces_set(const ExecContext *c) {
        assert(c);

        return (c->restrict_namespaces & NAMESPACE_FLAGS_ALL) != NAMESPACE_FLAGS_ALL;
}

typedef enum ExecFlags {
        EXEC_APPLY_SANDBOXING  = 1 << 0,
        EXEC_APPLY_CHROOT      = 1 << 1,
        EXEC_APPLY_TTY_STDIN   = 1 << 2,
        EXEC_NEW_KEYRING       = 1 << 3,
        EXEC_PASS_LOG_UNIT     = 1 << 4, /* Whether to pass the unit name to the service's journal stream connection */
        EXEC_CHOWN_DIRECTORIES = 1 << 5, /* chown() the runtime/state/cache/log directories to the user we run as, under all conditions */
        EXEC_NSS_BYPASS_BUS    = 1 << 6, /* Set the SYSTEMD_NSS_BYPASS_BUS environment variable, to disable nss-systemd for dbus */
        EXEC_CGROUP_DELEGATE   = 1 << 7,
        EXEC_IS_CONTROL        = 1 << 8,
        EXEC_CONTROL_CGROUP    = 1 << 9, /* Place the process not in the indicated cgroup but in a subcgroup '/.control', but only EXEC_CGROUP_DELEGATE and EXEC_IS_CONTROL is set, too */

        /* The following are not used by execute.c, but by consumers internally */
        EXEC_PASS_FDS          = 1 << 10,
        EXEC_SETENV_RESULT     = 1 << 11,
        EXEC_SET_WATCHDOG      = 1 << 12,
} ExecFlags;

struct ExecParameters {
        char **argv;
        char **environment;

        int *fds;
        char **fd_names;
        size_t n_socket_fds;
        size_t n_storage_fds;

        ExecFlags flags;
        bool selinux_context_net:1;

        CGroupMask cgroup_supported;
        const char *cgroup_path;

        char **prefix;

        const char *confirm_spawn;

        usec_t watchdog_usec;

        int *idle_pipe;

        int stdin_fd;
        int stdout_fd;
        int stderr_fd;

        /* An fd that is closed by the execve(), and thus will result in EOF when the execve() is done */
        int exec_fd;
};

#include "unit.h"
#include "dynamic-user.h"

int exec_spawn(Unit *unit,
               ExecCommand *command,
               const ExecContext *context,
               const ExecParameters *exec_params,
               ExecRuntime *runtime,
               DynamicCreds *dynamic_creds,
               pid_t *ret);

void exec_command_done_array(ExecCommand *c, size_t n);

ExecCommand* exec_command_free_list(ExecCommand *c);
void exec_command_free_array(ExecCommand **c, size_t n);

void exec_command_dump_list(ExecCommand *c, FILE *f, const char *prefix);
void exec_command_append_list(ExecCommand **l, ExecCommand *e);
int exec_command_set(ExecCommand *c, const char *path, ...);
int exec_command_append(ExecCommand *c, const char *path, ...);

void exec_context_init(ExecContext *c);
void exec_context_done(ExecContext *c);
void exec_context_dump(const ExecContext *c, FILE* f, const char *prefix);

int exec_context_destroy_runtime_directory(const ExecContext *c, const char *runtime_root);

const char* exec_context_fdname(const ExecContext *c, int fd_index);

bool exec_context_may_touch_console(const ExecContext *c);
bool exec_context_maintains_privileges(const ExecContext *c);

int exec_context_get_effective_ioprio(const ExecContext *c);

void exec_context_free_log_extra_fields(ExecContext *c);

void exec_status_start(ExecStatus *s, pid_t pid);
void exec_status_exit(ExecStatus *s, const ExecContext *context, pid_t pid, int code, int status);
void exec_status_dump(const ExecStatus *s, FILE *f, const char *prefix);

int exec_runtime_acquire(Manager *m, const ExecContext *c, const char *name, bool create, ExecRuntime **ret);
ExecRuntime *exec_runtime_unref(ExecRuntime *r, bool destroy);

int exec_runtime_serialize(const Manager *m, FILE *f, FDSet *fds);
int exec_runtime_deserialize_compat(Unit *u, const char *key, const char *value, FDSet *fds);
void exec_runtime_deserialize_one(Manager *m, const char *value, FDSet *fds);
void exec_runtime_vacuum(Manager *m);

bool exec_context_get_cpu_affinity_from_numa(const ExecContext *c);

const char* exec_output_to_string(ExecOutput i) _const_;
ExecOutput exec_output_from_string(const char *s) _pure_;

const char* exec_input_to_string(ExecInput i) _const_;
ExecInput exec_input_from_string(const char *s) _pure_;

const char* exec_utmp_mode_to_string(ExecUtmpMode i) _const_;
ExecUtmpMode exec_utmp_mode_from_string(const char *s) _pure_;

const char* exec_preserve_mode_to_string(ExecPreserveMode i) _const_;
ExecPreserveMode exec_preserve_mode_from_string(const char *s) _pure_;

const char* exec_keyring_mode_to_string(ExecKeyringMode i) _const_;
ExecKeyringMode exec_keyring_mode_from_string(const char *s) _pure_;

const char* exec_directory_type_to_string(ExecDirectoryType i) _const_;
ExecDirectoryType exec_directory_type_from_string(const char *s) _pure_;
