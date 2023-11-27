#!/usr/bin/env bash
set -e
TEST_DESCRIPTION="Test systemd generators"

# shellcheck source=test/test-functions
. "$TEST_BASE_DIR/test-functions"

test_setup() {
    create_empty_image
    mkdir -p "${TESTDIR:?}/root"
    mount "${LOOPDEV:?}p1" "$TESTDIR/root"

    (
        LOG_LEVEL=5
        # shellcheck disable=SC2046
        eval $(udevadm info --export --query=env --name="${LOOPDEV}p2")

        setup_basic_environment

        # mask some services that we do not want to run in these tests
        ln -fs /dev/null "$initdir/etc/systemd/system/systemd-hwdb-update.service"
        ln -fs /dev/null "$initdir/etc/systemd/system/systemd-journal-catalog-update.service"
        ln -fs /dev/null "$initdir/etc/systemd/system/systemd-networkd.service"
        ln -fs /dev/null "$initdir/etc/systemd/system/systemd-networkd.socket"
        ln -fs /dev/null "$initdir/etc/systemd/system/systemd-resolved.service"
        ln -fs /dev/null "$initdir/etc/systemd/system/systemd-machined.service"

        # setup the testsuite service
        cat >"$initdir/etc/systemd/system/testsuite.service" <<EOF
[Unit]
Description=Testsuite service

[Service]
ExecStart=/bin/bash -x /testsuite.sh
Type=oneshot
StandardOutput=tty
StandardError=tty
NotifyAccess=all
EOF
        cp generator-utils.sh testsuite*.sh "$initdir/"

        setup_testsuite
    ) || return 1
    setup_nspawn_root

    ddebug "umount $TESTDIR/root"
    umount "$TESTDIR/root"
}

do_test "$@"
