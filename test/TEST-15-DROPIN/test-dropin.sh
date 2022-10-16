#! /bin/bash

set -e
set -x

clear_unit () {
    local UNIT_NAME="${1:?}"
    systemctl stop "$UNIT_NAME" 2>/dev/null || :
    rm -f  /{etc,run,usr/lib}/systemd/system/"$UNIT_NAME"
    rm -fr /{etc,run,usr/lib}/systemd/system/"$UNIT_NAME".d
    rm -fr /{etc,run,usr/lib}/systemd/system/"$UNIT_NAME".{wants,requires}
    if [[ $UNIT_NAME == *@ ]]; then
        local base="${UNIT_NAME%@*}"
        local suffix="${UNIT_NAME##*.}"
        systemctl stop "$base@"*."$suffix" 2>/dev/null || :
        rm -f  /{etc,run,usr/lib}/systemd/system/"$base@"*."$suffix"
        rm -fr /{etc,run,usr/lib}/systemd/system/"$base@"*."$suffix".d
        rm -fr /{etc,run,usr/lib}/systemd/system/"$base@"*."$suffix".{wants,requires}
    fi
}

clear_units () {
    for u in "$@"; do
        clear_unit "$u"
    done
    systemctl daemon-reload
}

create_service () {
    local SERVICE_NAME="${1:?}"
    clear_units "${SERVICE_NAME}".service

    cat >/etc/systemd/system/"$SERVICE_NAME".service <<EOF
[Unit]
Description=$SERVICE_NAME unit

[Service]
ExecStart=/bin/sleep 100000
EOF
    mkdir -p /{etc,run,usr/lib}/systemd/system/"$SERVICE_NAME".service.d
    mkdir -p /etc/systemd/system/"$SERVICE_NAME".service.{wants,requires}
    mkdir -p /run/systemd/system/"$SERVICE_NAME".service.{wants,requires}
    mkdir -p /usr/lib/systemd/system/"$SERVICE_NAME".service.{wants,requires}
}

create_services () {
    for u in "$@"; do
        create_service "$u"
    done
}

check_ok () {
    [ $# -eq 3 ] || return

    x="$(systemctl show --value -p "$2" "$1")"
    case "$x" in
    *$3*)      return 0 ;;
    *)         return 1
    esac
}

check_ko () {
    ! check_ok "$@"
}

test_basic_dropins () {
    echo "Testing basic dropins..."

    echo "*** test a wants b wants c"
    create_services a b c
    ln -s ../b.service /etc/systemd/system/a.service.wants/
    ln -s ../c.service /etc/systemd/system/b.service.wants/
    check_ok a Wants b.service
    check_ok b Wants c.service

    echo "*** test a wants,requires b"
    create_services a b c
    ln -s ../b.service /etc/systemd/system/a.service.wants/
    ln -s ../b.service /etc/systemd/system/a.service.requires/
    check_ok a Wants b.service
    check_ok a Requires b.service

    echo "*** test a wants nonexistent"
    create_service a
    ln -s ../nonexistent.service /etc/systemd/system/a.service.wants/
    check_ok a Wants nonexistent.service
    systemctl start a
    systemctl stop  a

    echo "*** test a requires nonexistent"
    ln -sf ../nonexistent.service /etc/systemd/system/a.service.requires/
    systemctl daemon-reload
    check_ok a Requires nonexistent.service

    # 'b' is already loaded when 'c' pulls it in via a dropin.
    echo "*** test a,c require b"
    create_services a b c
    ln -sf ../b.service /etc/systemd/system/a.service.requires/
    ln -sf ../b.service /etc/systemd/system/c.service.requires/
    systemctl start a
    check_ok c Requires b.service
    systemctl stop a b

    # 'b'  is already loaded when 'c' pulls it in via an alias dropin.
    echo "*** test a wants alias"
    create_services a b c
    ln -sf c.service /etc/systemd/system/c1.service
    ln -sf ../c.service  /etc/systemd/system/a.service.wants/
    ln -sf ../c1.service /etc/systemd/system/b.service.wants/
    systemctl start a
    check_ok a Wants c.service
    check_ok b Wants c.service
    systemctl stop a c

    echo "*** test service.d/ top level drop-in"
    create_services a b
    check_ko a ExecCondition "/bin/echo a"
    check_ko b ExecCondition "/bin/echo b"
    mkdir -p /usr/lib/systemd/system/service.d
    cat >/usr/lib/systemd/system/service.d/override.conf <<EOF
[Service]
ExecCondition=/bin/echo %n
EOF
    check_ok a ExecCondition "/bin/echo a"
    check_ok b ExecCondition "/bin/echo b"
    rm -rf /usr/lib/systemd/system/service.d

    clear_units {a,b,c}.service
}

