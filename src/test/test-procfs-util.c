/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>

#include "log.h"
#include "parse-util.h"
#include "procfs-util.h"
#include "process-util.h"
#include "util.h"
#include "tests.h"

int main(int argc, char *argv[]) {
        char buf[CONST_MAX(FORMAT_TIMESPAN_MAX, FORMAT_BYTES_MAX)];
        nsec_t nsec;
        uint64_t v, w;
        int r;

        log_parse_environment();
        log_open();

        assert_se(procfs_cpu_get_usage(&nsec) >= 0);
        log_info("Current system CPU time: %s", format_timespan(buf, sizeof(buf), nsec/NSEC_PER_USEC, 1));

        assert_se(procfs_memory_get_used(&v) >= 0);
        log_info("Current memory usage: %s", format_bytes(buf, sizeof(buf), v));

        assert_se(procfs_tasks_get_current(&v) >= 0);
        log_info("Current number of tasks: %" PRIu64, v);

        v = TASKS_MAX;
        r = procfs_get_pid_max(&v);
        assert(r >= 0 || r == -ENOENT || ERRNO_IS_PRIVILEGE(r));
        log_info("kernel.pid_max: %"PRIu64, v);

        w = TASKS_MAX;
        r = procfs_get_threads_max(&w);
        assert(r >= 0 || r == -ENOENT || ERRNO_IS_PRIVILEGE(r));
        log_info("kernel.threads-max: %"PRIu64, w);

        v = MIN(v - (v > 0), w);

        assert_se(r >= 0);
        log_info("Limit of tasks: %" PRIu64, v);
        assert_se(v > 0);
        r = procfs_tasks_set_limit(v);
        if (r == -ENOENT || ERRNO_IS_PRIVILEGE(r)) {
                log_notice_errno(r, "Skipping test: can't set task limits");
                return EXIT_TEST_SKIP;
        }
        assert(r >= 0);

        if (v > 100) {
                log_info("Reducing limit by one to %"PRIu64"…", v-1);

                r = procfs_tasks_set_limit(v-1);
                if (IN_SET(r, -ENOENT, -EROFS) || ERRNO_IS_PRIVILEGE(r))
                        return log_tests_skipped_errno(r, "can't set tasks limit");
                assert_se(r >= 0);

                assert_se(procfs_get_threads_max(&w) >= 0);
                assert_se(r >= 0 ? w == v - 1 : w == v);

                assert_se(procfs_tasks_set_limit(v) >= 0);

                assert_se(procfs_get_threads_max(&w) >= 0);
                assert_se(v == w);
        }

        return 0;
}
