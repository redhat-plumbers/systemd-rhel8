/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "sd-bus.h"
#include "sd-daemon.h"

#include "bus-internal.h"
#include "bus-util.h"
#include "build.h"
#include "log.h"
#include "util.h"

#define DEFAULT_BUS_PATH "unix:path=/run/dbus/system_bus_socket"

const char *arg_bus_path = DEFAULT_BUS_PATH;

static int help(void) {

        printf("%s [OPTIONS...]\n\n"
               "STDIO or socket-activatable proxy to a given DBus endpoint.\n\n"
               "  -h --help              Show this help\n"
               "     --version           Show package version\n"
               "  -p --bus-path=PATH     Path to the kernel bus (default: %s)\n",
               program_invocation_short_name, DEFAULT_BUS_PATH);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
        };

        static const struct option options[] = {
                { "help",            no_argument,       NULL, 'h'         },
                { "version",         no_argument,       NULL, ARG_VERSION },
                { "bus-path",        required_argument, NULL, 'p'         },
                {},
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hsup:", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case '?':
                        return -EINVAL;

                case 'p':
                        arg_bus_path = optarg;
                        break;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }
        }

        return 1;
}

int main(int argc, char *argv[]) {
        _cleanup_(sd_bus_unrefp) sd_bus *a = NULL, *b = NULL;
        sd_id128_t server_id;
        bool is_unix;
        int r, in_fd, out_fd;

        log_set_target(LOG_TARGET_JOURNAL_OR_KMSG);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        r = sd_listen_fds(0);
        if (r == 0) {
                in_fd = STDIN_FILENO;
                out_fd = STDOUT_FILENO;
        } else if (r == 1) {
                in_fd = SD_LISTEN_FDS_START;
                out_fd = SD_LISTEN_FDS_START;
        } else {
                log_error("Illegal number of file descriptors passed.");
                goto finish;
        }

        is_unix =
                sd_is_socket(in_fd, AF_UNIX, 0, 0) > 0 &&
                sd_is_socket(out_fd, AF_UNIX, 0, 0) > 0;

        r = sd_bus_new(&a);
        if (r < 0) {
                log_error_errno(r, "Failed to allocate bus: %m");
                goto finish;
        }

        r = sd_bus_set_address(a, arg_bus_path);
        if (r < 0) {
                log_error_errno(r, "Failed to set address to connect to: %m");
                goto finish;
        }

        r = sd_bus_negotiate_fds(a, is_unix);
        if (r < 0) {
                log_error_errno(r, "Failed to set FD negotiation: %m");
                goto finish;
        }

        r = sd_bus_start(a);
        if (r < 0) {
                log_error_errno(r, "Failed to start bus client: %m");
                goto finish;
        }

        r = sd_bus_get_bus_id(a, &server_id);
        if (r < 0) {
                log_error_errno(r, "Failed to get server ID: %m");
                goto finish;
        }

        r = sd_bus_new(&b);
        if (r < 0) {
                log_error_errno(r, "Failed to allocate bus: %m");
                goto finish;
        }

        r = sd_bus_set_fd(b, in_fd, out_fd);
        if (r < 0) {
                log_error_errno(r, "Failed to set fds: %m");
                goto finish;
        }

        r = sd_bus_set_server(b, 1, server_id);
        if (r < 0) {
                log_error_errno(r, "Failed to set server mode: %m");
                goto finish;
        }

        r = sd_bus_negotiate_fds(b, is_unix);
        if (r < 0) {
                log_error_errno(r, "Failed to set FD negotiation: %m");
                goto finish;
        }

        r = sd_bus_set_anonymous(b, true);
        if (r < 0) {
                log_error_errno(r, "Failed to set anonymous authentication: %m");
                goto finish;
        }

        r = sd_bus_start(b);
        if (r < 0) {
                log_error_errno(r, "Failed to start bus client: %m");
                goto finish;
        }

        for (;;) {
                _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
                int events_a, events_b, fd;
                uint64_t timeout_a, timeout_b, t;
                struct timespec _ts, *ts;

                r = sd_bus_process(a, &m);
                if (r < 0) {
                        log_error_errno(r, "Failed to process bus a: %m");
                        goto finish;
                }

                if (m) {
                        r = sd_bus_send(b, m, NULL);
                        if (r < 0) {
                                log_error_errno(r, "Failed to send message: %m");
                                goto finish;
                        }
                }

                if (r > 0)
                        continue;

                r = sd_bus_process(b, &m);
                if (r < 0) {
                        /* treat 'connection reset by peer' as clean exit condition */
                        if (r == -ECONNRESET)
                                r = 0;

                        goto finish;
                }

                if (m) {
                        r = sd_bus_send(a, m, NULL);
                        if (r < 0) {
                                log_error_errno(r, "Failed to send message: %m");
                                goto finish;
                        }
                }

                if (r > 0)
                        continue;

                fd = sd_bus_get_fd(a);
                if (fd < 0) {
                        r = fd;
                        log_error_errno(r, "Failed to get fd: %m");
                        goto finish;
                }

                events_a = sd_bus_get_events(a);
                if (events_a < 0) {
                        r = events_a;
                        log_error_errno(r, "Failed to get events mask: %m");
                        goto finish;
                }

                r = sd_bus_get_timeout(a, &timeout_a);
                if (r < 0) {
                        log_error_errno(r, "Failed to get timeout: %m");
                        goto finish;
                }

                events_b = sd_bus_get_events(b);
                if (events_b < 0) {
                        r = events_b;
                        log_error_errno(r, "Failed to get events mask: %m");
                        goto finish;
                }

                r = sd_bus_get_timeout(b, &timeout_b);
                if (r < 0) {
                        log_error_errno(r, "Failed to get timeout: %m");
                        goto finish;
                }

                t = timeout_a;
                if (t == (uint64_t) -1 || (timeout_b != (uint64_t) -1 && timeout_b < timeout_a))
                        t = timeout_b;

                if (t == (uint64_t) -1)
                        ts = NULL;
                else {
                        usec_t nw;

                        nw = now(CLOCK_MONOTONIC);
                        if (t > nw)
                                t -= nw;
                        else
                                t = 0;

                        ts = timespec_store(&_ts, t);
                }

                {
                        struct pollfd p[3] = {
                                {.fd = fd,            .events = events_a, },
                                {.fd = STDIN_FILENO,  .events = events_b & POLLIN, },
                                {.fd = STDOUT_FILENO, .events = events_b & POLLOUT, }};

                        r = ppoll(p, ELEMENTSOF(p), ts, NULL);
                }
                if (r < 0) {
                        if (ERRNO_IS_TRANSIENT(r)) /* don't be bothered by signals, i.e. EINTR */
                                continue;
                        log_error_errno(errno, "ppoll() failed: %m");
                        goto finish;
                }
        }

finish:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
