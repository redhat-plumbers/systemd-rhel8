#!/bin/bash
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
set -ex
set -o pipefail

test_scope_unpriv_delegation() {
    useradd test ||:
    trap "userdel -r test" RETURN

    systemd-run --uid=test -p User=test -p Delegate=yes --slice workload.slice --unit workload0.scope --scope \
            test -w /sys/fs/cgroup/workload.slice/workload0.scope -a \
            -w /sys/fs/cgroup/workload.slice/workload0.scope/cgroup.procs -a \
            -w /sys/fs/cgroup/workload.slice/workload0.scope/cgroup.subtree_control
}

if grep -q cgroup2 /proc/filesystems ; then
        systemd-run --wait --unit=test0.service -p "DynamicUser=1" -p "Delegate=" \
                    test -w /sys/fs/cgroup/system.slice/test0.service/ -a \
                    -w /sys/fs/cgroup/system.slice/test0.service/cgroup.procs -a \
                    -w /sys/fs/cgroup/system.slice/test0.service/cgroup.subtree_control

        systemd-run --wait --unit=test1.service -p "DynamicUser=1" -p "Delegate=memory pids" \
                    grep memory /sys/fs/cgroup/system.slice/test1.service/cgroup.controllers

        systemd-run --wait --unit=test2.service -p "DynamicUser=1" -p "Delegate=memory pids" \
                    grep pids /sys/fs/cgroup/system.slice/test2.service/cgroup.controllers

        # Check that unprivileged delegation works for scopes
        test_scope_unpriv_delegation
else
        echo "Skipping TEST-19-DELEGATE, as the kernel doesn't actually support cgroupsv2" >&2
fi

echo OK > /testok

exit 0
