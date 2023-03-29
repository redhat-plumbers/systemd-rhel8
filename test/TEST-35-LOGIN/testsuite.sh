#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

cleanup_test_user() (
    set +ex

    pkill -u "$(id -u logind-test-user)"
    sleep 1
    pkill -KILL -u "$(id -u logind-test-user)"
    userdel -r logind-test-user

    return 0
)

setup_test_user() {
    mkdir -p /var/spool/cron /var/spool/mail /var/run/console
    useradd -m -s /bin/bash logind-test-user
    trap cleanup_test_user EXIT
}

test_enable_debug() {
    mkdir -p /run/systemd/system/systemd-logind.service.d
    cat >/run/systemd/system/systemd-logind.service.d/debug.conf <<EOF
[Service]
Environment=SYSTEMD_LOG_LEVEL=debug
EOF
    systemctl daemon-reload
    systemctl stop systemd-logind.service
}

check_session() (
    set +ex

    local seat session leader_pid

    if [[ $(loginctl --no-legend | grep -c "logind-test-user") != 1 ]]; then
        echo "no session or multiple sessions for logind-test-user." >&2
        return 1
    fi

    seat=$(loginctl --no-legend | grep 'logind-test-user *seat' | awk '{ print $4 }')
    if [[ -z "$seat" ]]; then
        echo "no seat found for user logind-test-user" >&2
        return 1
    fi

    session=$(loginctl --no-legend | awk '$3 == "logind-test-user" { print $1 }')
    if [[ -z "$session" ]]; then
        echo "no session found for user logind-test-user" >&2
        return 1
    fi

    if ! loginctl session-status "$session" | grep -q "Unit: session-${session}\.scope"; then
        echo "cannot find scope unit for session $session" >&2
        return 1
    fi

    leader_pid=$(loginctl session-status "$session" | awk '$1 == "Leader:" { print $2 }')
    if [[ -z "$leader_pid" ]]; then
        echo "cannot found leader process for session $session" >&2
        return 1
    fi

    # cgroup v1: "1:name=systemd:/user.slice/..."; unified hierarchy: "0::/user.slice"
    if ! grep -q -E '(name=systemd|^0:):.*session.*scope' /proc/"$leader_pid"/cgroup; then
        echo "FAIL: process $leader_pid is not in the session cgroup" >&2
        cat /proc/self/cgroup
        return 1
    fi
)

create_session() {
    # login with the test user to start a session
    mkdir -p /run/systemd/system/getty@tty2.service.d
    cat >/run/systemd/system/getty@tty2.service.d/override.conf <<EOF
[Service]
Type=simple
ExecStart=
ExecStart=-/sbin/agetty --autologin logind-test-user --noclear %I $TERM
Restart=no
EOF
    systemctl daemon-reload

    systemctl restart getty@tty2.service

    # check session
    for ((i = 0; i < 30; i++)); do
        (( i != 0 )) && sleep 1
        check_session && break
    done
    check_session
    [[ "$(loginctl --no-legend | awk '$3=="logind-test-user" { print $5 }')" == "tty2" ]]
}

cleanup_session() (
    set +ex

    local uid s

    uid=$(id -u logind-test-user)

    loginctl disable-linger logind-test-user

    systemctl stop getty@tty2.service

    for s in $(loginctl --no-legend list-sessions | awk '$3 == "logind-test-user" { print $1 }'); do
        echo "INFO: stopping session $s"
        loginctl terminate-session "$s"
    done

    loginctl terminate-user logind-test-user

    if ! timeout 30 bash -c "while loginctl --no-legend | grep -q logind-test-user; do sleep 1; done"; then
        echo "WARNING: session for logind-test-user still active, ignoring."
    fi

    pkill -u "$uid"
    sleep 1
    pkill -KILL -u "$uid"

    if ! timeout 30 bash -c "while systemctl is-active --quiet user@${uid}.service; do sleep 1; done"; then
        echo "WARNING: user@${uid}.service is still active, ignoring."
    fi

    if ! timeout 30 bash -c "while systemctl is-active --quiet user-runtime-dir@${uid}.service; do sleep 1; done"; then
        echo "WARNING: user-runtime-dir@${uid}.service is still active, ignoring."
    fi

    if ! timeout 30 bash -c "while systemctl is-active --quiet user-${uid}.slice; do sleep 1; done"; then
        echo "WARNING: user-${uid}.slice is still active, ignoring."
    fi

    rm -rf /run/systemd/system/getty@tty2.service.d
    systemctl daemon-reload

    return 0
)

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

    systemd-analyze cat-config --no-pager systemd/logind.conf
    systemctl restart systemd-logind.service
}

teardown_lock_idle_action() (
    set +eux

    rm -f /run/systemd/logind.conf.d/idle-action-lock.conf
    systemctl restart systemd-logind.service

    cleanup_session

    return 0
)

test_lock_idle_action() {
    local ts

    if [[ ! -c /dev/tty2 ]]; then
        echo "/dev/tty2 does not exist, skipping test ${FUNCNAME[0]}."
        return
    fi

    if loginctl --no-legend | grep -q logind-test-user; then
        echo >&2 "Session of the \'logind-test-user\' is already present."
        exit 1
    fi

    trap teardown_lock_idle_action RETURN

    create_session

    ts="$(date '+%H:%M:%S')"

    mkdir -p /run/systemd/logind.conf.d
    cat >/run/systemd/logind.conf.d/idle-action-lock.conf <<EOF
[Login]
IdleAction=lock
IdleActionSec=1s
EOF
    systemctl restart systemd-logind.service

    # Wait for 35s, in that interval all sessions should have become idle
    # and "Lock" signal should have been sent out. Then we wrote to tty to make
    # session active again and next we slept for another 35s so sessions have
    # become idle again. 'Lock' signal is sent out for each session, we have at
    # least one session, so minimum of 2 "Lock" signals must have been sent.
    timeout 35 bash -c "while [[ \"\$(journalctl -b -u systemd-logind.service --since=$ts | grep -c 'Sent message type=signal .* member=Lock')\" -lt 1 ]]; do sleep 1; done"

    # Wakeup
    touch /dev/tty2

    # Wait again
    timeout 35 bash -c "while [[ \"\$(journalctl -b -u systemd-logind.service --since=$ts | grep -c 'Sent message type=signal .* member=Lock')\" -lt 2 ]]; do sleep 1; done"

    if [[ "$(journalctl -b -u systemd-logind.service --since="$ts" | grep -c 'System idle. Doing lock operation.')" -lt 2 ]]; then
        echo >&2 "System haven't entered idle state at least 2 times."
        exit 1
    fi
}

: >/failed

setup_test_user
test_enable_debug
test_lock_idle_action

touch /testok
rm /failed
