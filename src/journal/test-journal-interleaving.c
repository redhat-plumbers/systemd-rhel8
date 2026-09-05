/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  Copyright © 2013 Marius Vollmer
***/

#include <fcntl.h>
#include <unistd.h>

#if HAVE_VALGRIND_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

#include "sd-journal.h"

#include "alloc-util.h"
#include "fd-util.h"
#include "journal-file.h"
#include "journal-internal.h"
#include "journal-vacuum.h"
#include "log.h"
#include "parse-util.h"
#include "rm-rf.h"
#include "sigbus.h"
#include "string-util.h"
#include "util.h"

/* This program tests skipping around in a multi-file journal.
 */

static bool arg_keep = false;

_noreturn_ static void log_assert_errno(const char *text, int error, const char *file, int line, const char *func) {
        log_internal(LOG_CRIT, error, file, line, func,
                     "'%s' failed at %s:%u (%s): %m", text, file, line, func);
        abort();
}

#define assert_ret(expr)                                                \
        do {                                                            \
                int _r_ = (expr);                                       \
                if (_unlikely_(_r_ < 0))                                \
                        log_assert_errno(#expr, -_r_, __FILE__, __LINE__, __PRETTY_FUNCTION__); \
        } while (false)

static JournalFile *test_open(const char *name) {
        JournalFile *f;
        assert_ret(journal_file_open(-1, name, O_RDWR|O_CREAT, 0644, true, (uint64_t) -1, false, NULL, NULL, NULL, NULL, &f));
        return f;
}

static void test_close(JournalFile *f) {
        (void) journal_file_close (f);
}

static void append_number(JournalFile *f, int n, uint64_t *seqnum) {
        char *p;
        dual_timestamp ts;
        static dual_timestamp previous_ts = {};
        struct iovec iovec[1];

        dual_timestamp_get(&ts);

        if (ts.monotonic <= previous_ts.monotonic)
                ts.monotonic = previous_ts.monotonic + 1;

        if (ts.realtime <= previous_ts.realtime)
                ts.realtime = previous_ts.realtime + 1;

        previous_ts = ts;

        assert_se(asprintf(&p, "NUMBER=%d", n) >= 0);
        iovec[0].iov_base = p;
        iovec[0].iov_len = strlen(p);
        assert_ret(journal_file_append_entry(f, &ts, NULL, iovec, 1, seqnum, NULL, NULL));
        free(p);
}

static void test_check_number (sd_journal *j, int n) {
        const void *d;
        _cleanup_free_ char *k;
        size_t l;
        int x;

        assert_ret(sd_journal_get_data(j, "NUMBER", &d, &l));
        assert_se(k = strndup(d, l));
        printf("%s\n", k);

        assert_se(safe_atoi(k + 7, &x) >= 0);
        assert_se(n == x);
}

static void test_check_numbers_down (sd_journal *j, int count) {
        int i;

        for (i = 1; i <= count; i++) {
                int r;
                test_check_number(j, i);
                assert_ret(r = sd_journal_next(j));
                if (i == count)
                        assert_se(r == 0);
                else
                        assert_se(r == 1);
        }

}

static void test_check_numbers_up (sd_journal *j, int count) {
        for (int i = count; i >= 1; i--) {
                int r;
                test_check_number(j, i);
                assert_ret(r = sd_journal_previous(j));
                if (i == 1)
                        assert_se(r == 0);
                else
                        assert_se(r == 1);
        }

}

static void setup_sequential(void) {
        JournalFile *one, *two;
        one = test_open("one.journal");
        two = test_open("two.journal");
        append_number(one, 1, NULL);
        append_number(one, 2, NULL);
        append_number(two, 3, NULL);
        append_number(two, 4, NULL);
        test_close(one);
        test_close(two);
}

static void setup_interleaved(void) {
        JournalFile *one, *two;
        one = test_open("one.journal");
        two = test_open("two.journal");
        append_number(one, 1, NULL);
        append_number(two, 2, NULL);
        append_number(one, 3, NULL);
        append_number(two, 4, NULL);
        test_close(one);
        test_close(two);
}

static void test_skip(void (*setup)(void)) {
        char t[] = "/tmp/journal-skip-XXXXXX";
        sd_journal *j;
        int r;

        assert_se(mkdtemp(t));
        assert_se(chdir(t) >= 0);

        setup();

        /* Seek to head, iterate down.
         */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_ret(sd_journal_next(j));
        test_check_numbers_down(j, 4);
        sd_journal_close(j);

        /* Seek to tail, iterate up.
         */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_ret(sd_journal_previous(j));
        test_check_numbers_up(j, 4);
        sd_journal_close(j);

        /* Seek to tail, skip to head, iterate down.
         */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_tail(j));
        assert_ret(r = sd_journal_previous_skip(j, 4));
        assert_se(r == 4);
        test_check_numbers_down(j, 4);
        sd_journal_close(j);

        /* Seek to head, skip to tail, iterate up.
         */
        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_ret(r = sd_journal_next_skip(j, 4));
        assert_se(r == 4);
        test_check_numbers_up(j, 4);
        sd_journal_close(j);

        log_info("Done...");

        if (arg_keep)
                log_info("Not removing %s", t);
        else {
                journal_directory_vacuum(".", 3000000, 0, 0, NULL, true);

                assert_se(rm_rf(t, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
        }

        puts("------------------------------------------------------------");
}

/* Read the first entry so that two.journal becomes the next candidate with LOCATION_SEEK, then simulate
 * vacuuming removing it while it is still open and mapped. */
static void test_remove_unlinked_selected_file_one(bool truncate, bool refresh_stat) {
        char t[] = "/tmp/journal-unlinked-XXXXXX";
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        _cleanup_close_ int fd = -1;
        uint8_t type = OBJECT_UNUSED;
        JournalFile *f;
        struct stat st;

        assert_se(!refresh_stat || truncate);

        assert_se(mkdtemp(t));
        assert_se(chdir(t) >= 0);
        setup_interleaved();

        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_next(j) > 0);
        test_check_number(j, 1);

        assert_se(f = ordered_hashmap_get(j->files, strjoina(t, "/two.journal")));
        assert_se(f->location_type == LOCATION_SEEK);

        if (truncate && !refresh_stat)
                sigbus_install();

        /* Keep the file open and mapped while the name goes away, like vacuuming does. */
        assert_se((fd = open("two.journal", O_WRONLY|O_CLOEXEC)) >= 0);
        assert_se(unlink("two.journal") >= 0);

        if (truncate) {
                /* The lookup only stats the file when the selected entry falls outside the cached size, and
                 * journal_file_fstat() then reports the unlinked file as -EIDRM. Truncating to the selected
                 * offset and refreshing the stat takes that path. Truncating the whole file without
                 * refreshing leaves the cached size stale, so touching the mapping raises SIGBUS and the
                 * lookup fails with -EIO. */
                assert_se(ftruncate(fd, refresh_stat ? f->current_offset : 0) >= 0);
                if (refresh_stat)
                        assert_se(journal_file_fstat(f) == -EIDRM);
                else {
                        assert_se(*(volatile uint8_t*) &f->header->state == STATE_OFFLINE);
                        assert_se(mmap_cache_got_sigbus(f->mmap, f->cache_fd));
                }
        } else {
                /* Emulate a deallocated range, which reads back as zeroes, by zeroing the selected entry's
                 * object type. */
                assert_se(pwrite(fd, &type, sizeof(type), f->current_offset) == (ssize_t) sizeof(type));
                assert_se(fsync(fd) >= 0);
        }

        assert_se(fstat(f->fd, &st) >= 0);
        assert_se(st.st_nlink == 0);
        if (refresh_stat)
                assert_se((uint64_t) st.st_size == f->current_offset);
        else if (truncate)
                assert_se(st.st_size == 0);

        assert_se(sd_journal_next(j) > 0);
        if (truncate && !refresh_stat)
                sigbus_reset();
        test_check_number(j, 3);
        assert_se(!ordered_hashmap_get(j->files, strjoina(t, "/two.journal")));

        if (arg_keep)
                log_info("Not removing %s", t);
        else
                assert_se(rm_rf(t, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
}

static void test_remove_truncated_unlinked_selected_file(void) {
#if HAS_FEATURE_ADDRESS_SANITIZER
        return;
#endif
#if HAVE_VALGRIND_VALGRIND_H
        if (RUNNING_ON_VALGRIND)
                return;
#endif

        test_remove_unlinked_selected_file_one(true, false);
}

/* Corruption in a file that is still linked must stay visible to the caller. Whether libsystemd should
 * skip such a file, or just the corrupt entry, is a separate question; this only pins today's behaviour. */
static void test_keep_linked_selected_file_error(void) {
        char t[] = "/tmp/journal-linked-XXXXXX";
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        _cleanup_close_ int fd = -1;
        JournalFile *f;
        uint8_t type = OBJECT_UNUSED;

        assert_se(mkdtemp(t));
        assert_se(chdir(t) >= 0);
        setup_interleaved();

        assert_ret(sd_journal_open_directory(&j, t, 0));
        assert_ret(sd_journal_seek_head(j));
        assert_se(sd_journal_next(j) > 0);
        test_check_number(j, 1);

        assert_se(f = ordered_hashmap_get(j->files, strjoina(t, "/two.journal")));
        assert_se(f->location_type == LOCATION_SEEK);

        assert_se((fd = open("two.journal", O_WRONLY|O_CLOEXEC)) >= 0);
        assert_se(pwrite(fd, &type, sizeof(type), f->current_offset) == (ssize_t) sizeof(type));
        assert_se(fsync(fd) >= 0);

        assert_se(sd_journal_next(j) == -EBADMSG);
        assert_se(ordered_hashmap_get(j->files, strjoina(t, "/two.journal")));

        if (arg_keep)
                log_info("Not removing %s", t);
        else
                assert_se(rm_rf(t, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
}

static void test_sequence_numbers(void) {

        char t[] = "/tmp/journal-seq-XXXXXX";
        JournalFile *one, *two;
        uint64_t seqnum = 0;
        sd_id128_t seqnum_id;

        assert_se(mkdtemp(t));
        assert_se(chdir(t) >= 0);

        assert_se(journal_file_open(-1, "one.journal", O_RDWR|O_CREAT, 0644,
                                    true, (uint64_t) -1, false, NULL, NULL, NULL, NULL, &one) == 0);

        append_number(one, 1, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 1);
        append_number(one, 2, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 2);

        assert_se(one->header->state == STATE_ONLINE);
        assert_se(!sd_id128_equal(one->header->file_id, one->header->machine_id));
        assert_se(!sd_id128_equal(one->header->file_id, one->header->boot_id));
        assert_se(sd_id128_equal(one->header->file_id, one->header->seqnum_id));

        memcpy(&seqnum_id, &one->header->seqnum_id, sizeof(sd_id128_t));

        assert_se(journal_file_open(-1, "two.journal", O_RDWR|O_CREAT, 0644,
                                    true, (uint64_t) -1, false, NULL, NULL, NULL, one, &two) == 0);

        assert_se(two->header->state == STATE_ONLINE);
        assert_se(!sd_id128_equal(two->header->file_id, one->header->file_id));
        assert_se(sd_id128_equal(one->header->machine_id, one->header->machine_id));
        assert_se(sd_id128_equal(one->header->boot_id, one->header->boot_id));
        assert_se(sd_id128_equal(one->header->seqnum_id, one->header->seqnum_id));

        append_number(two, 3, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 3);
        append_number(two, 4, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 4);

        test_close(two);

        append_number(one, 5, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 5);

        append_number(one, 6, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 6);

        test_close(one);

        /* restart server */
        seqnum = 0;

        assert_se(journal_file_open(-1, "two.journal", O_RDWR, 0,
                                    true, (uint64_t) -1, false, NULL, NULL, NULL, NULL, &two) == 0);

        assert_se(sd_id128_equal(two->header->seqnum_id, seqnum_id));

        append_number(two, 7, &seqnum);
        printf("seqnum=%"PRIu64"\n", seqnum);
        assert_se(seqnum == 5);

        /* So..., here we have the same seqnum in two files with the
         * same seqnum_id. */

        test_close(two);

        log_info("Done...");

        if (arg_keep)
                log_info("Not removing %s", t);
        else {
                journal_directory_vacuum(".", 3000000, 0, 0, NULL, true);

                assert_se(rm_rf(t, REMOVE_ROOT|REMOVE_PHYSICAL) >= 0);
        }
}

int main(int argc, char *argv[]) {
        log_set_max_level(LOG_DEBUG);

        /* journal_file_open requires a valid machine id */
        if (access("/etc/machine-id", F_OK) != 0)
                return EXIT_TEST_SKIP;

        arg_keep = argc > 1;

        test_skip(setup_sequential);
        test_skip(setup_interleaved);

        test_remove_unlinked_selected_file_one(false, false);
        test_remove_truncated_unlinked_selected_file();
        test_remove_unlinked_selected_file_one(true, true);
        test_keep_linked_selected_file_error();

        test_sequence_numbers();

        return 0;
}
