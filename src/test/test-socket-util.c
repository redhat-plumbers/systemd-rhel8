/* SPDX-License-Identifier: LGPL-2.1+ */

#include <sys/types.h>
#include <unistd.h>
#include <grp.h>

#include "alloc-util.h"
#include "async.h"
#include "exit-status.h"
#include "fd-util.h"
#include "fileio.h"
#include "in-addr-util.h"
#include "io-util.h"
#include "log.h"
#include "macro.h"
#include "process-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "util.h"

static void test_ifname_valid(void) {
        assert(ifname_valid("foo"));
        assert(ifname_valid("eth0"));

        assert(!ifname_valid("0"));
        assert(!ifname_valid("99"));
        assert(ifname_valid("a99"));
        assert(ifname_valid("99a"));

        assert(!ifname_valid(NULL));
        assert(!ifname_valid(""));
        assert(!ifname_valid(" "));
        assert(!ifname_valid(" foo"));
        assert(!ifname_valid("bar\n"));
        assert(!ifname_valid("."));
        assert(!ifname_valid(".."));
        assert(ifname_valid("foo.bar"));
        assert(!ifname_valid("x:y"));

        assert(ifname_valid("xxxxxxxxxxxxxxx"));
        assert(!ifname_valid("xxxxxxxxxxxxxxxx"));
        assert(ifname_valid_full("xxxxxxxxxxxxxxxx", true));
}

static void test_socket_address_parse(void) {
        SocketAddress a;

        assert_se(socket_address_parse(&a, "junk") < 0);
        assert_se(socket_address_parse(&a, "192.168.1.1") < 0);
        assert_se(socket_address_parse(&a, ".168.1.1") < 0);
        assert_se(socket_address_parse(&a, "989.168.1.1") < 0);
        assert_se(socket_address_parse(&a, "192.168.1.1:65536") < 0);
        assert_se(socket_address_parse(&a, "192.168.1.1:0") < 0);
        assert_se(socket_address_parse(&a, "0") < 0);
        assert_se(socket_address_parse(&a, "65536") < 0);

        assert_se(socket_address_parse(&a, "65535") >= 0);

        /* The checks below will pass even if ipv6 is disabled in
         * kernel. The underlying glibc's inet_pton() is just a string
         * parser and doesn't make any syscalls. */

        assert_se(socket_address_parse(&a, "[::1]") < 0);
        assert_se(socket_address_parse(&a, "[::1]8888") < 0);
        assert_se(socket_address_parse(&a, "::1") < 0);
        assert_se(socket_address_parse(&a, "[::1]:0") < 0);
        assert_se(socket_address_parse(&a, "[::1]:65536") < 0);
        assert_se(socket_address_parse(&a, "[a:b:1]:8888") < 0);

        assert_se(socket_address_parse(&a, "8888") >= 0);
        assert_se(a.sockaddr.sa.sa_family == (socket_ipv6_is_supported() ? AF_INET6 : AF_INET));

        assert_se(socket_address_parse(&a, "[2001:0db8:0000:85a3:0000:0000:ac1f:8001]:8888") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_INET6);

        assert_se(socket_address_parse(&a, "[::1]:8888") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_INET6);

        assert_se(socket_address_parse(&a, "192.168.1.254:8888") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_INET);

        assert_se(socket_address_parse(&a, "/foo/bar") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_UNIX);

        assert_se(socket_address_parse(&a, "@abstract") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_UNIX);

        assert_se(socket_address_parse(&a, "vsock::1234") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_VSOCK);
        assert_se(socket_address_parse(&a, "vsock:2:1234") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_VSOCK);
        assert_se(socket_address_parse(&a, "vsock:2:1234x") < 0);
        assert_se(socket_address_parse(&a, "vsock:2x:1234") < 0);
        assert_se(socket_address_parse(&a, "vsock:2") < 0);
}

