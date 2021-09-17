/* SPDX-License-Identifier: LGPL-2.1+ */

#include <net/if.h>
#include <netinet/ether.h>

#include "sd-netlink.h"

#include "ether-addr-util.h"
#include "macro.h"
#include "missing.h"
#include "netlink-util.h"
#include "socket-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "util.h"

static void test_message_link_bridge(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        uint32_t cost;

        assert_se(sd_rtnl_message_new_link(rtnl, &message, RTM_NEWLINK, 1) >= 0);
        assert_se(sd_rtnl_message_link_set_family(message, PF_BRIDGE) >= 0);
        assert_se(sd_netlink_message_open_container(message, IFLA_PROTINFO) >= 0);
        assert_se(sd_netlink_message_append_u32(message, IFLA_BRPORT_COST, 10) >= 0);
        assert_se(sd_netlink_message_close_container(message) >= 0);

        assert_se(sd_netlink_message_rewind(message) >= 0);

        assert_se(sd_netlink_message_enter_container(message, IFLA_PROTINFO) >= 0);
        assert_se(sd_netlink_message_read_u32(message, IFLA_BRPORT_COST, &cost) >= 0);
        assert_se(cost == 10);
        assert_se(sd_netlink_message_exit_container(message) >= 0);
}

static void test_link_configure(sd_netlink *rtnl, int ifindex) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        const char *mac = "98:fe:94:3f:c6:18", *name = "test";
        char buffer[ETHER_ADDR_TO_STRING_MAX];
        uint32_t mtu = 1450, mtu_out;
        const char *name_out;
        struct ether_addr mac_out;

        /* we'd really like to test NEWLINK, but let's not mess with the running kernel */
        assert_se(sd_rtnl_message_new_link(rtnl, &message, RTM_GETLINK, ifindex) >= 0);
        assert_se(sd_netlink_message_append_string(message, IFLA_IFNAME, name) >= 0);
        assert_se(sd_netlink_message_append_ether_addr(message, IFLA_ADDRESS, ether_aton(mac)) >= 0);
        assert_se(sd_netlink_message_append_u32(message, IFLA_MTU, mtu) >= 0);

        assert_se(sd_netlink_call(rtnl, message, 0, NULL) == 1);
        assert_se(sd_netlink_message_rewind(message) >= 0);

        assert_se(sd_netlink_message_read_string(message, IFLA_IFNAME, &name_out) >= 0);
        assert_se(streq(name, name_out));

        assert_se(sd_netlink_message_read_ether_addr(message, IFLA_ADDRESS, &mac_out) >= 0);
        assert_se(streq(mac, ether_addr_to_string(&mac_out, buffer)));

        assert_se(sd_netlink_message_read_u32(message, IFLA_MTU, &mtu_out) >= 0);
        assert_se(mtu == mtu_out);
}

static void test_link_get(sd_netlink *rtnl, int ifindex) {
        sd_netlink_message *m;
        sd_netlink_message *r;
        uint32_t mtu = 1500;
        const char *str_data;
        uint8_t u8_data;
        uint32_t u32_data;
        struct ether_addr eth_data;

        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_GETLINK, ifindex) >= 0);
        assert_se(m);

        /* u8 test cases  */
        assert_se(sd_netlink_message_append_u8(m, IFLA_CARRIER, 0) >= 0);
        assert_se(sd_netlink_message_append_u8(m, IFLA_OPERSTATE, 0) >= 0);
        assert_se(sd_netlink_message_append_u8(m, IFLA_LINKMODE, 0) >= 0);

        /* u32 test cases */
        assert_se(sd_netlink_message_append_u32(m, IFLA_MTU, mtu) >= 0);
        assert_se(sd_netlink_message_append_u32(m, IFLA_GROUP, 0) >= 0);
        assert_se(sd_netlink_message_append_u32(m, IFLA_TXQLEN, 0) >= 0);
        assert_se(sd_netlink_message_append_u32(m, IFLA_NUM_TX_QUEUES, 0) >= 0);
        assert_se(sd_netlink_message_append_u32(m, IFLA_NUM_RX_QUEUES, 0) >= 0);

        assert_se(sd_netlink_call(rtnl, m, -1, &r) == 1);

        assert_se(sd_netlink_message_read_string(r, IFLA_IFNAME, &str_data) == 0);

        assert_se(sd_netlink_message_read_u8(r, IFLA_CARRIER, &u8_data) == 0);
        assert_se(sd_netlink_message_read_u8(r, IFLA_OPERSTATE, &u8_data) == 0);
        assert_se(sd_netlink_message_read_u8(r, IFLA_LINKMODE, &u8_data) == 0);

        assert_se(sd_netlink_message_read_u32(r, IFLA_MTU, &u32_data) == 0);
        assert_se(sd_netlink_message_read_u32(r, IFLA_GROUP, &u32_data) == 0);
        assert_se(sd_netlink_message_read_u32(r, IFLA_TXQLEN, &u32_data) == 0);
        assert_se(sd_netlink_message_read_u32(r, IFLA_NUM_TX_QUEUES, &u32_data) == 0);
        assert_se(sd_netlink_message_read_u32(r, IFLA_NUM_RX_QUEUES, &u32_data) == 0);

        assert_se(sd_netlink_message_read_ether_addr(r, IFLA_ADDRESS, &eth_data) == 0);

        assert_se((m = sd_netlink_message_unref(m)) == NULL);
        assert_se((r = sd_netlink_message_unref(r)) == NULL);
}