test_hierarchical_service_dropins () {
    echo "Testing hierarchical service dropins..."
    echo "*** test service.d/ top level drop-in"
    create_services a-b-c
    check_ko a-b-c ExecCondition "echo service.d"
    check_ko a-b-c ExecCondition "echo a-.service.d"
    check_ko a-b-c ExecCondition "echo a-b-.service.d"
    check_ko a-b-c ExecCondition "echo a-b-c.service.d"

    for dropin in service.d a-.service.d a-b-.service.d a-b-c.service.d; do
        mkdir -p /usr/lib/systemd/system/$dropin
        echo "
[Service]
ExecCondition=echo $dropin
        " >/usr/lib/systemd/system/$dropin/override.conf
        systemctl daemon-reload
        check_ok a-b-c ExecCondition "echo $dropin"

        # Check that we can start a transient service in presence of the drop-ins
        systemd-run --unit a-b-c2.service -p Description='sleepy' sleep infinity

        # The transient setting replaces the default
        check_ok a-b-c2.service Description "sleepy"

        # The override takes precedence for ExecCondition
        # (except the last iteration when it only applies to the other service)
        if [ "$dropin" != "a-b-c.service.d" ]; then
            check_ok a-b-c2.service ExecCondition "echo $dropin"
        fi

        # Check that things are the same after a reload
        systemctl daemon-reload
        check_ok a-b-c2.service Description "sleepy"
        if [ "$dropin" != "a-b-c.service.d" ]; then
            check_ok a-b-c2.service ExecCondition "echo $dropin"
        fi

        systemctl stop a-b-c2.service
    done
    for dropin in service.d a-.service.d a-b-.service.d a-b-c.service.d; do
        rm -rf /usr/lib/systemd/system/$dropin
    done

    clear_units a-b-c.service
}

test_hierarchical_slice_dropins () {
    echo "Testing hierarchical slice dropins..."
    echo "*** test slice.d/ top level drop-in"
    # Slice units don't even need a fragment, so we test the defaults here
    check_ok a-b-c.slice Description "a-b-c.slice"
    check_ok a-b-c.slice MemoryMax "infinity"

    # Test drop-ins
    for dropin in slice.d a-.slice.d a-b-.slice.d a-b-c.slice.d; do
        mkdir -p /usr/lib/systemd/system/$dropin
        echo "
[Slice]
MemoryMax=1000000000
        " >/usr/lib/systemd/system/$dropin/override.conf
        systemctl daemon-reload
        check_ok a-b-c.slice MemoryMax "1000000000"

        busctl call \
               org.freedesktop.systemd1 \
               /org/freedesktop/systemd1 \
               org.freedesktop.systemd1.Manager \
               StartTransientUnit 'ssa(sv)a(sa(sv))' \
               'a-b-c.slice' 'replace' \
               2 \
               'Description' s 'slice too' \
               'MemoryMax' t 1000000002 \
               0

        # The override takes precedence for MemoryMax
        check_ok a-b-c.slice MemoryMax "1000000000"
        # The transient setting replaces the default
        check_ok a-b-c.slice Description "slice too"

        # Check that things are the same after a reload
        systemctl daemon-reload
        check_ok a-b-c.slice MemoryMax "1000000000"
        check_ok a-b-c.slice Description "slice too"

        busctl call \
               org.freedesktop.systemd1 \
               /org/freedesktop/systemd1 \
               org.freedesktop.systemd1.Manager \
               StopUnit 'ss' \
               'a-b-c.slice' 'replace'

        rm /usr/lib/systemd/system/$dropin/override.conf
    done

    # Test unit with a fragment
    echo "
[Slice]
MemoryMax=1000000001
        " >/usr/lib/systemd/system/a-b-c.slice
    systemctl daemon-reload
    check_ok a-b-c.slice MemoryMax "1000000001"

    clear_units a-b-c.slice
}