static void test_socket_address_parse_netlink(void) {
        SocketAddress a;

        assert_se(socket_address_parse_netlink(&a, "junk") < 0);
        assert_se(socket_address_parse_netlink(&a, "") < 0);

        assert_se(socket_address_parse_netlink(&a, "route") >= 0);
        assert_se(socket_address_parse_netlink(&a, "route 10") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_NETLINK);
        assert_se(a.protocol == NETLINK_ROUTE);

        /* With spaces and tabs */
        assert_se(socket_address_parse_netlink(&a, " kobject-uevent ") >= 0);
        assert_se(socket_address_parse_netlink(&a, " \t kobject-uevent \t 10 \t") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_NETLINK);
        assert_se(a.protocol == NETLINK_KOBJECT_UEVENT);

        assert_se(socket_address_parse_netlink(&a, "kobject-uevent\t10") >= 0);
        assert_se(a.sockaddr.sa.sa_family == AF_NETLINK);
        assert_se(a.protocol == NETLINK_KOBJECT_UEVENT);

        /* oss-fuzz #6884 */
        assert_se(socket_address_parse_netlink(&a, "\xff") < 0);
}

static void test_socket_address_equal(void) {
        SocketAddress a;
        SocketAddress b;

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "192.168.1.1:888") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "192.16.1.1:8888") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "8888") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "/foo/bar/") >= 0);
        assert_se(!socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_parse(&b, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "/foo/bar") >= 0);
        assert_se(socket_address_parse(&b, "/foo/bar") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "[::1]:8888") >= 0);
        assert_se(socket_address_parse(&b, "[::1]:8888") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "@abstract") >= 0);
        assert_se(socket_address_parse(&b, "@abstract") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse_netlink(&a, "firewall") >= 0);
        assert_se(socket_address_parse_netlink(&b, "firewall") >= 0);
        assert_se(socket_address_equal(&a, &b));

        assert_se(socket_address_parse(&a, "vsock:2:1234") >= 0);
        assert_se(socket_address_parse(&b, "vsock:2:1234") >= 0);
        assert_se(socket_address_equal(&a, &b));
        assert_se(socket_address_parse(&b, "vsock:2:1235") >= 0);
        assert_se(!socket_address_equal(&a, &b));
        assert_se(socket_address_parse(&b, "vsock:3:1234") >= 0);
        assert_se(!socket_address_equal(&a, &b));
}

static void test_socket_address_get_path(void) {
        SocketAddress a;

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(!socket_address_get_path(&a));

        assert_se(socket_address_parse(&a, "@abstract") >= 0);
        assert_se(!socket_address_get_path(&a));

        assert_se(socket_address_parse(&a, "[::1]:8888") >= 0);
        assert_se(!socket_address_get_path(&a));

        assert_se(socket_address_parse(&a, "/foo/bar") >= 0);
        assert_se(streq(socket_address_get_path(&a), "/foo/bar"));

        assert_se(socket_address_parse(&a, "vsock:2:1234") >= 0);
        assert_se(!socket_address_get_path(&a));
}

static void test_socket_address_is(void) {
        SocketAddress a;

        assert_se(socket_address_parse(&a, "192.168.1.1:8888") >= 0);
        assert_se(socket_address_is(&a, "192.168.1.1:8888", SOCK_STREAM));
        assert_se(!socket_address_is(&a, "route", SOCK_STREAM));
        assert_se(!socket_address_is(&a, "192.168.1.1:8888", SOCK_RAW));
}

static void test_socket_address_is_netlink(void) {
        SocketAddress a;

        assert_se(socket_address_parse_netlink(&a, "route 10") >= 0);
        assert_se(socket_address_is_netlink(&a, "route 10"));
        assert_se(!socket_address_is_netlink(&a, "192.168.1.1:8888"));
        assert_se(!socket_address_is_netlink(&a, "route 1"));
}

static void test_in_addr_is_null(void) {

        union in_addr_union i = {};

        assert_se(in_addr_is_null(AF_INET, &i) == true);
        assert_se(in_addr_is_null(AF_INET6, &i) == true);

        i.in.s_addr = 0x1000000;
        assert_se(in_addr_is_null(AF_INET, &i) == false);
        assert_se(in_addr_is_null(AF_INET6, &i) == false);

        assert_se(in_addr_is_null(-1, &i) == -EAFNOSUPPORT);
}

static void test_in_addr_prefix_intersect_one(unsigned f, const char *a, unsigned apl, const char *b, unsigned bpl, int result) {
        union in_addr_union ua, ub;

        assert_se(in_addr_from_string(f, a, &ua) >= 0);
        assert_se(in_addr_from_string(f, b, &ub) >= 0);

        assert_se(in_addr_prefix_intersect(f, &ua, apl, &ub, bpl) == result);
}

