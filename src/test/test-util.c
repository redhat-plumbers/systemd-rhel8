/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "def.h"
#include "fileio.h"
#include "fs-util.h"
#include "parse-util.h"
#include "process-util.h"
#include "raw-clone.h"
#include "rm-rf.h"
#include "stdio-util.h"
#include "string-util.h"
#include "util.h"

static void test_align_power2(void) {
        unsigned long i, p2;

        assert_se(ALIGN_POWER2(0) == 0);
        assert_se(ALIGN_POWER2(1) == 1);
        assert_se(ALIGN_POWER2(2) == 2);
        assert_se(ALIGN_POWER2(3) == 4);
        assert_se(ALIGN_POWER2(12) == 16);

        assert_se(ALIGN_POWER2(ULONG_MAX) == 0);
        assert_se(ALIGN_POWER2(ULONG_MAX - 1) == 0);
        assert_se(ALIGN_POWER2(ULONG_MAX - 1024) == 0);
        assert_se(ALIGN_POWER2(ULONG_MAX / 2) == ULONG_MAX / 2 + 1);
        assert_se(ALIGN_POWER2(ULONG_MAX + 1) == 0);

        for (i = 1; i < 131071; ++i) {
                for (p2 = 1; p2 < i; p2 <<= 1)
                        /* empty */ ;

                assert_se(ALIGN_POWER2(i) == p2);
        }

        for (i = ULONG_MAX - 1024; i < ULONG_MAX; ++i) {
                for (p2 = 1; p2 && p2 < i; p2 <<= 1)
                        /* empty */ ;

                assert_se(ALIGN_POWER2(i) == p2);
        }
}

static void test_max(void) {
        static const struct {
                int a;
                int b[CONST_MAX(10, 100)];
        } val1 = {
                .a = CONST_MAX(10, 100),
        };
        int d = 0;

        assert_cc(sizeof(val1.b) == sizeof(int) * 100);

        /* CONST_MAX returns (void) instead of a value if the passed arguments
         * are not of the same type or not constant expressions. */
        assert_cc(__builtin_types_compatible_p(typeof(CONST_MAX(1, 10)), int));
        assert_cc(__builtin_types_compatible_p(typeof(CONST_MAX(1, 1U)), void));

        assert_se(val1.a == 100);
        assert_se(MAX(++d, 0) == 1);
        assert_se(d == 1);

        assert_cc(MAXSIZE(char[3], uint16_t) == 3);
        assert_cc(MAXSIZE(char[3], uint32_t) == 4);
        assert_cc(MAXSIZE(char, long) == sizeof(long));

        assert_se(MAX(-5, 5) == 5);
        assert_se(MAX(5, 5) == 5);
        assert_se(MAX(MAX(1, MAX(2, MAX(3, 4))), 5) == 5);
        assert_se(MAX(MAX(1, MAX(2, MAX(3, 2))), 1) == 3);
        assert_se(MAX(MIN(1, MIN(2, MIN(3, 4))), 5) == 5);
        assert_se(MAX(MAX(1, MIN(2, MIN(3, 2))), 1) == 2);
        assert_se(LESS_BY(8, 4) == 4);
        assert_se(LESS_BY(8, 8) == 0);
        assert_se(LESS_BY(4, 8) == 0);
        assert_se(LESS_BY(16, LESS_BY(8, 4)) == 12);
        assert_se(LESS_BY(4, LESS_BY(8, 4)) == 0);
        assert_se(CLAMP(-5, 0, 1) == 0);
        assert_se(CLAMP(5, 0, 1) == 1);
        assert_se(CLAMP(5, -10, 1) == 1);
        assert_se(CLAMP(5, -10, 10) == 5);
        assert_se(CLAMP(CLAMP(0, -10, 10), CLAMP(-5, 10, 20), CLAMP(100, -5, 20)) == 10);
}

#pragma GCC diagnostic push
#ifdef __clang__
#  pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

static void test_container_of(void) {
        struct mytype {
                uint8_t pad1[3];
                uint64_t v1;
                uint8_t pad2[2];
                uint32_t v2;
        } _packed_ myval = { };

        assert_cc(sizeof(myval) == 17);
        assert_se(container_of(&myval.v1, struct mytype, v1) == &myval);
        assert_se(container_of(&myval.v2, struct mytype, v2) == &myval);
        assert_se(container_of(&container_of(&myval.v2,
                                             struct mytype,
                                             v2)->v1,
                               struct mytype,
                               v1) == &myval);
}

#pragma GCC diagnostic pop