static void test_address_get(sd_netlink *rtnl, int ifindex) {
        sd_netlink_message *m;
        sd_netlink_message *r;
        struct in_addr in_data;
        struct ifa_cacheinfo cache;
        const char *label;

        assert_se(sd_rtnl_message_new_addr(rtnl, &m, RTM_GETADDR, ifindex, AF_INET) >= 0);
        assert_se(m);

        assert_se(sd_netlink_call(rtnl, m, -1, &r) == 1);

        assert_se(sd_netlink_message_read_in_addr(r, IFA_LOCAL, &in_data) == 0);
        assert_se(sd_netlink_message_read_in_addr(r, IFA_ADDRESS, &in_data) == 0);
        assert_se(sd_netlink_message_read_string(r, IFA_LABEL, &label) == 0);
        assert_se(sd_netlink_message_read_cache_info(r, IFA_CACHEINFO, &cache) == 0);

        assert_se((m = sd_netlink_message_unref(m)) == NULL);
        assert_se((r = sd_netlink_message_unref(r)) == NULL);

}

static void test_route(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req;
        struct in_addr addr, addr_data;
        uint32_t index = 2, u32_data;
        int r;

        r = sd_rtnl_message_new_route(rtnl, &req, RTM_NEWROUTE, AF_INET, RTPROT_STATIC);
        if (r < 0) {
                log_error_errno(r, "Could not create RTM_NEWROUTE message: %m");
                return;
        }

        addr.s_addr = htonl(INADDR_LOOPBACK);

        r = sd_netlink_message_append_in_addr(req, RTA_GATEWAY, &addr);
        if (r < 0) {
                log_error_errno(r, "Could not append RTA_GATEWAY attribute: %m");
                return;
        }

        r = sd_netlink_message_append_u32(req, RTA_OIF, index);
        if (r < 0) {
                log_error_errno(r, "Could not append RTA_OIF attribute: %m");
                return;
        }

        assert_se(sd_netlink_message_rewind(req) >= 0);

        assert_se(sd_netlink_message_read_in_addr(req, RTA_GATEWAY, &addr_data) >= 0);
        assert_se(addr_data.s_addr == addr.s_addr);

        assert_se(sd_netlink_message_read_u32(req, RTA_OIF, &u32_data) >= 0);
        assert_se(u32_data == index);

        assert_se((req = sd_netlink_message_unref(req)) == NULL);
}

static void test_multiple(void) {
        sd_netlink *rtnl1, *rtnl2;

        assert_se(sd_netlink_open(&rtnl1) >= 0);
        assert_se(sd_netlink_open(&rtnl2) >= 0);

        rtnl1 = sd_netlink_unref(rtnl1);
        rtnl2 = sd_netlink_unref(rtnl2);
}

static int link_handler(sd_netlink *rtnl, sd_netlink_message *m, void *userdata) {
        char *ifname = userdata;
        const char *data;

        assert_se(rtnl);
        assert_se(m);

        log_info("got link info about %s", ifname);
        free(ifname);

        assert_se(sd_netlink_message_read_string(m, IFLA_IFNAME, &data) >= 0);
        assert_se(streq(data, "lo"));

        return 1;
}

static void test_event_loop(int ifindex) {
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        char *ifname;

        ifname = strdup("lo2");
        assert_se(ifname);

        assert_se(sd_netlink_open(&rtnl) >= 0);
        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_GETLINK, ifindex) >= 0);

        assert_se(sd_netlink_call_async(rtnl, m, link_handler, ifname, 0, NULL) >= 0);

        assert_se(sd_event_default(&event) >= 0);

        assert_se(sd_netlink_attach_event(rtnl, event, 0) >= 0);

        assert_se(sd_event_run(event, 0) >= 0);

        assert_se(sd_netlink_detach_event(rtnl) >= 0);

        assert_se((rtnl = sd_netlink_unref(rtnl)) == NULL);
}

