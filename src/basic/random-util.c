/* SPDX-License-Identifier: LGPL-2.1+ */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/random.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#if HAVE_SYS_AUXV_H
#  include <sys/auxv.h>
#endif

#if USE_SYS_RANDOM_H
#  include <sys/random.h>
#else
#  include <linux/random.h>
#endif

#include "fd-util.h"
#include "io-util.h"
#include "missing.h"
#include "random-util.h"
#include "time-util.h"

static bool srand_called = false;

int acquire_random_bytes(void *p, size_t n, bool high_quality_required) {
        static int have_syscall = -1;

        _cleanup_close_ int fd = -1;
        size_t already_done = 0;
        int r;

        /* Gathers some randomness from the kernel. This call will never block. If
         * high_quality_required, it will always return some data from the kernel,
         * regardless of whether the random pool is fully initialized or not.
         * Otherwise, it will return success if at least some random bytes were
         * successfully acquired, and an error if the kernel has no entropy whatsover
         * for us. */

        /* Use the getrandom() syscall unless we know we don't have it. */
        if (have_syscall != 0 && !HAS_FEATURE_MEMORY_SANITIZER) {
                r = getrandom(p, n, GRND_NONBLOCK);
                if (r > 0) {
                        have_syscall = true;
                        if ((size_t) r == n)
                                return 0;
                        if (!high_quality_required) {
                                /* Fill in the remaining bytes using pseudorandom values */
                                pseudorandom_bytes((uint8_t*) p + r, n - r);
                                return 0;
                        }

                        already_done = r;
                } else if (errno == ENOSYS)
                          /* We lack the syscall, continue with reading from /dev/urandom. */
                          have_syscall = false;
                else if (errno == EAGAIN) {
                        /* The kernel has no entropy whatsoever. Let's remember to
                         * use the syscall the next time again though.
                         *
                         * If high_quality_required is false, return an error so that
                         * random_bytes() can produce some pseudorandom
                         * bytes. Otherwise, fall back to /dev/urandom, which we know
                         * is empty, but the kernel will produce some bytes for us on
                         * a best-effort basis. */
                        have_syscall = true;

                        if (!high_quality_required)
                                return -ENODATA;
                } else
                        return -errno;
        }

        fd = open("/dev/urandom", O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return errno == ENOENT ? -ENOSYS : -errno;

        return loop_read_exact(fd, (uint8_t*) p + already_done, n - already_done, true);
}

static void clear_srand_initialization(void) {
        srand_called = false;
}

void initialize_srand(void) {
        static bool pthread_atfork_registered = false;
        unsigned x;
#if HAVE_SYS_AUXV_H
        void *auxv;
#endif

        if (srand_called)
                return;

#if HAVE_SYS_AUXV_H
        /* The kernel provides us with 16 bytes of entropy in auxv, so let's
         * try to make use of that to seed the pseudo-random generator. It's
         * better than nothing... */

        auxv = (void*) getauxval(AT_RANDOM);
        if (auxv) {
                assert_cc(sizeof(x) <= 16);
                memcpy(&x, auxv, sizeof(x));
        } else
#endif
                x = 0;

        x ^= (unsigned) now(CLOCK_REALTIME);
        x ^= (unsigned) gettid();

        srand(x);
        srand_called = true;

        if (!pthread_atfork_registered) {
                (void) pthread_atfork(NULL, NULL, clear_srand_initialization);
                pthread_atfork_registered = true;
        }
}

/* INT_MAX gives us only 31 bits, so use 24 out of that. */
#if RAND_MAX >= INT_MAX
#  define RAND_STEP 3
#else
/* SHORT_INT_MAX or lower gives at most 15 bits, we just just 8 out of that. */
#  define RAND_STEP 1
#endif

void pseudorandom_bytes(void *p, size_t n) {
        uint8_t *q;

        initialize_srand();

        for (q = p; q < (uint8_t*) p + n; q += RAND_STEP) {
                unsigned rr;

                rr = (unsigned) rand();

#if RAND_STEP >= 3
                if ((size_t) (q - (uint8_t*) p + 2) < n)
                        q[2] = rr >> 16;
#endif
#if RAND_STEP >= 2
                if ((size_t) (q - (uint8_t*) p + 1) < n)
                        q[1] = rr >> 8;
#endif
                q[0] = rr;
        }
}

void random_bytes(void *p, size_t n) {
        int r;

        r = acquire_random_bytes(p, n, false);
        if (r >= 0)
                return;

        /* If some idiot made /dev/urandom unavailable to us, or the
         * kernel has no entropy, use a PRNG instead. */
        return pseudorandom_bytes(p, n);
}
