/* SPDX-License-Identifier: LGPL-2.1+ */

#include "sd-bus.h"
#include "tests.h"

#include <stdlib.h>

#include "sd-daemon.h"

#include "pager.h"
#include "selinux-util.h"
#include "spawn-ask-password-agent.h"
#include "spawn-polkit-agent.h"
#include "util.h"

#define _DEFINE_MAIN_FUNCTION(intro, impl, ret)                         \
        int main(int argc, char *argv[]) {                              \
                int r;                                                  \
                intro;                                                  \
                r = impl;                                               \
                if (r < 0)                                              \
                        (void) sd_notifyf(0, "ERRNO=%i", -r);           \
                ask_password_agent_close();                             \
                polkit_agent_close();                                   \
                pager_close();                                          \
                mac_selinux_finish();                                   \
                return ret;                                             \
        }

/* Negative return values from impl are mapped to EXIT_FAILURE, and
 * everything else means success! */
#define DEFINE_MAIN_FUNCTION(impl)                                      \
        _DEFINE_MAIN_FUNCTION(,impl(argc, argv), r < 0 ? EXIT_FAILURE : EXIT_SUCCESS)

/* Zero is mapped to EXIT_SUCCESS, negative values are mapped to EXIT_FAILURE,
 * and positive values are propagated.
 * Note: "true" means failure! */
#define DEFINE_MAIN_FUNCTION_WITH_POSITIVE_FAILURE(impl)                \
        _DEFINE_MAIN_FUNCTION(,impl(argc, argv), r < 0 ? EXIT_FAILURE : r)

static int run(int argc, char *argv[]) {
        sd_bus_message *m = NULL;
        sd_bus *bus = NULL;
        int r;

        /* This test will result in a memory leak in <= v240, but not on v241. Hence to be really useful it
         * should be run through a leak tracker such as valgrind. */

        r = sd_bus_open_system(&bus);
        if (r < 0)
                return 1;

        /* Create a message and enqueue it (this shouldn't send it though as the connection setup is not complete yet) */
        assert_se(sd_bus_message_new_method_call(bus, &m, "foo.bar", "/foo", "quux.quux", "waldo") >= 0);
        assert_se(sd_bus_send(bus, m, NULL) >= 0);

        /* Let's now unref the message first and the bus second. */
        m = sd_bus_message_unref(m);
        bus = sd_bus_unref(bus);

        /* We should have a memory leak now on <= v240. Let's do this again, but destory in the opposite
         * order. On v240 that too should be a leak. */

        r = sd_bus_open_system(&bus);
        if (r < 0)
                return 1;

        assert_se(sd_bus_message_new_method_call(bus, &m, "foo.bar", "/foo", "quux.quux", "waldo") >= 0);
        assert_se(sd_bus_send(bus, m, NULL) >= 0);

        /* Let's now unref things in the opposite order */
        bus = sd_bus_unref(bus);
        m = sd_bus_message_unref(m);

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