static int pipe_handler(sd_netlink *rtnl, sd_netlink_message *m, void *userdata) {
        int *counter = userdata;
        int r;

        (*counter)--;

        r = sd_netlink_message_get_errno(m);

        log_info_errno(r, "%d left in pipe. got reply: %m", *counter);

        assert_se(r >= 0);

        return 1;
}

static void test_async(int ifindex) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL, *r = NULL;
        uint32_t serial;
        char *ifname;

        ifname = strdup("lo");
        assert_se(ifname);

        assert_se(sd_netlink_open(&rtnl) >= 0);

        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_GETLINK, ifindex) >= 0);

        assert_se(sd_netlink_call_async(rtnl, m, link_handler, ifname, 0, &serial) >= 0);

        assert_se(sd_netlink_wait(rtnl, 0) >= 0);
        assert_se(sd_netlink_process(rtnl, &r) >= 0);

        assert_se((rtnl = sd_netlink_unref(rtnl)) == NULL);
}

static void test_pipe(int ifindex) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m1 = NULL, *m2 = NULL;
        int counter = 0;

        assert_se(sd_netlink_open(&rtnl) >= 0);

        assert_se(sd_rtnl_message_new_link(rtnl, &m1, RTM_GETLINK, ifindex) >= 0);
        assert_se(sd_rtnl_message_new_link(rtnl, &m2, RTM_GETLINK, ifindex) >= 0);

        counter++;
        assert_se(sd_netlink_call_async(rtnl, m1, pipe_handler, &counter, 0, NULL) >= 0);

        counter++;
        assert_se(sd_netlink_call_async(rtnl, m2, pipe_handler, &counter, 0, NULL) >= 0);

        while (counter > 0) {
                assert_se(sd_netlink_wait(rtnl, 0) >= 0);
                assert_se(sd_netlink_process(rtnl, NULL) >= 0);
        }

        assert_se((rtnl = sd_netlink_unref(rtnl)) == NULL);
}

static void test_container(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        uint16_t u16_data;
        uint32_t u32_data;
        const char *string_data;

        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_NEWLINK, 0) >= 0);

        assert_se(sd_netlink_message_open_container(m, IFLA_LINKINFO) >= 0);
        assert_se(sd_netlink_message_open_container_union(m, IFLA_INFO_DATA, "vlan") >= 0);
        assert_se(sd_netlink_message_append_u16(m, IFLA_VLAN_ID, 100) >= 0);
        assert_se(sd_netlink_message_close_container(m) >= 0);
        assert_se(sd_netlink_message_append_string(m, IFLA_INFO_KIND, "vlan") >= 0);
        assert_se(sd_netlink_message_close_container(m) >= 0);
        assert_se(sd_netlink_message_close_container(m) == -EINVAL);

        assert_se(sd_netlink_message_rewind(m) >= 0);

        assert_se(sd_netlink_message_enter_container(m, IFLA_LINKINFO) >= 0);
        assert_se(sd_netlink_message_read_string(m, IFLA_INFO_KIND, &string_data) >= 0);
        assert_se(streq("vlan", string_data));

        assert_se(sd_netlink_message_enter_container(m, IFLA_INFO_DATA) >= 0);
        assert_se(sd_netlink_message_read_u16(m, IFLA_VLAN_ID, &u16_data) >= 0);
        assert_se(sd_netlink_message_exit_container(m) >= 0);

        assert_se(sd_netlink_message_read_string(m, IFLA_INFO_KIND, &string_data) >= 0);
        assert_se(streq("vlan", string_data));
        assert_se(sd_netlink_message_exit_container(m) >= 0);

        assert_se(sd_netlink_message_read_u32(m, IFLA_LINKINFO, &u32_data) < 0);

        assert_se(sd_netlink_message_exit_container(m) == -EINVAL);
}

static void test_match(void) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;

        assert_se(sd_netlink_open(&rtnl) >= 0);

        assert_se(sd_netlink_add_match(rtnl, RTM_NEWLINK, link_handler, NULL) >= 0);
        assert_se(sd_netlink_add_match(rtnl, RTM_NEWLINK, link_handler, NULL) >= 0);

        assert_se(sd_netlink_remove_match(rtnl, RTM_NEWLINK, link_handler, NULL) == 1);
        assert_se(sd_netlink_remove_match(rtnl, RTM_NEWLINK, link_handler, NULL) == 1);
        assert_se(sd_netlink_remove_match(rtnl, RTM_NEWLINK, link_handler, NULL) == 0);

        assert_se((rtnl = sd_netlink_unref(rtnl)) == NULL);
}

