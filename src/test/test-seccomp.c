/* SPDX-License-Identifier: LGPL-2.1+ */

#include <poll.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "macro.h"
#include "missing.h"
#include "nsflags.h"
#include "process-util.h"
#include "raw-clone.h"
#include "rm-rf.h"
#include "seccomp-util.h"
#include "set.h"
#include "string-util.h"
#include "umask-util.h"
#include "util.h"
#include "virt.h"

#if SCMP_SYS(socket) < 0 || defined(__i386__) || defined(__s390x__) || defined(__s390__) || defined(__powerpc64__) || defined(__powerpc__)
/* On these archs, socket() is implemented via the socketcall() syscall multiplexer,
 * and we can't restrict it hence via seccomp. */
#  define SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN 1
#else
#  define SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN 0
#endif

static void test_seccomp_arch_to_string(void) {
        uint32_t a, b;
        const char *name;

        a = seccomp_arch_native();
        assert_se(a > 0);
        name = seccomp_arch_to_string(a);
        assert_se(name);
        assert_se(seccomp_arch_from_string(name, &b) >= 0);
        assert_se(a == b);
}

static void test_architecture_table(void) {
        const char *n, *n2;

        NULSTR_FOREACH(n,
                       "native\0"
                       "x86\0"
                       "x86-64\0"
                       "x32\0"
                       "arm\0"
                       "arm64\0"
                       "mips\0"
                       "mips64\0"
                       "mips64-n32\0"
                       "mips-le\0"
                       "mips64-le\0"
                       "mips64-le-n32\0"
                       "ppc\0"
                       "ppc64\0"
                       "ppc64-le\0"
                       "s390\0"
                       "s390x\0") {
                uint32_t c;

                assert_se(seccomp_arch_from_string(n, &c) >= 0);
                n2 = seccomp_arch_to_string(c);
                log_info("seccomp-arch: %s → 0x%"PRIx32" → %s", n, c, n2);
                assert_se(streq_ptr(n, n2));
        }
}

static void test_syscall_filter_set_find(void) {
        assert_se(!syscall_filter_set_find(NULL));
        assert_se(!syscall_filter_set_find(""));
        assert_se(!syscall_filter_set_find("quux"));
        assert_se(!syscall_filter_set_find("@quux"));

        assert_se(syscall_filter_set_find("@clock") == syscall_filter_sets + SYSCALL_FILTER_SET_CLOCK);
        assert_se(syscall_filter_set_find("@default") == syscall_filter_sets + SYSCALL_FILTER_SET_DEFAULT);
        assert_se(syscall_filter_set_find("@raw-io") == syscall_filter_sets + SYSCALL_FILTER_SET_RAW_IO);
}

static void test_filter_sets(void) {
        unsigned i;
        int r;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        for (i = 0; i < _SYSCALL_FILTER_SET_MAX; i++) {
                pid_t pid;

                log_info("Testing %s", syscall_filter_sets[i].name);

                pid = fork();
                assert_se(pid >= 0);

                if (pid == 0) { /* Child? */
                        int fd;

                        /* If we look at the default set (or one that includes it), allow-list instead of deny-list */
                        if (IN_SET(i, SYSCALL_FILTER_SET_DEFAULT,
                                      SYSCALL_FILTER_SET_SYSTEM_SERVICE,
                                      SYSCALL_FILTER_SET_KNOWN))
                                r = seccomp_load_syscall_filter_set(SCMP_ACT_ERRNO(EUCLEAN), syscall_filter_sets + i, SCMP_ACT_ALLOW, true);
                        else
                                r = seccomp_load_syscall_filter_set(SCMP_ACT_ALLOW, syscall_filter_sets + i, SCMP_ACT_ERRNO(EUCLEAN), true);
                        if (r < 0)
                                _exit(EXIT_FAILURE);

                        /* Test the sycall filter with one random system call */
                        fd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);
                        if (IN_SET(i, SYSCALL_FILTER_SET_IO_EVENT, SYSCALL_FILTER_SET_DEFAULT))
                                assert_se(fd < 0 && errno == EUCLEAN);
                        else {
                                assert_se(fd >= 0);
                                safe_close(fd);
                        }

                        _exit(EXIT_SUCCESS);
                }

                assert_se(wait_for_terminate_and_check(syscall_filter_sets[i].name, pid, WAIT_LOG) == EXIT_SUCCESS);
        }
}

