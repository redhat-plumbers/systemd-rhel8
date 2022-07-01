#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

setup_idle_action_lock() {
    useradd testuser ||:

    mkdir -p /run/systemd/logind.conf.d/
    cat >/run/systemd/logind.conf.d/idle-action-lock.conf <<EOF
[Login]
IdleAction=lock
IdleActionSec=1s
EOF

    mkdir -p /run/systemd/systemd-logind.service.d/
    cat >/run/systemd/systemd-logind.service.d/debug.conf <<EOF
[Service]
Environment=SYSTEMD_LOG_LEVEL=debug
EOF

    systemctl restart systemd-logind.service
}

teardown_idle_action_lock() {(
    set +ex
    rm -f /run/systemd/logind.conf.d/idle-action-lock.conf
    rm -f /run/systemd/systemd-logind.service.d/debug.conf
    pkill -9 -u "$(id -u testuser)"
    userdel -r testuser
    systemctl restart systemd-logind.service
)}

test_lock_idle_action() {
    if ! command -v expect >/dev/null ; then
        echo >&2 "expect not installed, skiping test ${FUNCNAME[0]}"
        return 0
    fi

    setup_idle_action_lock
    trap teardown_idle_action_lock RETURN

    if loginctl --no-legend | awk '{ print $3; }' | sort -u | grep -q testuser ; then
        echo >&2 "Session of the \'testuser\' is already present."
        return 1
    fi

    # IdleActionSec is set 1s but the accuracy of associated timer is 30s so we
    # need to sleep in worst case for 31s to make sure timer elapsed. We sleep
    # here for 35s to accomodate for any possible scheudling delays.
    cat > /tmp/test.exp <<EOF
spawn systemd-run -G -t -p PAMName=login -p User=testuser bash
send "sleep 35\r"
send "echo foobar\r"
send "sleep 35\r"
send "exit\r"
interact
wait
EOF

    ts="$(date '+%H:%M:%S')"
    busctl --match "type='signal',sender='org.freedesktop.login1',interface='org.freedesktop.login1.Session',member='Lock'" monitor  > dbus.log &

    expect /tmp/test.exp &

    # Sleep a bit to give expect time to spawn systemd-run before we check for
    # the presence of resulting session.
    sleep 2
    if [ "$(loginctl --no-legend | awk '{ print $3; }' | sort -u | grep -c testuser)" != 1 ] ; then
        echo >&2 "\'testuser\' is expected to have exactly one session running."
        return 1
    fi

    wait %2
    sleep 20
    kill %1

    # We slept for 35s , in that interval all sessions should have become idle
    # and "Lock" signal should have been sent out. Then we wrote to tty to make
    # session active again and next we slept for another 35s so sessions have
    # become idle again. 'Lock' signal is sent out for each session, we have at
    # least one session, so minimum of 2 "Lock" signals must have been sent.
    if [ "$(grep -c Member=Lock dbus.log)" -lt 2 ]; then
        echo >&2 "Too few 'Lock' D-Bus signal sent, expected at least 2."
        return 1
    fi

    journalctl -b -u systemd-logind.service --since="$ts" > logind.log
    if [ "$(grep -c 'System idle. Doing lock operation.' logind.log)" -lt 2 ]; then
        echo >&2 "System haven't entered idle state at least 2 times."
        return 1
    fi

    rm -f dbus.log logind.log
}

: >/failed

test_lock_idle_action

touch /testok
rm /failed