static void test_in_addr_prefix_intersect(void) {

        test_in_addr_prefix_intersect_one(AF_INET, "255.255.255.255", 32, "255.255.255.254", 32, 0);
        test_in_addr_prefix_intersect_one(AF_INET, "255.255.255.255", 0, "255.255.255.255", 32, 1);
        test_in_addr_prefix_intersect_one(AF_INET, "0.0.0.0", 0, "47.11.8.15", 32, 1);

        test_in_addr_prefix_intersect_one(AF_INET, "1.1.1.1", 24, "1.1.1.1", 24, 1);
        test_in_addr_prefix_intersect_one(AF_INET, "2.2.2.2", 24, "1.1.1.1", 24, 0);

        test_in_addr_prefix_intersect_one(AF_INET, "1.1.1.1", 24, "1.1.1.127", 25, 1);
        test_in_addr_prefix_intersect_one(AF_INET, "1.1.1.1", 24, "1.1.1.127", 26, 1);
        test_in_addr_prefix_intersect_one(AF_INET, "1.1.1.1", 25, "1.1.1.127", 25, 1);
        test_in_addr_prefix_intersect_one(AF_INET, "1.1.1.1", 25, "1.1.1.255", 25, 0);

        test_in_addr_prefix_intersect_one(AF_INET6, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 128, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe", 128, 0);
        test_in_addr_prefix_intersect_one(AF_INET6, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 0, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 128, 1);
        test_in_addr_prefix_intersect_one(AF_INET6, "::", 0, "beef:beef:beef:beef:beef:beef:beef:beef", 128, 1);

        test_in_addr_prefix_intersect_one(AF_INET6, "1::2", 64, "1::2", 64, 1);
        test_in_addr_prefix_intersect_one(AF_INET6, "2::2", 64, "1::2", 64, 0);

        test_in_addr_prefix_intersect_one(AF_INET6, "1::1", 120, "1::007f", 121, 1);
        test_in_addr_prefix_intersect_one(AF_INET6, "1::1", 120, "1::007f", 122, 1);
        test_in_addr_prefix_intersect_one(AF_INET6, "1::1", 121, "1::007f", 121, 1);
        test_in_addr_prefix_intersect_one(AF_INET6, "1::1", 121, "1::00ff", 121, 0);
}

static void test_in_addr_prefix_next_one(unsigned f, const char *before, unsigned pl, const char *after) {
        union in_addr_union ubefore, uafter, t;

        assert_se(in_addr_from_string(f, before, &ubefore) >= 0);

        t = ubefore;
        assert_se((in_addr_prefix_next(f, &t, pl) > 0) == !!after);

        if (after) {
                assert_se(in_addr_from_string(f, after, &uafter) >= 0);
                assert_se(in_addr_equal(f, &t, &uafter) > 0);
        }
}

static void test_in_addr_prefix_next(void) {

        test_in_addr_prefix_next_one(AF_INET, "192.168.0.0", 24, "192.168.1.0");
        test_in_addr_prefix_next_one(AF_INET, "192.168.0.0", 16, "192.169.0.0");
        test_in_addr_prefix_next_one(AF_INET, "192.168.0.0", 20, "192.168.16.0");

        test_in_addr_prefix_next_one(AF_INET, "0.0.0.0", 32, "0.0.0.1");
        test_in_addr_prefix_next_one(AF_INET, "255.255.255.255", 32, NULL);
        test_in_addr_prefix_next_one(AF_INET, "255.255.255.0", 24, NULL);

        test_in_addr_prefix_next_one(AF_INET6, "4400::", 128, "4400::0001");
        test_in_addr_prefix_next_one(AF_INET6, "4400::", 120, "4400::0100");
        test_in_addr_prefix_next_one(AF_INET6, "4400::", 127, "4400::0002");
        test_in_addr_prefix_next_one(AF_INET6, "4400::", 8, "4500::");
        test_in_addr_prefix_next_one(AF_INET6, "4400::", 7, "4600::");

        test_in_addr_prefix_next_one(AF_INET6, "::", 128, "::1");

        test_in_addr_prefix_next_one(AF_INET6, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 128, NULL);
        test_in_addr_prefix_next_one(AF_INET6, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ff00", 120, NULL);

}

static void test_in_addr_to_string_one(int f, const char *addr) {
        union in_addr_union ua;
        _cleanup_free_ char *r = NULL;

        assert_se(in_addr_from_string(f, addr, &ua) >= 0);
        assert_se(in_addr_to_string(f, &ua, &r) >= 0);
        printf("test_in_addr_to_string_one: %s == %s\n", addr, r);
        assert_se(streq(addr, r));
}

static void test_in_addr_to_string(void) {
        test_in_addr_to_string_one(AF_INET, "192.168.0.1");
        test_in_addr_to_string_one(AF_INET, "10.11.12.13");
        test_in_addr_to_string_one(AF_INET6, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
        test_in_addr_to_string_one(AF_INET6, "::1");
        test_in_addr_to_string_one(AF_INET6, "fe80::");
}

static void test_in_addr_ifindex_to_string_one(int f, const char *a, int ifindex, const char *b) {
        _cleanup_free_ char *r = NULL;
        union in_addr_union ua, uuaa;
        int ff, ifindex2;

        assert_se(in_addr_from_string(f, a, &ua) >= 0);
        assert_se(in_addr_ifindex_to_string(f, &ua, ifindex, &r) >= 0);
        printf("test_in_addr_ifindex_to_string_one: %s == %s\n", b, r);
        assert_se(streq(b, r));

        assert_se(in_addr_ifindex_from_string_auto(b, &ff, &uuaa, &ifindex2) >= 0);
        assert_se(ff == f);
        assert_se(in_addr_equal(f, &ua, &uuaa));
        assert_se(ifindex2 == ifindex || ifindex2 == 0);
}

static void test_in_addr_ifindex_to_string(void) {
        test_in_addr_ifindex_to_string_one(AF_INET, "192.168.0.1", 7, "192.168.0.1");
        test_in_addr_ifindex_to_string_one(AF_INET, "10.11.12.13", 9, "10.11.12.13");
        test_in_addr_ifindex_to_string_one(AF_INET6, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 10, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
        test_in_addr_ifindex_to_string_one(AF_INET6, "::1", 11, "::1");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::", 12, "fe80::%12");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::", 0, "fe80::");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::14", 12, "fe80::14%12");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::15", -7, "fe80::15");
        test_in_addr_ifindex_to_string_one(AF_INET6, "fe80::16", LOOPBACK_IFINDEX, "fe80::16%1");
}

static void test_in_addr_ifindex_from_string_auto(void) {
        int family, ifindex;
        union in_addr_union ua;

        /* Most in_addr_ifindex_from_string_auto() invocations have already been tested above, but let's test some more */

        assert_se(in_addr_ifindex_from_string_auto("fe80::17", &family, &ua, &ifindex) >= 0);
        assert_se(family == AF_INET6);
        assert_se(ifindex == 0);

        assert_se(in_addr_ifindex_from_string_auto("fe80::18%19", &family, &ua, &ifindex) >= 0);
        assert_se(family == AF_INET6);
        assert_se(ifindex == 19);

        assert_se(in_addr_ifindex_from_string_auto("fe80::18%lo", &family, &ua, &ifindex) >= 0);
        assert_se(family == AF_INET6);
        assert_se(ifindex == LOOPBACK_IFINDEX);

        assert_se(in_addr_ifindex_from_string_auto("fe80::19%thisinterfacecantexist", &family, &ua, &ifindex) == -ENODEV);
}

static void test_sockaddr_equal(void) {
        union sockaddr_union a = {
                .in.sin_family = AF_INET,
                .in.sin_port = 0,
                .in.sin_addr.s_addr = htobe32(INADDR_ANY),
        };
        union sockaddr_union b = {
                .in.sin_family = AF_INET,
                .in.sin_port = 0,
                .in.sin_addr.s_addr = htobe32(INADDR_ANY),
        };
        union sockaddr_union c = {
                .in.sin_family = AF_INET,
                .in.sin_port = 0,
                .in.sin_addr.s_addr = htobe32(1234),
        };
        union sockaddr_union d = {
                .in6.sin6_family = AF_INET6,
                .in6.sin6_port = 0,
                .in6.sin6_addr = IN6ADDR_ANY_INIT,
        };
        union sockaddr_union e = {
                .vm.svm_family = AF_VSOCK,
                .vm.svm_port = 0,
                .vm.svm_cid = VMADDR_CID_ANY,
        };
        assert_se(sockaddr_equal(&a, &a));
        assert_se(sockaddr_equal(&a, &b));
        assert_se(sockaddr_equal(&d, &d));
        assert_se(sockaddr_equal(&e, &e));
        assert_se(!sockaddr_equal(&a, &c));
        assert_se(!sockaddr_equal(&b, &c));
        assert_se(!sockaddr_equal(&a, &e));
}

static void test_sockaddr_un_len(void) {
        static const struct sockaddr_un fs = {
                .sun_family = AF_UNIX,
                .sun_path = "/foo/bar/waldo",
        };

        static const struct sockaddr_un abstract = {
                .sun_family = AF_UNIX,
                .sun_path = "\0foobar",
        };

        assert_se(SOCKADDR_UN_LEN(fs) == offsetof(struct sockaddr_un, sun_path) + strlen(fs.sun_path));
        assert_se(SOCKADDR_UN_LEN(abstract) == offsetof(struct sockaddr_un, sun_path) + 1 + strlen(abstract.sun_path + 1));
}

static void test_in_addr_is_multicast(void) {
        union in_addr_union a, b;
        int f;

        assert_se(in_addr_from_string_auto("192.168.3.11", &f, &a) >= 0);
        assert_se(in_addr_is_multicast(f, &a) == 0);

        assert_se(in_addr_from_string_auto("224.0.0.1", &f, &a) >= 0);
        assert_se(in_addr_is_multicast(f, &a) == 1);

        assert_se(in_addr_from_string_auto("FF01:0:0:0:0:0:0:1", &f, &b) >= 0);
        assert_se(in_addr_is_multicast(f, &b) == 1);

        assert_se(in_addr_from_string_auto("2001:db8::c:69b:aeff:fe53:743e", &f, &b) >= 0);
        assert_se(in_addr_is_multicast(f, &b) == 0);
}

static void test_getpeercred_getpeergroups(void) {
        int r;

        r = safe_fork("(getpeercred)", FORK_DEATHSIG|FORK_LOG|FORK_WAIT, NULL);
        assert_se(r >= 0);

        if (r == 0) {
                static const gid_t gids[] = { 3, 4, 5, 6, 7 };
                gid_t *test_gids;
                size_t n_test_gids;
                uid_t test_uid;
                gid_t test_gid;
                struct ucred ucred;
                int pair[2];

                if (geteuid() == 0) {
                        test_uid = 1;
                        test_gid = 2;
                        test_gids = (gid_t*) gids;
                        n_test_gids = ELEMENTSOF(gids);

                        assert_se(setgroups(n_test_gids, test_gids) >= 0);
                        assert_se(setresgid(test_gid, test_gid, test_gid) >= 0);
                        assert_se(setresuid(test_uid, test_uid, test_uid) >= 0);

                } else {
                        long ngroups_max;

                        test_uid = getuid();
                        test_gid = getgid();

                        ngroups_max = sysconf(_SC_NGROUPS_MAX);
                        assert(ngroups_max > 0);

                        test_gids = newa(gid_t, ngroups_max);

                        r = getgroups(ngroups_max, test_gids);
                        assert_se(r >= 0);
                        n_test_gids = (size_t) r;
                }

                assert_se(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) >= 0);

                assert_se(getpeercred(pair[0], &ucred) >= 0);

                assert_se(ucred.uid == test_uid);
                assert_se(ucred.gid == test_gid);
                assert_se(ucred.pid == getpid_cached());

                {
                        _cleanup_free_ gid_t *peer_groups = NULL;

                        r = getpeergroups(pair[0], &peer_groups);
                        assert_se(r >= 0 || IN_SET(r, -EOPNOTSUPP, -ENOPROTOOPT));

                        if (r >= 0) {
                                assert_se((size_t) r == n_test_gids);
                                assert_se(memcmp(peer_groups, test_gids, sizeof(gid_t) * n_test_gids) == 0);
                        }
                }

                safe_close_pair(pair);
                _exit(EXIT_SUCCESS);
        }
}

static void test_passfd_read(void) {
        static const char file_contents[] = "test contents for passfd";
        _cleanup_close_pair_ int pair[2] = { -1, -1 };
        int r;

        assert_se(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) >= 0);

        r = safe_fork("(passfd_read)", FORK_DEATHSIG|FORK_LOG|FORK_WAIT, NULL);
        assert_se(r >= 0);

        if (r == 0) {
                /* Child */
                char tmpfile[] = "/tmp/test-socket-util-passfd-read-XXXXXX";
                _cleanup_close_ int tmpfd = -1;

                pair[0] = safe_close(pair[0]);

                tmpfd = mkostemp_safe(tmpfile);
                assert_se(tmpfd >= 0);
                assert_se(write(tmpfd, file_contents, strlen(file_contents)) == (ssize_t) strlen(file_contents));
                tmpfd = safe_close(tmpfd);

                tmpfd = open(tmpfile, O_RDONLY);
                assert_se(tmpfd >= 0);
                assert_se(unlink(tmpfile) == 0);

                assert_se(send_one_fd(pair[1], tmpfd, MSG_DONTWAIT) == 0);
                _exit(EXIT_SUCCESS);
        }

        /* Parent */
        char buf[64];
        struct iovec iov = IOVEC_INIT(buf, sizeof(buf)-1);
        _cleanup_close_ int fd = -1;

        pair[1] = safe_close(pair[1]);

        assert_se(receive_one_fd_iov(pair[0], &iov, 1, MSG_DONTWAIT, &fd) == 0);

        assert_se(fd >= 0);
        r = read(fd, buf, sizeof(buf)-1);
        assert_se(r >= 0);
        buf[r] = 0;
        assert_se(streq(buf, file_contents));
}

