/* SPDX-License-Identifier: LGPL-2.1+ */

#include "alloc-util.h"
#include "log.h"
#include "macro.h"
#include "string-util.h"
#include "user-util.h"
#include "util.h"
#include "path-util.h"

static void test_uid_to_name_one(uid_t uid, const char *name) {
        _cleanup_free_ char *t = NULL;

        log_info("/* %s("UID_FMT", \"%s\") */", __func__, uid, name);

        assert_se(t = uid_to_name(uid));
        if (!synthesize_nobody() && streq(name, NOBODY_USER_NAME)) {
                log_info("(skipping detailed tests because nobody is not synthesized)");
                return;
        }
        assert_se(streq_ptr(t, name));
}

static void test_gid_to_name_one(gid_t gid, const char *name) {
        _cleanup_free_ char *t = NULL;

        log_info("/* %s("GID_FMT", \"%s\") */", __func__, gid, name);

        assert_se(t = gid_to_name(gid));
        if (!synthesize_nobody() && streq(name, NOBODY_GROUP_NAME)) {
                log_info("(skipping detailed tests because nobody is not synthesized)");
                return;
        }
        assert_se(streq_ptr(t, name));
}

static void test_parse_uid(void) {
        int r;
        uid_t uid;

        log_info("/* %s */", __func__);

        r = parse_uid("0", &uid);
        assert_se(r == 0);
        assert_se(uid == 0);

        r = parse_uid("1", &uid);
        assert_se(r == 0);
        assert_se(uid == 1);

        r = parse_uid("01", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 1);

        r = parse_uid("001", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 1);

        r = parse_uid("100", &uid);
        assert_se(r == 0);
        assert_se(uid == 100);

        r = parse_uid("65535", &uid);
        assert_se(r == -ENXIO);
        assert_se(uid == 100);

        r = parse_uid("0x1234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("0o1234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("0b1234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("+1234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("-1234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid(" 1234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("01234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("001234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("0001234", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("-0", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("+0", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("00", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("000", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);

        r = parse_uid("asdsdas", &uid);
        assert_se(r == -EINVAL);
        assert_se(uid == 100);
}

static void test_uid_ptr(void) {
        log_info("/* %s */", __func__);

        assert_se(UID_TO_PTR(0) != NULL);
        assert_se(UID_TO_PTR(1000) != NULL);

        assert_se(PTR_TO_UID(UID_TO_PTR(0)) == 0);
        assert_se(PTR_TO_UID(UID_TO_PTR(1000)) == 1000);
}

static void test_valid_user_group_name_relaxed(void) {
        log_info("/* %s */", __func__);

        assert_se(!valid_user_group_name(NULL, VALID_USER_RELAX));
        assert_se(!valid_user_group_name("", VALID_USER_RELAX));
        assert_se(!valid_user_group_name("1", VALID_USER_RELAX));
        assert_se(!valid_user_group_name("65535", VALID_USER_RELAX));
        assert_se(!valid_user_group_name("-1", VALID_USER_RELAX));
        assert_se(!valid_user_group_name("foo\nbar", VALID_USER_RELAX));
        assert_se(!valid_user_group_name("0123456789012345678901234567890123456789", VALID_USER_RELAX));
        assert_se(!valid_user_group_name("aaa:bbb", VALID_USER_RELAX|VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name(".aaa:bbb", VALID_USER_RELAX|VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name(".", VALID_USER_RELAX));
        assert_se(!valid_user_group_name("..", VALID_USER_RELAX));

        assert_se(valid_user_group_name("root", VALID_USER_RELAX));
        assert_se(valid_user_group_name("lennart", VALID_USER_RELAX));
        assert_se(valid_user_group_name("LENNART", VALID_USER_RELAX));
        assert_se(valid_user_group_name("_kkk", VALID_USER_RELAX));
        assert_se(valid_user_group_name("kkk-", VALID_USER_RELAX));
        assert_se(valid_user_group_name("kk-k", VALID_USER_RELAX));
        assert_se(valid_user_group_name("eff.eff", VALID_USER_RELAX));
        assert_se(valid_user_group_name("eff.", VALID_USER_RELAX));
        assert_se(valid_user_group_name("-kkk", VALID_USER_RELAX));
        assert_se(valid_user_group_name("rööt", VALID_USER_RELAX));
        assert_se(valid_user_group_name(".eff", VALID_USER_RELAX));
        assert_se(valid_user_group_name(".1", VALID_USER_RELAX));
        assert_se(valid_user_group_name(".65535", VALID_USER_RELAX));
        assert_se(valid_user_group_name(".-1", VALID_USER_RELAX));
        assert_se(valid_user_group_name(".-kkk", VALID_USER_RELAX));
        assert_se(valid_user_group_name(".rööt", VALID_USER_RELAX));
        assert_se(valid_user_group_name("...", VALID_USER_RELAX));

        assert_se(valid_user_group_name("some5", VALID_USER_RELAX));
        assert_se(valid_user_group_name("5some", VALID_USER_RELAX));
        assert_se(valid_user_group_name("INNER5NUMBER", VALID_USER_RELAX));

        assert_se(valid_user_group_name("piff.paff@ad.domain.example", VALID_USER_RELAX));
        assert_se(valid_user_group_name("Dāvis", VALID_USER_RELAX));
}

static void test_valid_user_group_name(void) {
        log_info("/* %s */", __func__);

        assert_se(!valid_user_group_name(NULL, 0));
        assert_se(!valid_user_group_name("", 0));
        assert_se(!valid_user_group_name("1", 0));
        assert_se(!valid_user_group_name("65535", 0));
        assert_se(!valid_user_group_name("-1", 0));
        assert_se(!valid_user_group_name("-kkk", 0));
        assert_se(!valid_user_group_name("rööt", 0));
        assert_se(!valid_user_group_name(".", 0));
        assert_se(!valid_user_group_name(".eff", 0));
        assert_se(!valid_user_group_name("foo\nbar", 0));
        assert_se(!valid_user_group_name("0123456789012345678901234567890123456789", 0));
        assert_se(!valid_user_group_name("aaa:bbb", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name(".", 0));
        assert_se(!valid_user_group_name("..", 0));
        assert_se(!valid_user_group_name("...", 0));
        assert_se(!valid_user_group_name(".1", 0));
        assert_se(!valid_user_group_name(".65535", 0));
        assert_se(!valid_user_group_name(".-1", 0));
        assert_se(!valid_user_group_name(".-kkk", 0));
        assert_se(!valid_user_group_name(".rööt", 0));
        assert_se(!valid_user_group_name(".aaa:bbb", VALID_USER_ALLOW_NUMERIC));

        assert_se(valid_user_group_name("root", 0));
        assert_se(valid_user_group_name("lennart", 0));
        assert_se(valid_user_group_name("LENNART", 0));
        assert_se(valid_user_group_name("_kkk", 0));
        assert_se(valid_user_group_name("kkk-", 0));
        assert_se(valid_user_group_name("kk-k", 0));
        assert_se(!valid_user_group_name("eff.eff", 0));
        assert_se(!valid_user_group_name("eff.", 0));

        assert_se(valid_user_group_name("some5", 0));
        assert_se(!valid_user_group_name("5some", 0));
        assert_se(valid_user_group_name("INNER5NUMBER", 0));

        assert_se(!valid_user_group_name("piff.paff@ad.domain.example", 0));
        assert_se(!valid_user_group_name("Dāvis", 0));
}

static void test_valid_user_group_name_or_numeric_relaxed(void) {
        log_info("/* %s */", __func__);

        assert_se(!valid_user_group_name(NULL, VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name("", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("0", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("1", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("65534", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name("65535", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("65536", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name("-1", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name("foo\nbar", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name("0123456789012345678901234567890123456789", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name("aaa:bbb", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name(".", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(!valid_user_group_name("..", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));

        assert_se(valid_user_group_name("root", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("lennart", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("LENNART", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("_kkk", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("kkk-", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("kk-k", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("-kkk", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("rööt", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name(".eff", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("eff.eff", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("eff.", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("...", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));

        assert_se(valid_user_group_name("some5", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("5some", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("INNER5NUMBER", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));

        assert_se(valid_user_group_name("piff.paff@ad.domain.example", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
        assert_se(valid_user_group_name("Dāvis", VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX));
}

static void test_valid_user_group_name_or_numeric(void) {
        log_info("/* %s */", __func__);

        assert_se(!valid_user_group_name(NULL, VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("0", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("1", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("65534", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("65535", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("65536", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("-1", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("-kkk", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("rööt", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name(".", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("..", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("...", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name(".eff", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("eff.eff", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("eff.", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("foo\nbar", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("0123456789012345678901234567890123456789", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("aaa:bbb", VALID_USER_ALLOW_NUMERIC));

        assert_se(valid_user_group_name("root", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("lennart", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("LENNART", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("_kkk", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("kkk-", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("kk-k", VALID_USER_ALLOW_NUMERIC));

        assert_se(valid_user_group_name("some5", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("5some", VALID_USER_ALLOW_NUMERIC));
        assert_se(valid_user_group_name("INNER5NUMBER", VALID_USER_ALLOW_NUMERIC));

        assert_se(!valid_user_group_name("piff.paff@ad.domain.example", VALID_USER_ALLOW_NUMERIC));
        assert_se(!valid_user_group_name("Dāvis", VALID_USER_ALLOW_NUMERIC));
}

static void test_valid_gecos(void) {
        log_info("/* %s */", __func__);

        assert_se(!valid_gecos(NULL));
        assert_se(valid_gecos(""));
        assert_se(valid_gecos("test"));
        assert_se(valid_gecos("Ümläüt"));
        assert_se(!valid_gecos("In\nvalid"));
        assert_se(!valid_gecos("In:valid"));
}

static void test_valid_home(void) {
        log_info("/* %s */", __func__);

        assert_se(!valid_home(NULL));
        assert_se(!valid_home(""));
        assert_se(!valid_home("."));
        assert_se(!valid_home("/home/.."));
        assert_se(!valid_home("/home/../"));
        assert_se(!valid_home("/home\n/foo"));
        assert_se(!valid_home("./piep"));
        assert_se(!valid_home("piep"));
        assert_se(!valid_home("/home/user:lennart"));

        assert_se(valid_home("/"));
        assert_se(valid_home("/home"));
        assert_se(valid_home("/home/foo"));
}

static void test_get_user_creds_one(const char *id, const char *name, uid_t uid, gid_t gid, const char *home, const char *shell) {
        const char *rhome = NULL;
        const char *rshell = NULL;
        uid_t ruid = UID_INVALID;
        gid_t rgid = GID_INVALID;
        int r;

        log_info("/* %s(\"%s\", \"%s\", "UID_FMT", "GID_FMT", \"%s\", \"%s\") */",
                 __func__, id, name, uid, gid, home, shell);

        r = get_user_creds(&id, &ruid, &rgid, &rhome, &rshell);
        log_info_errno(r, "got \"%s\", "UID_FMT", "GID_FMT", \"%s\", \"%s\": %m",
                       id, ruid, rgid, strnull(rhome), strnull(rshell));
        if (!synthesize_nobody() && streq(name, NOBODY_USER_NAME)) {
                log_info("(skipping detailed tests because nobody is not synthesized)");
                return;
        }
        assert_se(r == 0);
        assert_se(streq_ptr(id, name));
        assert_se(ruid == uid);
        assert_se(rgid == gid);
        assert_se(path_equal(rhome, home));
        assert_se(path_equal(rshell, shell));
}

static void test_get_group_creds_one(const char *id, const char *name, gid_t gid) {
        gid_t rgid = GID_INVALID;
        int r;

        log_info("/* %s(\"%s\", \"%s\", "GID_FMT") */", __func__, id, name, gid);

        r = get_group_creds(&id, &rgid);
        log_info_errno(r, "got \"%s\", "GID_FMT": %m", id, rgid);
        if (!synthesize_nobody() && streq(name, NOBODY_GROUP_NAME)) {
                log_info("(skipping detailed tests because nobody is not synthesized)");
                return;
        }
        assert_se(r == 0);
        assert_se(streq_ptr(id, name));
        assert_se(rgid == gid);
}

int main(int argc, char*argv[]) {
        test_uid_to_name_one(0, "root");
        test_uid_to_name_one(UID_NOBODY, NOBODY_USER_NAME);
        test_uid_to_name_one(0xFFFF, "65535");
        test_uid_to_name_one(0xFFFFFFFF, "4294967295");

        test_gid_to_name_one(0, "root");
        test_gid_to_name_one(GID_NOBODY, NOBODY_GROUP_NAME);
        test_gid_to_name_one(TTY_GID, "tty");
        test_gid_to_name_one(0xFFFF, "65535");
        test_gid_to_name_one(0xFFFFFFFF, "4294967295");

        test_get_user_creds_one("root", "root", 0, 0, "/root", "/bin/sh");
        test_get_user_creds_one("0", "root", 0, 0, "/root", "/bin/sh");
        test_get_user_creds_one(NOBODY_USER_NAME, NOBODY_USER_NAME, UID_NOBODY, GID_NOBODY, "/", "/sbin/nologin");
        test_get_user_creds_one("65534", NOBODY_USER_NAME, UID_NOBODY, GID_NOBODY, "/", "/sbin/nologin");

        test_get_group_creds_one("root", "root", 0);
        test_get_group_creds_one("0", "root", 0);
        test_get_group_creds_one(NOBODY_GROUP_NAME, NOBODY_GROUP_NAME, GID_NOBODY);
        test_get_group_creds_one("65534", NOBODY_GROUP_NAME, GID_NOBODY);

        test_parse_uid();
        test_uid_ptr();

        test_valid_user_group_name_relaxed();
        test_valid_user_group_name();
        test_valid_user_group_name_or_numeric_relaxed();
        test_valid_user_group_name_or_numeric();
        test_valid_gecos();
        test_valid_home();

        return 0;
}