static void test_div_round_up(void) {
        int div;

        /* basic tests */
        assert_se(DIV_ROUND_UP(0, 8) == 0);
        assert_se(DIV_ROUND_UP(1, 8) == 1);
        assert_se(DIV_ROUND_UP(8, 8) == 1);
        assert_se(DIV_ROUND_UP(12, 8) == 2);
        assert_se(DIV_ROUND_UP(16, 8) == 2);

        /* test multiple evaluation */
        div = 0;
        assert_se(DIV_ROUND_UP(div++, 8) == 0 && div == 1);
        assert_se(DIV_ROUND_UP(++div, 8) == 1 && div == 2);
        assert_se(DIV_ROUND_UP(8, div++) == 4 && div == 3);
        assert_se(DIV_ROUND_UP(8, ++div) == 2 && div == 4);

        /* overflow test with exact division */
        assert_se(sizeof(0U) == 4);
        assert_se(0xfffffffaU % 10U == 0U);
        assert_se(0xfffffffaU / 10U == 429496729U);
        assert_se(DIV_ROUND_UP(0xfffffffaU, 10U) == 429496729U);
        assert_se((0xfffffffaU + 10U - 1U) / 10U == 0U);
        assert_se(0xfffffffaU / 10U + !!(0xfffffffaU % 10U) == 429496729U);

        /* overflow test with rounded division */
        assert_se(0xfffffffdU % 10U == 3U);
        assert_se(0xfffffffdU / 10U == 429496729U);
        assert_se(DIV_ROUND_UP(0xfffffffdU, 10U) == 429496730U);
        assert_se((0xfffffffdU + 10U - 1U) / 10U == 0U);
        assert_se(0xfffffffdU / 10U + !!(0xfffffffdU % 10U) == 429496730U);
}

static void test_u64log2(void) {
        assert_se(u64log2(0) == 0);
        assert_se(u64log2(8) == 3);
        assert_se(u64log2(9) == 3);
        assert_se(u64log2(15) == 3);
        assert_se(u64log2(16) == 4);
        assert_se(u64log2(1024*1024) == 20);
        assert_se(u64log2(1024*1024+5) == 20);
}

static void test_protect_errno(void) {
        errno = 12;
        {
                PROTECT_ERRNO;
                errno = 11;
        }
        assert_se(errno == 12);
}

static void test_unprotect_errno_inner_function(void) {
        PROTECT_ERRNO;

        errno = 2222;
}

static void test_unprotect_errno(void) {
        log_info("/* %s */", __func__);

        errno = 4711;

        PROTECT_ERRNO;

        errno = 815;

        UNPROTECT_ERRNO;

        assert_se(errno == 4711);

        test_unprotect_errno_inner_function();

        assert_se(errno == 4711);
}

static void test_in_set(void) {
        assert_se(IN_SET(1, 1));
        assert_se(IN_SET(1, 1, 2, 3, 4));
        assert_se(IN_SET(2, 1, 2, 3, 4));
        assert_se(IN_SET(3, 1, 2, 3, 4));
        assert_se(IN_SET(4, 1, 2, 3, 4));
        assert_se(!IN_SET(0, 1));
        assert_se(!IN_SET(0, 1, 2, 3, 4));
}

static void test_log2i(void) {
        assert_se(log2i(1) == 0);
        assert_se(log2i(2) == 1);
        assert_se(log2i(3) == 1);
        assert_se(log2i(4) == 2);
        assert_se(log2i(32) == 5);
        assert_se(log2i(33) == 5);
        assert_se(log2i(63) == 5);
        assert_se(log2i(INT_MAX) == sizeof(int)*8-2);
}