test_transient_service_dropins () {
    echo "Testing dropins for a transient service..."
    echo "*** test transient service drop-ins"

    mkdir -p /etc/systemd/system/service.d
    mkdir -p /etc/systemd/system/a-.service.d
    mkdir -p /etc/systemd/system/a-b-.service.d
    mkdir -p /etc/systemd/system/a-b-c.service.d

    echo -e '[Service]\nStandardInputText=aaa' >/etc/systemd/system/service.d/drop1.conf
    echo -e '[Service]\nStandardInputText=bbb' >/etc/systemd/system/a-.service.d/drop2.conf
    echo -e '[Service]\nStandardInputText=ccc' >/etc/systemd/system/a-b-.service.d/drop3.conf
    echo -e '[Service]\nStandardInputText=ddd' >/etc/systemd/system/a-b-c.service.d/drop4.conf

    # There's no fragment yet, so this fails
    systemctl cat a-b-c.service && exit 1

    # xxx → eHh4Cg==
    systemd-run --unit a-b-c.service -p StandardInputData=eHh4Cg== sleep infinity

    data=$(systemctl show --value -p StandardInputData a-b-c.service)
    # xxx\naaa\n\bbb\nccc\nddd\n → eHh4…
    test "$data" = "eHh4CmFhYQpiYmIKY2NjCmRkZAo="

    # Do a reload and check again
    systemctl daemon-reload
    data=$(systemctl show --value -p StandardInputData a-b-c.service)
    test "$data" = "eHh4CmFhYQpiYmIKY2NjCmRkZAo="

    clear_units a-b-c.service
    rm /etc/systemd/system/service.d/drop1.conf \
       /etc/systemd/system/a-.service.d/drop2.conf \
       /etc/systemd/system/a-b-.service.d/drop3.conf
}

test_template_dropins () {
    echo "Testing template dropins..."

    create_services foo bar@ yup@

    ln -s /etc/systemd/system/bar@.service /etc/systemd/system/foo.service.wants/bar@1.service
    check_ok foo Wants bar@1.service

    clear_units {foo,bar@,yup@}.service
}

test_alias_dropins () {
    echo "Testing alias dropins..."

    echo "*** test a wants b1 alias of b"
    create_services a b
    ln -sf b.service /etc/systemd/system/b1.service
    ln -sf ../b1.service /etc/systemd/system/a.service.wants/
    check_ok a Wants b.service
    systemctl start a
    systemctl --quiet is-active b
    systemctl stop a b
    rm /etc/systemd/system/b1.service
    clear_units {a,b}.service

    # A weird behavior: the dependencies for 'a' may vary. It can be
    # changed by loading an alias...
    #
    # [1] 'a1' is loaded and then "renamed" into 'a'. 'a1' is therefore
    # part of the names set so all its specific dropins are loaded.
    #
    # [2] 'a' is already loaded. 'a1' is simply only merged into 'a' so
    # none of its dropins are loaded ('y' is missing from the deps).
    echo "*** test 2"
    create_services a x y
    mkdir -p /etc/systemd/system/a1.service.wants/
    ln -sf a.service /etc/systemd/system/a1.service
    ln -sf ../x.service /etc/systemd/system/a.service.wants/
    ln -sf ../y.service /etc/systemd/system/a1.service.wants/
    check_ok a1 Wants x.service # see [1]
    check_ok a1 Wants y.service
    systemctl start a
    check_ok a1 Wants x.service # see [2]
    check_ko a1 Wants y.service
    systemctl stop a x y
    rm /etc/systemd/system/a1.service

    clear_units {a,x,y}.service
}