static void test_restrict_namespace(void) {
        char *s = NULL;
        unsigned long ul;
        pid_t pid;

        assert_se(namespace_flags_to_string(0, &s) == 0 && streq(s, ""));
        s = mfree(s);
        assert_se(namespace_flags_to_string(CLONE_NEWNS, &s) == 0 && streq(s, "mnt"));
        s = mfree(s);
        assert_se(namespace_flags_to_string(CLONE_NEWNS|CLONE_NEWIPC, &s) == 0 && streq(s, "ipc mnt"));
        s = mfree(s);
        assert_se(namespace_flags_to_string(CLONE_NEWCGROUP, &s) == 0 && streq(s, "cgroup"));
        s = mfree(s);

        assert_se(namespace_flags_from_string("mnt", &ul) == 0 && ul == CLONE_NEWNS);
        assert_se(namespace_flags_from_string(NULL, &ul) == 0 && ul == 0);
        assert_se(namespace_flags_from_string("", &ul) == 0 && ul == 0);
        assert_se(namespace_flags_from_string("uts", &ul) == 0 && ul == CLONE_NEWUTS);
        assert_se(namespace_flags_from_string("mnt uts ipc", &ul) == 0 && ul == (CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWIPC));

        assert_se(namespace_flags_to_string(CLONE_NEWUTS, &s) == 0 && streq(s, "uts"));
        assert_se(namespace_flags_from_string(s, &ul) == 0 && ul == CLONE_NEWUTS);
        s = mfree(s);
        assert_se(namespace_flags_from_string("ipc", &ul) == 0 && ul == CLONE_NEWIPC);
        assert_se(namespace_flags_to_string(ul, &s) == 0 && streq(s, "ipc"));
        s = mfree(s);

        assert_se(namespace_flags_to_string(NAMESPACE_FLAGS_ALL, &s) == 0);
        assert_se(streq(s, "cgroup ipc net mnt pid user uts"));
        assert_se(namespace_flags_from_string(s, &ul) == 0 && ul == NAMESPACE_FLAGS_ALL);
        s = mfree(s);

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {

                assert_se(seccomp_restrict_namespaces(CLONE_NEWNS|CLONE_NEWNET) >= 0);

                assert_se(unshare(CLONE_NEWNS) == 0);
                assert_se(unshare(CLONE_NEWNET) == 0);
                assert_se(unshare(CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);
                assert_se(unshare(CLONE_NEWIPC) == -1);
                assert_se(errno == EPERM);
                assert_se(unshare(CLONE_NEWNET|CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);

                /* We use fd 0 (stdin) here, which of course will fail with EINVAL on setns(). Except of course our
                 * seccomp filter worked, and hits first and makes it return EPERM */
                assert_se(setns(0, CLONE_NEWNS) == -1);
                assert_se(errno == EINVAL);
                assert_se(setns(0, CLONE_NEWNET) == -1);
                assert_se(errno == EINVAL);
                assert_se(setns(0, CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);
                assert_se(setns(0, CLONE_NEWIPC) == -1);
                assert_se(errno == EPERM);
                assert_se(setns(0, CLONE_NEWNET|CLONE_NEWUTS) == -1);
                assert_se(errno == EPERM);
                assert_se(setns(0, 0) == -1);
                assert_se(errno == EPERM);

                pid = raw_clone(CLONE_NEWNS);
                assert_se(pid >= 0);
                if (pid == 0)
                        _exit(EXIT_SUCCESS);
                pid = raw_clone(CLONE_NEWNET);
                assert_se(pid >= 0);
                if (pid == 0)
                        _exit(EXIT_SUCCESS);
                pid = raw_clone(CLONE_NEWUTS);
                assert_se(pid < 0);
                assert_se(errno == EPERM);
                pid = raw_clone(CLONE_NEWIPC);
                assert_se(pid < 0);
                assert_se(errno == EPERM);
                pid = raw_clone(CLONE_NEWNET|CLONE_NEWUTS);
                assert_se(pid < 0);
                assert_se(errno == EPERM);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("nsseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_protect_sysctl(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        if (detect_container() > 0) /* in containers _sysctl() is likely missing anyway */
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
#if defined __NR__sysctl &&  __NR__sysctl >= 0
                assert_se(syscall(__NR__sysctl, NULL) < 0);
                assert_se(IN_SET(errno, EFAULT, ENOSYS));
#endif

                assert_se(seccomp_protect_sysctl() >= 0);

#if defined __NR__sysctl && __NR__sysctl >= 0
                assert_se(syscall(__NR__sysctl, 0, 0, 0) < 0);
                assert_se(errno == EPERM);
#endif

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("sysctlseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_restrict_address_families(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                int fd;
                Set *s;

                fd = socket(AF_INET, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_UNIX, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_NETLINK, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                assert_se(s = set_new(NULL));
                assert_se(set_put(s, INT_TO_PTR(AF_UNIX)) >= 0);

                assert_se(seccomp_restrict_address_families(s, false) >= 0);

                fd = socket(AF_INET, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_UNIX, SOCK_DGRAM, 0);
#if SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN
                assert_se(fd >= 0);
                safe_close(fd);
#else
                assert_se(fd < 0);
                assert_se(errno == EAFNOSUPPORT);
#endif

                fd = socket(AF_NETLINK, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                set_clear(s);

                assert_se(set_put(s, INT_TO_PTR(AF_INET)) >= 0);

                assert_se(seccomp_restrict_address_families(s, true) >= 0);

                fd = socket(AF_INET, SOCK_DGRAM, 0);
                assert_se(fd >= 0);
                safe_close(fd);

                fd = socket(AF_UNIX, SOCK_DGRAM, 0);
#if SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN
                assert_se(fd >= 0);
                safe_close(fd);
#else
                assert_se(fd < 0);
                assert_se(errno == EAFNOSUPPORT);
#endif

                fd = socket(AF_NETLINK, SOCK_DGRAM, 0);
#if SECCOMP_RESTRICT_ADDRESS_FAMILIES_BROKEN
                assert_se(fd >= 0);
                safe_close(fd);
#else
                assert_se(fd < 0);
                assert_se(errno == EAFNOSUPPORT);
#endif

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("socketseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_restrict_realtime(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        if (detect_container() > 0) /* in containers RT privs are likely missing anyway */
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                assert_se(sched_setscheduler(0, SCHED_FIFO, &(struct sched_param) { .sched_priority = 1 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_RR, &(struct sched_param) { .sched_priority = 1 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_IDLE, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_BATCH, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_OTHER, &(struct sched_param) {}) >= 0);

                assert_se(seccomp_restrict_realtime() >= 0);

                assert_se(sched_setscheduler(0, SCHED_IDLE, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_BATCH, &(struct sched_param) { .sched_priority = 0 }) >= 0);
                assert_se(sched_setscheduler(0, SCHED_OTHER, &(struct sched_param) {}) >= 0);

                assert_se(sched_setscheduler(0, SCHED_FIFO, &(struct sched_param) { .sched_priority = 1 }) < 0);
                assert_se(errno == EPERM);
                assert_se(sched_setscheduler(0, SCHED_RR, &(struct sched_param) { .sched_priority = 1 }) < 0);
                assert_se(errno == EPERM);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("realtimeseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_memory_deny_write_execute_mmap(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                void *p;

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);

                assert_se(seccomp_memory_deny_write_execute() >= 0);

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
#if defined(__x86_64__) || defined(__i386__) || defined(__powerpc64__) || defined(__arm__) || defined(__aarch64__)
                assert_se(p == MAP_FAILED);
                assert_se(errno == EPERM);
#else /* unknown architectures */
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);
#endif

                p = mmap(NULL, page_size(), PROT_WRITE|PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1,0);
                assert_se(p != MAP_FAILED);
                assert_se(munmap(p, page_size()) >= 0);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("memoryseccomp-mmap", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_memory_deny_write_execute_shmat(void) {
        int shmid;
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        shmid = shmget(IPC_PRIVATE, page_size(), 0);
        assert_se(shmid >= 0);

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                void *p;

                p = shmat(shmid, NULL, 0);
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);

                p = shmat(shmid, NULL, SHM_EXEC);
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);

                assert_se(seccomp_memory_deny_write_execute() >= 0);

                p = shmat(shmid, NULL, SHM_EXEC);
#if defined(__x86_64__) || defined(__arm__) || defined(__aarch64__)
                assert_se(p == MAP_FAILED);
                assert_se(errno == EPERM);
#else /* __i386__, __powerpc64__, and "unknown" architectures */
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);
#endif

                p = shmat(shmid, NULL, 0);
                assert_se(p != MAP_FAILED);
                assert_se(shmdt(p) == 0);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("memoryseccomp-shmat", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_restrict_archs(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                _cleanup_set_free_ Set *s = NULL;

                assert_se(access("/", F_OK) >= 0);

                assert_se(s = set_new(NULL));

#ifdef __x86_64__
                assert_se(set_put(s, UINT32_TO_PTR(SCMP_ARCH_X86+1)) >= 0);
#endif
                assert_se(seccomp_restrict_archs(s) >= 0);

                assert_se(access("/", F_OK) >= 0);
                assert_se(seccomp_restrict_archs(NULL) >= 0);

                assert_se(access("/", F_OK) >= 0);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("archseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_load_syscall_filter_set_raw(void) {
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                _cleanup_hashmap_free_ Hashmap *s = NULL;

                assert_se(access("/", F_OK) >= 0);
                assert_se(poll(NULL, 0, 0) == 0);

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, NULL, SCMP_ACT_KILL, true) >= 0);
                assert_se(access("/", F_OK) >= 0);
                assert_se(poll(NULL, 0, 0) == 0);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(access) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_access + 1), INT_TO_PTR(-1)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_faccessat + 1), INT_TO_PTR(-1)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUCLEAN), true) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EUCLEAN);

                assert_se(poll(NULL, 0, 0) == 0);

                s = hashmap_free(s);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(access) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_access + 1), INT_TO_PTR(EILSEQ)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_faccessat + 1), INT_TO_PTR(EILSEQ)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUCLEAN), true) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EILSEQ);

                assert_se(poll(NULL, 0, 0) == 0);

                s = hashmap_free(s);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(poll) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_poll + 1), INT_TO_PTR(-1)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_ppoll + 1), INT_TO_PTR(-1)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUNATCH), true) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EILSEQ);

                assert_se(poll(NULL, 0, 0) < 0);
                assert_se(errno == EUNATCH);

                s = hashmap_free(s);

                assert_se(s = hashmap_new(NULL));