static void test_raw_clone(void) {
        pid_t parent, pid, pid2;

        parent = getpid();
        log_info("before clone: getpid()→"PID_FMT, parent);
        assert_se(raw_getpid() == parent);

        pid = raw_clone(0);
        assert_se(pid >= 0);

        pid2 = raw_getpid();
        log_info("raw_clone: "PID_FMT" getpid()→"PID_FMT" raw_getpid()→"PID_FMT,
                 pid, getpid(), pid2);
        if (pid == 0) {
                assert_se(pid2 != parent);
                _exit(EXIT_SUCCESS);
        } else {
                int status;

                assert_se(pid2 == parent);
                waitpid(pid, &status, __WCLONE);
                assert_se(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
        }

        errno = 0;
        assert_se(raw_clone(CLONE_FS|CLONE_NEWNS) == -1);
        assert_se(errno == EINVAL);
}

static void test_physical_memory(void) {
        uint64_t p;
        char buf[FORMAT_BYTES_MAX];

        p = physical_memory();
        assert_se(p > 0);
        assert_se(p < UINT64_MAX);
        assert_se(p % page_size() == 0);

        log_info("Memory: %s (%" PRIu64 ")", format_bytes(buf, sizeof(buf), p), p);
}

static void test_physical_memory_scale(void) {
        uint64_t p;

        p = physical_memory();

        assert_se(physical_memory_scale(0, 100) == 0);
        assert_se(physical_memory_scale(100, 100) == p);

        log_info("Memory original: %" PRIu64, physical_memory());
        log_info("Memory scaled by 50%%: %" PRIu64, physical_memory_scale(50, 100));
        log_info("Memory divided by 2: %" PRIu64, physical_memory() / 2);
        log_info("Page size: %zu", page_size());

        /* There might be an uneven number of pages, hence permit these calculations to be half a page off... */
        assert_se(page_size()/2 + physical_memory_scale(50, 100) - p/2 <= page_size());
        assert_se(physical_memory_scale(200, 100) == p*2);

        assert_se(physical_memory_scale(0, 1) == 0);
        assert_se(physical_memory_scale(1, 1) == p);
        assert_se(physical_memory_scale(2, 1) == p*2);

        assert_se(physical_memory_scale(0, 2) == 0);

        assert_se(page_size()/2 + physical_memory_scale(1, 2) - p/2 <= page_size());
        assert_se(physical_memory_scale(2, 2) == p);
        assert_se(physical_memory_scale(4, 2) == p*2);

        assert_se(physical_memory_scale(0, UINT32_MAX) == 0);
        assert_se(physical_memory_scale(UINT32_MAX, UINT32_MAX) == p);

        /* overflow */
        assert_se(physical_memory_scale(UINT64_MAX/4, UINT64_MAX) == UINT64_MAX);
}

static void test_system_tasks_max(void) {
        uint64_t t;

        t = system_tasks_max();
        assert_se(t > 0);
        assert_se(t < UINT64_MAX);

        log_info("Max tasks: %" PRIu64, t);
}

static void test_system_tasks_max_scale(void) {
        uint64_t t;

        t = system_tasks_max();

        assert_se(system_tasks_max_scale(0, 100) == 0);
        assert_se(system_tasks_max_scale(100, 100) == t);

        assert_se(system_tasks_max_scale(0, 1) == 0);
        assert_se(system_tasks_max_scale(1, 1) == t);
        assert_se(system_tasks_max_scale(2, 1) == 2*t);

        assert_se(system_tasks_max_scale(0, 2) == 0);
        assert_se(system_tasks_max_scale(1, 2) == t/2);
        assert_se(system_tasks_max_scale(2, 2) == t);
        assert_se(system_tasks_max_scale(3, 2) == (3*t)/2);
        assert_se(system_tasks_max_scale(4, 2) == t*2);

        assert_se(system_tasks_max_scale(0, UINT32_MAX) == 0);
        assert_se(system_tasks_max_scale((UINT32_MAX-1)/2, UINT32_MAX-1) == t/2);
        assert_se(system_tasks_max_scale(UINT32_MAX, UINT32_MAX) == t);

        /* overflow */

        assert_se(system_tasks_max_scale(UINT64_MAX/4, UINT64_MAX) == UINT64_MAX);
}

static void test_strerror_not_threadsafe(void) {
        /* Just check that strerror really is not thread-safe. */
        log_info("strerror(%d) → %s", 200, strerror(200));
        log_info("strerror(%d) → %s", 201, strerror(201));
        log_info("strerror(%d) → %s", INT_MAX, strerror(INT_MAX));

        log_info("strerror(%d), strerror(%d) → %p, %p", 200, 201, strerror(200), strerror(201));

        /* This call is not allowed, because the first returned string becomes invalid when
         * we call strerror the second time:
         *
         * log_info("strerror(%d), strerror(%d) → %s, %s", 200, 201, strerror(200), strerror(201));
         */
}

static void test_STRERROR(void) {
        /* Just check that STRERROR really is thread-safe. */
        log_info("STRERROR(%d) → %s", 200, STRERROR(200));
        log_info("STRERROR(%d) → %s", 201, STRERROR(201));
        log_info("STRERROR(%d), STRERROR(%d) → %s, %s", 200, 201, STRERROR(200), STRERROR(201));

        const char *a = STRERROR(200), *b = STRERROR(201);
        assert_se(strstr(a, "200"));
        assert_se(strstr(b, "201"));

        /* Check with negative values */
        assert_se(streq(a, STRERROR(-200)));
        assert_se(streq(b, STRERROR(-201)));

        const char *c = STRERROR(INT_MAX);
        char buf[DECIMAL_STR_MAX(int)];
        xsprintf(buf, "%d", INT_MAX);  /* INT_MAX is hexadecimal, use printf to convert to decimal */
        log_info("STRERROR(%d) → %s", INT_MAX, c);
        assert_se(strstr(c, buf));
}

int main(int argc, char *argv[]) {
        log_parse_environment();
        log_open();

        test_align_power2();
        test_max();
        test_container_of();
        test_div_round_up();
        test_u64log2();
        test_protect_errno();
        test_unprotect_errno();
        test_in_set();
        test_log2i();
        test_raw_clone();
        test_physical_memory();
        test_physical_memory_scale();
        test_system_tasks_max();
        test_system_tasks_max_scale();

        test_strerror_not_threadsafe();
        test_STRERROR();

        return 0;
}