static void test_get_addresses(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        sd_netlink_message *m;

        assert_se(sd_rtnl_message_new_addr(rtnl, &req, RTM_GETADDR, 0, AF_UNSPEC) >= 0);

        assert_se(sd_netlink_call(rtnl, req, 0, &reply) >= 0);

        for (m = reply; m; m = sd_netlink_message_next(m)) {
                uint16_t type;
                unsigned char scope, flags;
                int family, ifindex;

                assert_se(sd_netlink_message_get_type(m, &type) >= 0);
                assert_se(type == RTM_NEWADDR);

                assert_se(sd_rtnl_message_addr_get_ifindex(m, &ifindex) >= 0);
                assert_se(sd_rtnl_message_addr_get_family(m, &family) >= 0);
                assert_se(sd_rtnl_message_addr_get_scope(m, &scope) >= 0);
                assert_se(sd_rtnl_message_addr_get_flags(m, &flags) >= 0);

                assert_se(ifindex > 0);
                assert_se(IN_SET(family, AF_INET, AF_INET6));

                log_info("got IPv%u address on ifindex %i", family == AF_INET ? 4: 6, ifindex);
        }
}

static void test_message(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;

        assert_se(rtnl_message_new_synthetic_error(rtnl, -ETIMEDOUT, 1, &m) >= 0);
        assert_se(sd_netlink_message_get_errno(m) == -ETIMEDOUT);
}

static void test_strv(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        _cleanup_strv_free_ char **names_in = NULL, **names_out;
        const char *p;

        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_NEWLINKPROP, 1) >= 0);

        for (unsigned i = 0; i < 10; i++) {
                char name[STRLEN("hoge") + DECIMAL_STR_MAX(uint32_t)];

                xsprintf(name, "hoge%" PRIu32, i + 1000);
                assert_se(strv_extend(&names_in, name) >= 0);
        }

        assert_se(sd_netlink_message_open_container(m, IFLA_PROP_LIST) >= 0);
        assert_se(sd_netlink_message_append_strv(m, IFLA_ALT_IFNAME, names_in) >= 0);
        assert_se(sd_netlink_message_close_container(m) >= 0);

        rtnl_message_seal(m);
        assert_se(sd_netlink_message_rewind(m) >= 0);

        assert_se(sd_netlink_message_read_strv(m, IFLA_PROP_LIST, IFLA_ALT_IFNAME, &names_out) >= 0);
        assert_se(strv_equal(names_in, names_out));

        assert_se(sd_netlink_message_enter_container(m, IFLA_PROP_LIST) >= 0);
        assert_se(sd_netlink_message_read_string(m, IFLA_ALT_IFNAME, &p) >= 0);
        assert_se(streq(p, "hoge1009"));
        assert_se(sd_netlink_message_exit_container(m) >= 0);
}

int main(void) {
        sd_netlink *rtnl;
        sd_netlink_message *m;
        sd_netlink_message *r;
        const char *string_data;
        int if_loopback;
        uint16_t type;

        test_match();

        test_multiple();

        assert_se(sd_netlink_open(&rtnl) >= 0);
        assert_se(rtnl);

        test_route(rtnl);

        test_message(rtnl);

        test_container(rtnl);
        test_strv(rtnl);

        if_loopback = (int) if_nametoindex("lo");
        assert_se(if_loopback > 0);

        test_async(if_loopback);

        test_pipe(if_loopback);

        test_event_loop(if_loopback);

        test_link_configure(rtnl, if_loopback);

        test_get_addresses(rtnl);

        test_message_link_bridge(rtnl);

        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_GETLINK, if_loopback) >= 0);
        assert_se(m);

        assert_se(sd_netlink_message_get_type(m, &type) >= 0);
        assert_se(type == RTM_GETLINK);

        assert_se(sd_netlink_message_read_string(m, IFLA_IFNAME, &string_data) == -EPERM);

        assert_se(sd_netlink_call(rtnl, m, 0, &r) == 1);
        assert_se(sd_netlink_message_get_type(r, &type) >= 0);
        assert_se(type == RTM_NEWLINK);

        assert_se((r = sd_netlink_message_unref(r)) == NULL);

        assert_se(sd_netlink_call(rtnl, m, -1, &r) == -EPERM);
        assert_se((m = sd_netlink_message_unref(m)) == NULL);
        assert_se((r = sd_netlink_message_unref(r)) == NULL);

        test_link_get(rtnl, if_loopback);
        test_address_get(rtnl, if_loopback);

        assert_se((m = sd_netlink_message_unref(m)) == NULL);
        assert_se((r = sd_netlink_message_unref(r)) == NULL);
        assert_se((rtnl = sd_netlink_unref(rtnl)) == NULL);

        return EXIT_SUCCESS;
}
