/* SPDX-License-Identifier: LGPL-2.1+ */

#include <alloc-util.h>
#include <fs-util.h>
#include <libgen.h>
#include <stdlib.h>
#include <util.h>

#include "tests.h"
#include "env-util.h"
#include "path-util.h"
#include "strv.h"

char* setup_fake_runtime_dir(void) {
        char t[] = "/tmp/fake-xdg-runtime-XXXXXX", *p;

        assert_se(mkdtemp(t));
        assert_se(setenv("XDG_RUNTIME_DIR", t, 1) >= 0);
        assert_se(p = strdup(t));

        return p;
}

bool test_is_running_from_builddir(char **exedir) {
        _cleanup_free_ char *s = NULL;
        bool r;

        /* Check if we're running from the builddir. Optionally, this returns
         * the path to the directory where the binary is located. */

        assert_se(readlink_and_make_absolute("/proc/self/exe", &s) >= 0);
        r = path_startswith(s, ABS_BUILD_DIR);

        if (exedir) {
                dirname(s);
                *exedir = TAKE_PTR(s);
        }

        return r;
}

const char* get_testdata_dir(void) {
        const char *env;
        /* convenience: caller does not need to free result */
        static char testdir[PATH_MAX];

        /* if the env var is set, use that */
        env = getenv("SYSTEMD_TEST_DATA");
        testdir[sizeof(testdir) - 1] = '\0';
        if (env) {
                if (access(env, F_OK) < 0) {
                        fputs("ERROR: $SYSTEMD_TEST_DATA directory does not exist\n", stderr);
                        exit(EXIT_FAILURE);
                }
                strncpy(testdir, env, sizeof(testdir) - 1);
        } else {
                _cleanup_free_ char *exedir = NULL;

                /* Check if we're running from the builddir. If so, use the compiled in path. */
                if (test_is_running_from_builddir(&exedir))
                        assert_se(snprintf(testdir, sizeof(testdir), "%s/test", ABS_SRC_DIR) > 0);
                else
                        /* Try relative path, according to the install-test layout */
                        assert_se(snprintf(testdir, sizeof(testdir), "%s/testdata", exedir) > 0);

                if (access(testdir, F_OK) < 0) {
                        fputs("ERROR: Cannot find testdata directory, set $SYSTEMD_TEST_DATA\n", stderr);
                        exit(EXIT_FAILURE);
                }
        }

        return testdir;
}

void test_setup_logging(int level) {
        log_set_max_level(level);
        log_parse_environment();
        log_open();
}

int log_tests_skipped(const char *message) {
        log_notice("%s: %s, skipping tests.",
                   program_invocation_short_name, message);
        return EXIT_TEST_SKIP;
}

int log_tests_skipped_errno(int r, const char *message) {
        log_notice_errno(r, "%s: %s, skipping tests: %m",
                         program_invocation_short_name, message);
        return EXIT_TEST_SKIP;
}

const char *ci_environment(void) {
        /* We return a string because we might want to provide multiple bits of information later on: not
         * just the general CI environment type, but also whether we're sanitizing or not, etc. The caller is
         * expected to use strstr on the returned value. */
        static const char *ans = (void*) UINTPTR_MAX;
        const char *p;
        int r;

        if (ans != (void*) UINTPTR_MAX)
                return ans;

        /* We allow specifying the environment with $CITYPE. Nobody uses this so far, but we are ready. */
        p = getenv("CITYPE");
        if (!isempty(p))
                return (ans = p);

        if (getenv_bool("TRAVIS") > 0)
                return (ans = "travis");
        if (getenv_bool("SEMAPHORE") > 0)
                return (ans = "semaphore");
        if (getenv_bool("GITHUB_ACTIONS") > 0)
                return (ans = "github-actions");
        if (getenv("AUTOPKGTEST_ARTIFACTS") || getenv("AUTOPKGTEST_TMP"))
                return (ans = "autopkgtest");

        FOREACH_STRING(p, "CI", "CONTINOUS_INTEGRATION") {
                /* Those vars are booleans according to Semaphore and Travis docs:
                 * https://docs.travis-ci.com/user/environment-variables/#default-environment-variables
                 * https://docs.semaphoreci.com/ci-cd-environment/environment-variables/#ci
                 */
                r = getenv_bool(p);
                if (r > 0)
                        return (ans = "unknown"); /* Some other unknown thing */
                if (r == 0)
                        return (ans = NULL);
        }

        return (ans = NULL);
}