#if SCMP_SYS(poll) >= 0
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_poll + 1), INT_TO_PTR(EILSEQ)) >= 0);
#else
                assert_se(hashmap_put(s, UINT32_TO_PTR(__NR_ppoll + 1), INT_TO_PTR(EILSEQ)) >= 0);
#endif

                assert_se(seccomp_load_syscall_filter_set_raw(SCMP_ACT_ALLOW, s, SCMP_ACT_ERRNO(EUNATCH), true) >= 0);

                assert_se(access("/", F_OK) < 0);
                assert_se(errno == EILSEQ);

                assert_se(poll(NULL, 0, 0) < 0);
                assert_se(errno == EILSEQ);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("syscallrawseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_lock_personality(void) {
        unsigned long current;
        pid_t pid;

        if (!is_seccomp_available())
                return;
        if (geteuid() != 0)
                return;

        assert_se(opinionated_personality(&current) >= 0);

        log_info("current personality=%lu", current);

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                assert_se(seccomp_lock_personality(current) >= 0);

                assert_se((unsigned long) safe_personality(current) == current);

                /* Note, we also test that safe_personality() works correctly, by checkig whether errno is properly
                 * set, in addition to the return value */
                errno = 0;
                assert_se(safe_personality(PER_LINUX | ADDR_NO_RANDOMIZE) == -EPERM);
                assert_se(errno == EPERM);

                assert_se(safe_personality(PER_LINUX | MMAP_PAGE_ZERO) == -EPERM);
                assert_se(safe_personality(PER_LINUX | ADDR_COMPAT_LAYOUT) == -EPERM);
                assert_se(safe_personality(PER_LINUX | READ_IMPLIES_EXEC) == -EPERM);
                assert_se(safe_personality(PER_LINUX_32BIT) == -EPERM);
                assert_se(safe_personality(PER_SVR4) == -EPERM);
                assert_se(safe_personality(PER_BSD) == -EPERM);
                assert_se(safe_personality(current == PER_LINUX ? PER_LINUX32 : PER_LINUX) == -EPERM);
                assert_se(safe_personality(PER_LINUX32_3GB) == -EPERM);
                assert_se(safe_personality(PER_UW7) == -EPERM);
                assert_se(safe_personality(0x42) == -EPERM);

                assert_se(safe_personality(PERSONALITY_INVALID) == -EPERM); /* maybe remove this later */

                assert_se((unsigned long) personality(current) == current);
                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("lockpersonalityseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

static void test_filter_sets_ordered(void) {
        /* Ensure "@default" always remains at the beginning of the list */
        assert_se(SYSCALL_FILTER_SET_DEFAULT == 0);
        assert_se(streq(syscall_filter_sets[0].name, "@default"));

        /* Ensure "@known" always remains at the end of the list */
        assert_se(SYSCALL_FILTER_SET_KNOWN == _SYSCALL_FILTER_SET_MAX - 1);
        assert_se(streq(syscall_filter_sets[SYSCALL_FILTER_SET_KNOWN].name, "@known"));

        for (size_t i = 0; i < _SYSCALL_FILTER_SET_MAX; i++) {
                const char *k, *p = NULL;

                /* Make sure each group has a description */
                assert_se(!isempty(syscall_filter_sets[0].help));

                /* Make sure the groups are ordered alphabetically, except for the first and last entries */
                assert_se(i < 2 || i == _SYSCALL_FILTER_SET_MAX - 1 ||
                          strcmp(syscall_filter_sets[i-1].name, syscall_filter_sets[i].name) < 0);

                NULSTR_FOREACH(k, syscall_filter_sets[i].value) {

                        /* Ensure each syscall list is in itself ordered, but groups before names */
                        assert_se(!p ||
                                  (*p == '@' && *k != '@') ||
                                  (((*p == '@' && *k == '@') ||
                                    (*p != '@' && *k != '@')) &&
                                   strcmp(p, k) < 0));

                        p = k;
                }
        }
}

static int mkostemp_safe(char *pattern) {
        _unused_ _cleanup_umask_ mode_t u = umask(0077);
        int fd;

        assert(pattern);

        fd = mkostemp(pattern, O_CLOEXEC);
        if (fd < 0)
                return -errno;

        return fd;
}

static int real_open(const char *path, int flags, mode_t mode) {
        /* glibc internally calls openat() when open() is requested. Let's hence define our own wrapper for
         * testing purposes that calls the real syscall, on architectures where SYS_open is defined. On
         * other architectures, let's just fall back to the glibc call. */

#ifdef SYS_open
        return (int) syscall(SYS_open, path, flags, mode);
#else
        return open(path, flags, mode);
#endif
}

static void test_restrict_suid_sgid(void) {
        pid_t pid;

        log_info("/* %s */", __func__);

        if (!is_seccomp_available()) {
                log_notice("Seccomp not available, skipping %s", __func__);
                return;
        }
        if (geteuid() != 0) {
                log_notice("Not root, skipping %s", __func__);
                return;
        }

        pid = fork();
        assert_se(pid >= 0);

        if (pid == 0) {
                char path[] = "/tmp/suidsgidXXXXXX", dir[] = "/tmp/suidsgiddirXXXXXX";
                int fd = -1, k = -1;
                const char *z;

                fd = mkostemp_safe(path);
                assert_se(fd >= 0);

                assert_se(mkdtemp(dir));
                z = strjoina(dir, "/test");

                assert_se(chmod(path, 0755 | S_ISUID) >= 0);
                assert_se(chmod(path, 0755 | S_ISGID) >= 0);
                assert_se(chmod(path, 0755 | S_ISGID | S_ISUID) >= 0);
                assert_se(chmod(path, 0755) >= 0);

                assert_se(fchmod(fd, 0755 | S_ISUID) >= 0);
                assert_se(fchmod(fd, 0755 | S_ISGID) >= 0);
                assert_se(fchmod(fd, 0755 | S_ISGID | S_ISUID) >= 0);
                assert_se(fchmod(fd, 0755) >= 0);

                assert_se(fchmodat(AT_FDCWD, path, 0755 | S_ISUID, 0) >= 0);
                assert_se(fchmodat(AT_FDCWD, path, 0755 | S_ISGID, 0) >= 0);
                assert_se(fchmodat(AT_FDCWD, path, 0755 | S_ISGID | S_ISUID, 0) >= 0);
                assert_se(fchmodat(AT_FDCWD, path, 0755, 0) >= 0);

                k = real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISGID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID | S_ISGID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = creat(z, 0644 | S_ISUID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = creat(z, 0644 | S_ISGID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = creat(z, 0644 | S_ISUID | S_ISGID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = creat(z, 0644);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISGID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID | S_ISGID);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                k = openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                assert_se(mkdir(z, 0755 | S_ISUID) >= 0);
                assert_se(rmdir(z) >= 0);
                assert_se(mkdir(z, 0755 | S_ISGID) >= 0);
                assert_se(rmdir(z) >= 0);
                assert_se(mkdir(z, 0755 | S_ISUID | S_ISGID) >= 0);
                assert_se(rmdir(z) >= 0);
                assert_se(mkdir(z, 0755) >= 0);
                assert_se(rmdir(z) >= 0);

                assert_se(mkdirat(AT_FDCWD, z, 0755 | S_ISUID) >= 0);
                assert_se(rmdir(z) >= 0);
                assert_se(mkdirat(AT_FDCWD, z, 0755 | S_ISGID) >= 0);
                assert_se(rmdir(z) >= 0);
                assert_se(mkdirat(AT_FDCWD, z, 0755 | S_ISUID | S_ISGID) >= 0);
                assert_se(rmdir(z) >= 0);
                assert_se(mkdirat(AT_FDCWD, z, 0755) >= 0);
                assert_se(rmdir(z) >= 0);

                assert_se(mknod(z, S_IFREG | 0755 | S_ISUID, 0) >= 0);
                assert_se(unlink(z) >= 0);
                assert_se(mknod(z, S_IFREG | 0755 | S_ISGID, 0) >= 0);
                assert_se(unlink(z) >= 0);
                assert_se(mknod(z, S_IFREG | 0755 | S_ISUID | S_ISGID, 0) >= 0);
                assert_se(unlink(z) >= 0);
                assert_se(mknod(z, S_IFREG | 0755, 0) >= 0);
                assert_se(unlink(z) >= 0);

                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755 | S_ISUID, 0) >= 0);
                assert_se(unlink(z) >= 0);
                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755 | S_ISGID, 0) >= 0);
                assert_se(unlink(z) >= 0);
                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755 | S_ISUID | S_ISGID, 0) >= 0);
                assert_se(unlink(z) >= 0);
                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755, 0) >= 0);
                assert_se(unlink(z) >= 0);

                assert_se(seccomp_restrict_suid_sgid() >= 0);

                assert_se(chmod(path, 0775 | S_ISUID) < 0 && errno == EPERM);
                assert_se(chmod(path, 0775 | S_ISGID) < 0  && errno == EPERM);
                assert_se(chmod(path, 0775 | S_ISGID | S_ISUID) < 0  && errno == EPERM);
                assert_se(chmod(path, 0775) >= 0);

                assert_se(fchmod(fd, 0775 | S_ISUID) < 0 && errno == EPERM);
                assert_se(fchmod(fd, 0775 | S_ISGID) < 0  && errno == EPERM);
                assert_se(fchmod(fd, 0775 | S_ISGID | S_ISUID) < 0  && errno == EPERM);
                assert_se(fchmod(fd, 0775) >= 0);

                assert_se(fchmodat(AT_FDCWD, path, 0755 | S_ISUID, 0) < 0 && errno == EPERM);
                assert_se(fchmodat(AT_FDCWD, path, 0755 | S_ISGID, 0) < 0 && errno == EPERM);
                assert_se(fchmodat(AT_FDCWD, path, 0755 | S_ISGID | S_ISUID, 0) < 0 && errno == EPERM);
                assert_se(fchmodat(AT_FDCWD, path, 0755, 0) >= 0);

                assert_se(real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID) < 0 && errno == EPERM);
                assert_se(real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISGID) < 0 && errno == EPERM);
                assert_se(real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID | S_ISGID) < 0 && errno == EPERM);
                k = real_open(z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                assert_se(creat(z, 0644 | S_ISUID) < 0 && errno == EPERM);
                assert_se(creat(z, 0644 | S_ISGID) < 0 && errno == EPERM);
                assert_se(creat(z, 0644 | S_ISUID | S_ISGID) < 0 && errno == EPERM);
                k = creat(z, 0644);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                assert_se(openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID) < 0 && errno == EPERM);
                assert_se(openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISGID) < 0 && errno == EPERM);
                assert_se(openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644 | S_ISUID | S_ISGID) < 0 && errno == EPERM);
                k = openat(AT_FDCWD, z, O_CREAT|O_RDWR|O_CLOEXEC|O_EXCL, 0644);
                k = safe_close(k);
                assert_se(unlink(z) >= 0);

                assert_se(mkdir(z, 0755 | S_ISUID) < 0 && errno == EPERM);
                assert_se(mkdir(z, 0755 | S_ISGID) < 0 && errno == EPERM);
                assert_se(mkdir(z, 0755 | S_ISUID | S_ISGID) < 0 && errno == EPERM);
                assert_se(mkdir(z, 0755) >= 0);
                assert_se(rmdir(z) >= 0);

                assert_se(mkdirat(AT_FDCWD, z, 0755 | S_ISUID) < 0 && errno == EPERM);
                assert_se(mkdirat(AT_FDCWD, z, 0755 | S_ISGID) < 0 && errno == EPERM);
                assert_se(mkdirat(AT_FDCWD, z, 0755 | S_ISUID | S_ISGID) < 0 && errno == EPERM);
                assert_se(mkdirat(AT_FDCWD, z, 0755) >= 0);
                assert_se(rmdir(z) >= 0);

                assert_se(mknod(z, S_IFREG | 0755 | S_ISUID, 0) < 0 && errno == EPERM);
                assert_se(mknod(z, S_IFREG | 0755 | S_ISGID, 0) < 0 && errno == EPERM);
                assert_se(mknod(z, S_IFREG | 0755 | S_ISUID | S_ISGID, 0) < 0 && errno == EPERM);
                assert_se(mknod(z, S_IFREG | 0755, 0) >= 0);
                assert_se(unlink(z) >= 0);

                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755 | S_ISUID, 0) < 0 && errno == EPERM);
                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755 | S_ISGID, 0) < 0 && errno == EPERM);
                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755 | S_ISUID | S_ISGID, 0) < 0 && errno == EPERM);
                assert_se(mknodat(AT_FDCWD, z, S_IFREG | 0755, 0) >= 0);
                assert_se(unlink(z) >= 0);

                assert_se(unlink(path) >= 0);
                assert_se(rm_rf(dir, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);

                _exit(EXIT_SUCCESS);
        }

        assert_se(wait_for_terminate_and_check("suidsgidseccomp", pid, WAIT_LOG) == EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {

        log_set_max_level(LOG_DEBUG);

        test_seccomp_arch_to_string();
        test_architecture_table();
        test_syscall_filter_set_find();
        test_filter_sets();
        test_restrict_namespace();
        test_protect_sysctl();
        test_restrict_address_families();
        test_restrict_realtime();
        test_memory_deny_write_execute_mmap();
        test_memory_deny_write_execute_shmat();
        test_restrict_archs();
        test_load_syscall_filter_set_raw();
        test_lock_personality();
        test_filter_sets_ordered();
        test_restrict_suid_sgid();

        return 0;
}
