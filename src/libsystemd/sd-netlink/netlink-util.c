/* SPDX-License-Identifier: LGPL-2.1+ */

#include <net/if.h>

#include "sd-netlink.h"

#include "netlink-internal.h"
#include "netlink-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "strv.h"

int rtnl_set_link_name(sd_netlink **rtnl, int ifindex, const char *name) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        _cleanup_strv_free_ char **alternative_names = NULL;
        char old_name[IF_NAMESIZE + 1] = {};
        int r;

        assert(rtnl);
        assert(ifindex > 0);
        assert(name);

        if (!ifname_valid(name))
                return -EINVAL;

        r = rtnl_get_link_alternative_names(rtnl, ifindex, &alternative_names);
        if (r < 0)
                log_debug_errno(r, "Failed to get alternative names on network interface %i, ignoring: %m",
                                ifindex);

        if (strv_contains(alternative_names, name)) {
                r = rtnl_delete_link_alternative_names(rtnl, ifindex, STRV_MAKE(name));
                if (r < 0)
                        return log_debug_errno(r, "Failed to remove '%s' from alternative names on network interface %i: %m",
                                               name, ifindex);

                if_indextoname(ifindex, old_name);
        }

        r = sd_rtnl_message_new_link(*rtnl, &message, RTM_SETLINK, ifindex);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_string(message, IFLA_IFNAME, name);
        if (r < 0)
                return r;

        r = sd_netlink_call(*rtnl, message, 0, NULL);
        if (r < 0)
                return r;

        if (!isempty(old_name)) {
                r = rtnl_set_link_alternative_names(rtnl, ifindex, STRV_MAKE(old_name));
                if (r < 0)
                        log_debug_errno(r, "Failed to set '%s' as an alternative name on network interface %i, ignoring: %m",
                                        old_name, ifindex);
        }

        return 0;
}

int rtnl_set_link_properties(sd_netlink **rtnl, int ifindex, const char *alias,
                             const struct ether_addr *mac, uint32_t mtu) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        int r;

        assert(rtnl);
        assert(ifindex > 0);

        if (!alias && !mac && mtu == 0)
                return 0;

        if (!*rtnl) {
                r = sd_netlink_open(rtnl);
                if (r < 0)
                        return r;
        }

        r = sd_rtnl_message_new_link(*rtnl, &message, RTM_SETLINK, ifindex);
        if (r < 0)
                return r;

        if (alias) {
                r = sd_netlink_message_append_string(message, IFLA_IFALIAS, alias);
                if (r < 0)
                        return r;
        }

        if (mac) {
                r = sd_netlink_message_append_ether_addr(message, IFLA_ADDRESS, mac);
                if (r < 0)
                        return r;
        }

        if (mtu != 0) {
                r = sd_netlink_message_append_u32(message, IFLA_MTU, mtu);
                if (r < 0)
                        return r;
        }

        r = sd_netlink_call(*rtnl, message, 0, NULL);
        if (r < 0)
                return r;

        return 0;
}

int rtnl_get_link_alternative_names(sd_netlink **rtnl, int ifindex, char ***ret) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL, *reply = NULL;
        _cleanup_strv_free_ char **names = NULL;
        int r;

        assert(rtnl);
        assert(ifindex > 0);
        assert(ret);

        if (!*rtnl) {
                r = sd_netlink_open(rtnl);
                if (r < 0)
                        return r;
        }

        r = sd_rtnl_message_new_link(*rtnl, &message, RTM_GETLINK, ifindex);
        if (r < 0)
                return r;

        r = sd_netlink_call(*rtnl, message, 0, &reply);
        if (r < 0)
                return r;

        r = sd_netlink_message_read_strv(reply, IFLA_PROP_LIST, IFLA_ALT_IFNAME, &names);
        if (r < 0 && r != -ENODATA)
                return r;

        *ret = TAKE_PTR(names);

        return 0;
}

static int rtnl_update_link_alternative_names(sd_netlink **rtnl, uint16_t nlmsg_type, int ifindex, char * const *alternative_names) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        int r;

        assert(rtnl);
        assert(ifindex > 0);
        assert(IN_SET(nlmsg_type, RTM_NEWLINKPROP, RTM_DELLINKPROP));

        if (strv_isempty(alternative_names))
                return 0;

        if (!*rtnl) {
                r = sd_netlink_open(rtnl);
                if (r < 0)
                        return r;
        }

        r = sd_rtnl_message_new_link(*rtnl, &message, nlmsg_type, ifindex);
        if (r < 0)
                return r;

        r = sd_netlink_message_open_container(message, IFLA_PROP_LIST);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_strv(message, IFLA_ALT_IFNAME, alternative_names);
        if (r < 0)
                return r;

        r = sd_netlink_message_close_container(message);
        if (r < 0)
                return r;

        r = sd_netlink_call(*rtnl, message, 0, NULL);
        if (r < 0)
                return r;

        return 0;
}

int rtnl_set_link_alternative_names(sd_netlink **rtnl, int ifindex, char * const *alternative_names) {
        return rtnl_update_link_alternative_names(rtnl, RTM_NEWLINKPROP, ifindex, alternative_names);
}

int rtnl_delete_link_alternative_names(sd_netlink **rtnl, int ifindex, char * const *alternative_names) {
        return rtnl_update_link_alternative_names(rtnl, RTM_DELLINKPROP, ifindex, alternative_names);
}

int rtnl_resolve_link_alternative_name(sd_netlink **rtnl, const char *name, int *ret) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL, *reply = NULL;
        int r;

        assert(rtnl);
        assert(name);
        assert(ret);

        if (!*rtnl) {
                r = sd_netlink_open(rtnl);
                if (r < 0)
                        return r;
        }

        r = sd_rtnl_message_new_link(*rtnl, &message, RTM_GETLINK, 0);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_string(message, IFLA_ALT_IFNAME, name);
        if (r < 0)
                return r;

        r = sd_netlink_call(*rtnl, message, 0, &reply);
        if (r < 0)
                return r;

        return sd_rtnl_message_link_get_ifindex(reply, ret);
}

int rtnl_message_new_synthetic_error(sd_netlink *rtnl, int error, uint32_t serial, sd_netlink_message **ret) {
        struct nlmsgerr *err;
        int r;

        assert(error <= 0);

        r = message_new(rtnl, ret, NLMSG_ERROR);
        if (r < 0)
                return r;

        (*ret)->hdr->nlmsg_seq = serial;

        err = NLMSG_DATA((*ret)->hdr);

        err->error = error;

        return 0;
}

int rtnl_log_parse_error(int r) {
        return log_error_errno(r, "Failed to parse netlink message: %m");
}

int rtnl_log_create_error(int r) {
        return log_error_errno(r, "Failed to create netlink message: %m");
}