static void test_passfd_contents_read(void) {
        _cleanup_close_pair_ int pair[2] = { -1, -1 };
        static const char file_contents[] = "test contents in the file";
        static const char wire_contents[] = "test contents on the wire";
        int r;

        assert_se(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) >= 0);

        r = safe_fork("(passfd_contents_read)", FORK_DEATHSIG|FORK_LOG|FORK_WAIT, NULL);
        assert_se(r >= 0);

        if (r == 0) {
                /* Child */
                struct iovec iov = IOVEC_INIT_STRING(wire_contents);
                char tmpfile[] = "/tmp/test-socket-util-passfd-contents-read-XXXXXX";
                _cleanup_close_ int tmpfd = -1;

                pair[0] = safe_close(pair[0]);

                tmpfd = mkostemp_safe(tmpfile);
                assert_se(tmpfd >= 0);
                assert_se(write(tmpfd, file_contents, strlen(file_contents)) == (ssize_t) strlen(file_contents));
                tmpfd = safe_close(tmpfd);

                tmpfd = open(tmpfile, O_RDONLY);
                assert_se(tmpfd >= 0);
                assert_se(unlink(tmpfile) == 0);

                assert_se(send_one_fd_iov(pair[1], tmpfd, &iov, 1, MSG_DONTWAIT) > 0);
                _exit(EXIT_SUCCESS);
        }

        /* Parent */
        char buf[64];
        struct iovec iov = IOVEC_INIT(buf, sizeof(buf)-1);
        _cleanup_close_ int fd = -1;
        ssize_t k;

        pair[1] = safe_close(pair[1]);

        k = receive_one_fd_iov(pair[0], &iov, 1, MSG_DONTWAIT, &fd);
        assert_se(k > 0);
        buf[k] = 0;
        assert_se(streq(buf, wire_contents));

        assert_se(fd >= 0);
        r = read(fd, buf, sizeof(buf)-1);
        assert_se(r >= 0);
        buf[r] = 0;
        assert_se(streq(buf, file_contents));
}