test_masked_dropins () {
    echo "Testing masked dropins..."

    create_services a b

    # 'b' is masked for both deps
    echo "*** test a wants,requires b is masked"
    ln -sf /dev/null /etc/systemd/system/a.service.wants/b.service
    ln -sf /dev/null /etc/systemd/system/a.service.requires/b.service
    check_ko a Wants b.service
    check_ko a Requires b.service

    # 'a' wants 'b' and 'b' is masked at a lower level
    echo "*** test a wants b, mask override"
    ln -sf ../b.service /etc/systemd/system/a.service.wants/b.service
    ln -sf /dev/null /usr/lib/systemd/system/a.service.wants/b.service
    check_ok a Wants b.service

    # 'a' wants 'b' and 'b' is masked at a higher level
    echo "*** test a wants b, mask"
    ln -sf /dev/null /etc/systemd/system/a.service.wants/b.service
    ln -sf ../b.service /usr/lib/systemd/system/a.service.wants/b.service
    check_ko a Wants b.service

    # 'a' is masked but has an override config file
    echo "*** test a is masked but has an override"
    create_services a b
    ln -sf /dev/null /etc/systemd/system/a.service
    cat >/usr/lib/systemd/system/a.service.d/override.conf <<EOF
[Unit]
After=b.service
EOF
    check_ok a UnitFileState masked

    # 'b1' is an alias for 'b': masking 'b' dep should not influence 'b1' dep
    echo "*** test a wants b, b1, and one is masked"
    create_services a b
    ln -sf b.service /etc/systemd/system/b1.service
    ln -sf /dev/null /etc/systemd/system/a.service.wants/b.service
    ln -sf ../b1.service /usr/lib/systemd/system/a.service.wants/b1.service
    systemctl cat a
    systemctl show -p Wants,Requires a
    systemctl cat b1
    systemctl show -p Wants,Requires b1
    check_ok a Wants b.service
    check_ko a Wants b1.service # the alias does not show up in the list of units
    rm /etc/systemd/system/b1.service

    # 'b1' is an alias for 'b': masking 'b1' should not influence 'b' dep
    echo "*** test a wants b, alias dep is masked"
    create_services a b
    ln -sf b.service /etc/systemd/system/b1.service
    ln -sf /dev/null /etc/systemd/system/a.service.wants/b1.service
    ln -sf ../b.service /usr/lib/systemd/system/a.service.wants/b.service
    check_ok a Wants b.service
    check_ko a Wants b1.service # the alias does not show up in the list of units
    rm /etc/systemd/system/b1.service

    # 'a' has Wants=b.service but also has a masking
    # dropin 'b': 'b' should still be pulled in.
    echo "*** test a wants b both ways"
    create_services a b
    ln -sf /dev/null /etc/systemd/system/a.service.wants/b.service
    cat >/usr/lib/systemd/system/a.service.d/wants-b.conf<<EOF
[Unit]
Wants=b.service
EOF
    check_ok a Wants b.service

    # mask a dropin that points to an nonexistent unit.
    echo "*** test a wants nonexistent is masked"
    create_services a
    ln -sf /dev/null /etc/systemd/system/a.service.requires/nonexistent.service
    ln -sf ../nonexistent.service /usr/lib/systemd/system/a.service.requires/
    check_ko a Requires nonexistent.service

    # 'b' is already loaded when 'c' pulls it in via a dropin but 'b' is
    # masked at a higher level.
    echo "*** test a wants b is masked"
    create_services a b c
    ln -sf ../b.service /etc/systemd/system/a.service.requires/
    ln -sf ../b.service /run/systemd/system/c.service.requires/
    ln -sf /dev/null /etc/systemd/system/c.service.requires/b.service
    systemctl start a
    check_ko c Requires b.service
    systemctl stop a b

    # 'b' is already loaded when 'c' pulls it in via a dropin but 'b' is
    # masked at a lower level.
    echo "*** test a requires b is masked"
    create_services a b c
    ln -sf ../b.service /etc/systemd/system/a.service.requires/
    ln -sf ../b.service /etc/systemd/system/c.service.requires/
    ln -sf /dev/null /run/systemd/system/c.service.requires/b.service
    systemctl start a
    check_ok c Requires b.service
    systemctl stop a b

    # 'a' requires 2 aliases of 'b' and one of them is a mask.
    echo "*** test a requires alias of b, other alias masked"
    create_services a b
    ln -sf b.service /etc/systemd/system/b1.service
    ln -sf b.service /etc/systemd/system/b2.service
    ln -sf /dev/null /etc/systemd/system/a.service.requires/b1.service
    ln -sf ../b1.service /run/systemd/system/a.service.requires/
    ln -sf ../b2.service /usr/lib/systemd/system/a.service.requires/
    check_ok a Requires b

    # Same as above but now 'b' is masked.
    echo "*** test a requires alias of b, b dep masked"
    create_services a b
    ln -sf b.service /etc/systemd/system/b1.service
    ln -sf b.service /etc/systemd/system/b2.service
    ln -sf ../b1.service /run/systemd/system/a.service.requires/
    ln -sf ../b2.service /usr/lib/systemd/system/a.service.requires/
    ln -sf /dev/null /etc/systemd/system/a.service.requires/b.service
    check_ok a Requires b

    clear_units {a,b}.service
}

test_invalid_dropins () {
    echo "Testing invalid dropins..."
    # Assertion failed on earlier versions, command exits unsuccessfully on later versions
    systemctl cat nonexistent@.service || true
    create_services a
    systemctl daemon-reload
    # Assertion failed on earlier versions, command exits unsuccessfully on later versions
    systemctl cat a@.service || true
    systemctl stop a
    clear_units a.service
    return 0
}

test_basic_dropins
test_hierarchical_service_dropins
test_hierarchical_slice_dropins
test_transient_service_dropins
test_template_dropins
test_alias_dropins
test_masked_dropins
test_invalid_dropins

touch /testok