static void test_receive_nopassfd(void) {
        _cleanup_close_pair_ int pair[2] = { -1, -1 };
        static const char wire_contents[] = "no fd passed here";
        int r;

        assert_se(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) >= 0);

        r = safe_fork("(receive_nopassfd)", FORK_DEATHSIG|FORK_LOG|FORK_WAIT, NULL);
        assert_se(r >= 0);

        if (r == 0) {
                /* Child */
                struct iovec iov = IOVEC_INIT_STRING(wire_contents);

                pair[0] = safe_close(pair[0]);

                assert_se(send_one_fd_iov(pair[1], -1, &iov, 1, MSG_DONTWAIT) > 0);
                _exit(EXIT_SUCCESS);
        }

        /* Parent */
        char buf[64];
        struct iovec iov = IOVEC_INIT(buf, sizeof(buf)-1);
        int fd = -999;
        ssize_t k;

        pair[1] = safe_close(pair[1]);

        k = receive_one_fd_iov(pair[0], &iov, 1, MSG_DONTWAIT, &fd);
        assert_se(k > 0);
        buf[k] = 0;
        assert_se(streq(buf, wire_contents));

        /* no fd passed here, confirm it was reset */
        assert_se(fd == -1);
}

static void test_send_nodata_nofd(void) {
        _cleanup_close_pair_ int pair[2] = { -1, -1 };
        int r;

        assert_se(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) >= 0);

        r = safe_fork("(send_nodata_nofd)", FORK_DEATHSIG|FORK_LOG|FORK_WAIT, NULL);
        assert_se(r >= 0);

        if (r == 0) {
                /* Child */
                pair[0] = safe_close(pair[0]);

                assert_se(send_one_fd_iov(pair[1], -1, NULL, 0, MSG_DONTWAIT) == -EINVAL);
                _exit(EXIT_SUCCESS);
        }

        /* Parent */
        char buf[64];
        struct iovec iov = IOVEC_INIT(buf, sizeof(buf)-1);
        int fd = -999;
        ssize_t k;

        pair[1] = safe_close(pair[1]);

        k = receive_one_fd_iov(pair[0], &iov, 1, MSG_DONTWAIT, &fd);
        /* recvmsg() will return errno EAGAIN if nothing was sent */
        assert_se(k == -EAGAIN);

        /* receive_one_fd_iov returned error, so confirm &fd wasn't touched */
        assert_se(fd == -999);
}

static void test_send_emptydata(void) {
        _cleanup_close_pair_ int pair[2] = { -1, -1 };
        int r;

        assert_se(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) >= 0);

        r = safe_fork("(send_emptydata)", FORK_DEATHSIG|FORK_LOG|FORK_WAIT, NULL);
        assert_se(r >= 0);

        if (r == 0) {
                /* Child */
                struct iovec iov = IOVEC_INIT_STRING("");  /* zero-length iov */
                assert_se(iov.iov_len == 0);

                pair[0] = safe_close(pair[0]);

                /* This will succeed, since iov is set. */
                assert_se(send_one_fd_iov(pair[1], -1, &iov, 1, MSG_DONTWAIT) == 0);
                _exit(EXIT_SUCCESS);
        }

        /* Parent */
        char buf[64];
        struct iovec iov = IOVEC_INIT(buf, sizeof(buf)-1);
        int fd = -999;
        ssize_t k;

        pair[1] = safe_close(pair[1]);

        k = receive_one_fd_iov(pair[0], &iov, 1, MSG_DONTWAIT, &fd);
        /* receive_one_fd_iov() returns -EIO if an fd is not found and no data was returned. */
        assert_se(k == -EIO);

        /* receive_one_fd_iov returned error, so confirm &fd wasn't touched */
        assert_se(fd == -999);
}

int main(int argc, char *argv[]) {

        log_set_max_level(LOG_DEBUG);

        test_ifname_valid();

        test_socket_address_parse();
        test_socket_address_parse_netlink();
        test_socket_address_equal();
        test_socket_address_get_path();
        test_socket_address_is();
        test_socket_address_is_netlink();

        test_in_addr_is_null();
        test_in_addr_prefix_intersect();
        test_in_addr_prefix_next();
        test_in_addr_to_string();
        test_in_addr_ifindex_to_string();
        test_in_addr_ifindex_from_string_auto();

        test_sockaddr_equal();

        test_sockaddr_un_len();

        test_in_addr_is_multicast();

        test_getpeercred_getpeergroups();

        test_passfd_read();
        test_passfd_contents_read();
        test_receive_nopassfd();
        test_send_nodata_nofd();
        test_send_emptydata();

        return 0;
}
