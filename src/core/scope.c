/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <unistd.h>

#include "alloc-util.h"
#include "dbus-scope.h"
#include "dbus-unit.h"
#include "exit-status.h"
#include "load-dropin.h"
#include "log.h"
#include "scope.h"
#include "special.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"
#include "unit.h"
#include "user-util.h"

static const UnitActiveState state_translation_table[_SCOPE_STATE_MAX] = {
        [SCOPE_DEAD] = UNIT_INACTIVE,
        [SCOPE_START_CHOWN] = UNIT_ACTIVATING,
        [SCOPE_RUNNING] = UNIT_ACTIVE,
        [SCOPE_ABANDONED] = UNIT_ACTIVE,
        [SCOPE_STOP_SIGTERM] = UNIT_DEACTIVATING,
        [SCOPE_STOP_SIGKILL] = UNIT_DEACTIVATING,
        [SCOPE_FAILED] = UNIT_FAILED
};

static int scope_dispatch_timer(sd_event_source *source, usec_t usec, void *userdata);

static void scope_init(Unit *u) {
        Scope *s = SCOPE(u);

        assert(u);
        assert(u->load_state == UNIT_STUB);

        s->timeout_stop_usec = u->manager->default_timeout_stop_usec;
        u->ignore_on_isolate = true;
        s->user = s->group = NULL;
}

static void scope_done(Unit *u) {
        Scope *s = SCOPE(u);

        assert(u);

        s->controller = mfree(s->controller);
        s->controller_track = sd_bus_track_unref(s->controller_track);

        s->timer_event_source = sd_event_source_unref(s->timer_event_source);

        s->user = mfree(s->user);
        s->group = mfree(s->group);
}

static int scope_arm_timer(Scope *s, usec_t usec) {
        int r;

        assert(s);

        if (s->timer_event_source) {
                r = sd_event_source_set_time(s->timer_event_source, usec);
                if (r < 0)
                        return r;

                return sd_event_source_set_enabled(s->timer_event_source, SD_EVENT_ONESHOT);
        }

        if (usec == USEC_INFINITY)
                return 0;

        r = sd_event_add_time(
                        UNIT(s)->manager->event,
                        &s->timer_event_source,
                        CLOCK_MONOTONIC,
                        usec, 0,
                        scope_dispatch_timer, s);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(s->timer_event_source, "scope-timer");

        return 0;
}

static void scope_set_state(Scope *s, ScopeState state) {
        ScopeState old_state;
        assert(s);

        old_state = s->state;
        s->state = state;

        if (!IN_SET(state, SCOPE_STOP_SIGTERM, SCOPE_STOP_SIGKILL, SCOPE_START_CHOWN))
                s->timer_event_source = sd_event_source_unref(s->timer_event_source);

        if (IN_SET(state, SCOPE_DEAD, SCOPE_FAILED)) {
                unit_unwatch_all_pids(UNIT(s));
                unit_dequeue_rewatch_pids(UNIT(s));
        }

        if (state != old_state)
                log_debug("%s changed %s -> %s", UNIT(s)->id, scope_state_to_string(old_state), scope_state_to_string(state));

        unit_notify(UNIT(s), state_translation_table[old_state], state_translation_table[state], 0);
}

static int scope_add_default_dependencies(Scope *s) {
        int r;

        assert(s);

        if (!UNIT(s)->default_dependencies)
                return 0;

        /* Make sure scopes are unloaded on shutdown */
        r = unit_add_two_dependencies_by_name(
                        UNIT(s),
                        UNIT_BEFORE, UNIT_CONFLICTS,
                        SPECIAL_SHUTDOWN_TARGET, NULL, true,
                        UNIT_DEPENDENCY_DEFAULT);
        if (r < 0)
                return r;

        return 0;
}

static int scope_verify(Scope *s) {
        assert(s);

        if (UNIT(s)->load_state != UNIT_LOADED)
                return 0;

        if (set_isempty(UNIT(s)->pids) &&
            !MANAGER_IS_RELOADING(UNIT(s)->manager) &&
            !unit_has_name(UNIT(s), SPECIAL_INIT_SCOPE)) {
                log_unit_error(UNIT(s), "Scope has no PIDs. Refusing.");
                return -ENOENT;
        }

        return 0;
}

static int scope_load_init_scope(Unit *u) {
        assert(u);

        if (!unit_has_name(u, SPECIAL_INIT_SCOPE))
                return 0;

        u->transient = true;
        u->perpetual = true;

        /* init.scope is a bit special, as it has to stick around forever. Because of its special semantics we
         * synthesize it here, instead of relying on the unit file on disk. */

        u->default_dependencies = false;

        /* Prettify things, if we can. */
        if (!u->description)
                u->description = strdup("System and Service Manager");
        if (!u->documentation)
                (void) strv_extend(&u->documentation, "man:systemd(1)");

        return 1;
}

static int scope_load(Unit *u) {
        Scope *s = SCOPE(u);
        int r;

        assert(s);
        assert(u->load_state == UNIT_STUB);

        if (!u->transient && !MANAGER_IS_RELOADING(u->manager))
                /* Refuse to load non-transient scope units, but allow them while reloading. */
                return -ENOENT;

        r = scope_load_init_scope(u);
        if (r < 0)
                return r;
        r = unit_load_fragment_and_dropin_optional(u);
        if (r < 0)
                return r;

        if (u->load_state == UNIT_LOADED) {
                r = unit_patch_contexts(u);
                if (r < 0)
                        return r;

                r = unit_set_default_slice(u);
                if (r < 0)
                        return r;

                r = scope_add_default_dependencies(s);
                if (r < 0)
                        return r;
        }

        return scope_verify(s);
}

static int scope_coldplug(Unit *u) {
        Scope *s = SCOPE(u);
        int r;

        assert(s);
        assert(s->state == SCOPE_DEAD);

        if (s->deserialized_state == s->state)
                return 0;

        if (IN_SET(s->deserialized_state, SCOPE_STOP_SIGKILL, SCOPE_STOP_SIGTERM)) {
                r = scope_arm_timer(s, usec_add(u->state_change_timestamp.monotonic, s->timeout_stop_usec));
                if (r < 0)
                        return r;
        }

        if (!IN_SET(s->deserialized_state, SCOPE_DEAD, SCOPE_FAILED))
                (void) unit_enqueue_rewatch_pids(u);

        bus_scope_track_controller(s);

        scope_set_state(s, s->deserialized_state);
        return 0;
}

static void scope_dump(Unit *u, FILE *f, const char *prefix) {
        Scope *s = SCOPE(u);

        assert(s);
        assert(f);

        fprintf(f,
                "%sScope State: %s\n"
                "%sResult: %s\n",
                prefix, scope_state_to_string(s->state),
                prefix, scope_result_to_string(s->result));

        cgroup_context_dump(&s->cgroup_context, f, prefix);
        kill_context_dump(&s->kill_context, f, prefix);
}

static void scope_enter_dead(Scope *s, ScopeResult f) {
        assert(s);

        if (s->result == SCOPE_SUCCESS)
                s->result = f;

        if (s->result == SCOPE_SUCCESS)
                unit_log_success(UNIT(s));
        else
                unit_log_failure(UNIT(s), scope_result_to_string(s->result));

        scope_set_state(s, s->result != SCOPE_SUCCESS ? SCOPE_FAILED : SCOPE_DEAD);
}

static void scope_enter_signal(Scope *s, ScopeState state, ScopeResult f) {
        bool skip_signal = false;
        int r;

        assert(s);

        if (s->result == SCOPE_SUCCESS)
                s->result = f;

        /* Before sending any signal, make sure we track all members of this cgroup */
        (void) unit_watch_all_pids(UNIT(s));

        /* Also, enqueue a job that we recheck all our PIDs a bit later, given that it's likely some processes have
         * died now */
        (void) unit_enqueue_rewatch_pids(UNIT(s));

        /* If we have a controller set let's ask the controller nicely to terminate the scope, instead of us going
         * directly into SIGTERM berserk mode */
        if (state == SCOPE_STOP_SIGTERM)
                skip_signal = bus_scope_send_request_stop(s) > 0;

        if (skip_signal)
                r = 1; /* wait */
        else {
                r = unit_kill_context(
                                UNIT(s),
                                &s->kill_context,
                                state != SCOPE_STOP_SIGTERM ? KILL_KILL :
                                s->was_abandoned            ? KILL_TERMINATE_AND_LOG :
                                                              KILL_TERMINATE,
                                -1, -1, false);
                if (r < 0)
                        goto fail;
        }

        if (r > 0) {
                r = scope_arm_timer(s, usec_add(now(CLOCK_MONOTONIC), s->timeout_stop_usec));
                if (r < 0)
                        goto fail;

                scope_set_state(s, state);
        } else if (state == SCOPE_STOP_SIGTERM)
                scope_enter_signal(s, SCOPE_STOP_SIGKILL, SCOPE_SUCCESS);
        else
                scope_enter_dead(s, SCOPE_SUCCESS);

        return;

fail:
        log_unit_warning_errno(UNIT(s), r, "Failed to kill processes: %m");

        scope_enter_dead(s, SCOPE_FAILURE_RESOURCES);
}

static int scope_enter_start_chown(Scope *s) {
        Unit *u = UNIT(s);
        pid_t pid;
        int r;

        assert(s);
        assert(s->user);

        r = scope_arm_timer(s, usec_add(now(CLOCK_MONOTONIC), u->manager->default_timeout_start_usec));
        if (r < 0)
                return r;

        r = unit_fork_helper_process(u, "(sd-chown-cgroup)", &pid);
        if (r < 0)
                goto fail;

        if (r == 0) {
                uid_t uid = UID_INVALID;
                gid_t gid = GID_INVALID;

                if (!isempty(s->user)) {
                        const char *user = s->user;

                        r = get_user_creds(&user, &uid, &gid, NULL, NULL);
                        if (r < 0) {
                                log_unit_error_errno(UNIT(s), r, "Failed to resolve user \"%s\": %m", user);
                                _exit(EXIT_USER);
                        }
                }

                if (!isempty(s->group)) {
                        const char *group = s->group;

                        r = get_group_creds(&group, &gid);
                        if (r < 0) {
                                log_unit_error_errno(UNIT(s), r, "Failed to resolve group \"%s\": %m", group);
                                _exit(EXIT_GROUP);
                        }
                }

                r = cg_set_access(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, uid, gid);
                if (r < 0) {
                        log_unit_error_errno(UNIT(s), r, "Failed to adjust control group access: %m");
                        _exit(EXIT_CGROUP);
                }

                _exit(EXIT_SUCCESS);
        }

        r = unit_watch_pid(UNIT(s), pid, true);
        if (r < 0)
                goto fail;

        scope_set_state(s, SCOPE_START_CHOWN);

        return 1;
fail:
        s->timer_event_source = sd_event_source_disable_unref(s->timer_event_source);
        return r;
}

static int scope_enter_running(Scope *s) {
        Unit *u = UNIT(s);
        int r;

        assert(s);

        (void) bus_scope_track_controller(s);

        r = unit_acquire_invocation_id(u);
        if (r < 0)
                return r;

        unit_export_state_files(u);

        r = unit_attach_pids_to_cgroup(u, UNIT(s)->pids, NULL);
        if (r < 0) {
                log_unit_warning_errno(UNIT(s), r, "Failed to add PIDs to scope's control group: %m");
                scope_enter_dead(s, SCOPE_FAILURE_RESOURCES);
                return r;
        }

        s->result = SCOPE_SUCCESS;

        scope_set_state(s, SCOPE_RUNNING);

        /* Start watching the PIDs currently in the scope */
        (void) unit_enqueue_rewatch_pids(UNIT(s));
        return 1;
}

static int scope_start(Unit *u) {
        Scope *s = SCOPE(u);

        assert(s);

        if (unit_has_name(u, SPECIAL_INIT_SCOPE))
                return -EPERM;

        if (s->state == SCOPE_FAILED)
                return -EPERM;

        /* We can't fulfill this right now, please try again later */
        if (IN_SET(s->state, SCOPE_STOP_SIGTERM, SCOPE_STOP_SIGKILL))
                return -EAGAIN;

        assert(s->state == SCOPE_DEAD);

        if (!u->transient && !MANAGER_IS_RELOADING(u->manager))
                return -ENOENT;

        (void) unit_realize_cgroup(u);
        (void) unit_reset_cpu_accounting(u);
        (void) unit_reset_ip_accounting(u);

        /* We check only for User= option to keep behavior consistent with logic for service units,
         * i.e. having 'Delegate=true Group=foo' w/o specifing User= has no effect. */
        if (s->user && unit_cgroup_delegate(u))
                return scope_enter_start_chown(s);

        return scope_enter_running(s);
}

static int scope_stop(Unit *u) {
        Scope *s = SCOPE(u);

        assert(s);

        if (IN_SET(s->state, SCOPE_STOP_SIGTERM, SCOPE_STOP_SIGKILL))
                return 0;

        assert(IN_SET(s->state, SCOPE_RUNNING, SCOPE_ABANDONED));

        scope_enter_signal(s, SCOPE_STOP_SIGTERM, SCOPE_SUCCESS);
        return 1;
}

static void scope_reset_failed(Unit *u) {
        Scope *s = SCOPE(u);

        assert(s);

        if (s->state == SCOPE_FAILED)
                scope_set_state(s, SCOPE_DEAD);

        s->result = SCOPE_SUCCESS;
}

static int scope_kill(Unit *u, KillWho who, int signo, sd_bus_error *error) {
        return unit_kill_common(u, who, signo, -1, -1, error);
}

static int scope_get_timeout(Unit *u, usec_t *timeout) {
        Scope *s = SCOPE(u);
        usec_t t;
        int r;

        if (!s->timer_event_source)
                return 0;

        r = sd_event_source_get_time(s->timer_event_source, &t);
        if (r < 0)
                return r;
        if (t == USEC_INFINITY)
                return 0;

        *timeout = t;
        return 1;
}

static int scope_serialize(Unit *u, FILE *f, FDSet *fds) {
        Scope *s = SCOPE(u);

        assert(s);
        assert(f);
        assert(fds);

        unit_serialize_item(u, f, "state", scope_state_to_string(s->state));
        unit_serialize_item(u, f, "was-abandoned", yes_no(s->was_abandoned));

        if (s->controller)
                unit_serialize_item(u, f, "controller", s->controller);

        return 0;
}

static int scope_deserialize_item(Unit *u, const char *key, const char *value, FDSet *fds) {
        Scope *s = SCOPE(u);
        int r;

        assert(u);
        assert(key);
        assert(value);
        assert(fds);

        if (streq(key, "state")) {
                ScopeState state;

                state = scope_state_from_string(value);
                if (state < 0)
                        log_unit_debug(u, "Failed to parse state value: %s", value);
                else
                        s->deserialized_state = state;

        } else if (streq(key, "was-abandoned")) {
                int k;

                k = parse_boolean(value);
                if (k < 0)
                        log_unit_debug(u, "Failed to parse boolean value: %s", value);
                else
                        s->was_abandoned = k;
        } else if (streq(key, "controller")) {

                r = free_and_strdup(&s->controller, value);
                if (r < 0)
                        log_oom();

        } else
                log_unit_debug(u, "Unknown serialization key: %s", key);

        return 0;
}

static void scope_notify_cgroup_empty_event(Unit *u) {
        Scope *s = SCOPE(u);
        assert(u);

        log_unit_debug(u, "cgroup is empty");

        if (IN_SET(s->state, SCOPE_RUNNING, SCOPE_ABANDONED, SCOPE_STOP_SIGTERM, SCOPE_STOP_SIGKILL))
                scope_enter_dead(s, SCOPE_SUCCESS);
}

static void scope_sigchld_event(Unit *u, pid_t pid, int code, int status) {
        Scope *s = SCOPE(u);

        assert(s);

        if (s->state == SCOPE_START_CHOWN) {
                if (!is_clean_exit(code, status, EXIT_CLEAN_COMMAND, NULL))
                        scope_enter_dead(s, SCOPE_FAILURE_RESOURCES);
                else
                        scope_enter_running(s);
                return;
        }

        /* If we get a SIGCHLD event for one of the processes we were interested in, then we look for others to
         * watch, under the assumption that we'll sooner or later get a SIGCHLD for them, as the original
         * process we watched was probably the parent of them, and they are hence now our children. */

        (void) unit_enqueue_rewatch_pids(u);
}

static int scope_dispatch_timer(sd_event_source *source, usec_t usec, void *userdata) {
        Scope *s = SCOPE(userdata);

        assert(s);
        assert(s->timer_event_source == source);

        switch (s->state) {

        case SCOPE_STOP_SIGTERM:
                if (s->kill_context.send_sigkill) {
                        log_unit_warning(UNIT(s), "Stopping timed out. Killing.");
                        scope_enter_signal(s, SCOPE_STOP_SIGKILL, SCOPE_FAILURE_TIMEOUT);
                } else {
                        log_unit_warning(UNIT(s), "Stopping timed out. Skipping SIGKILL.");
                        scope_enter_dead(s, SCOPE_FAILURE_TIMEOUT);
                }

                break;

        case SCOPE_STOP_SIGKILL:
                log_unit_warning(UNIT(s), "Still around after SIGKILL. Ignoring.");
                scope_enter_dead(s, SCOPE_FAILURE_TIMEOUT);
                break;

        case SCOPE_START_CHOWN:
                log_unit_warning(UNIT(s), "User lookup timed out. Entering failed state.");
                scope_enter_dead(s, SCOPE_FAILURE_TIMEOUT);
                break;

        default:
                assert_not_reached("Timeout at wrong time.");
        }

        return 0;
}

int scope_abandon(Scope *s) {
        assert(s);

        if (unit_has_name(UNIT(s), SPECIAL_INIT_SCOPE))
                return -EPERM;

        if (!IN_SET(s->state, SCOPE_RUNNING, SCOPE_ABANDONED))
                return -ESTALE;

        s->was_abandoned = true;

        s->controller = mfree(s->controller);
        s->controller_track = sd_bus_track_unref(s->controller_track);

        scope_set_state(s, SCOPE_ABANDONED);

        /* The client is no longer watching the remaining processes, so let's step in here, under the assumption that
         * the remaining processes will be sooner or later reassigned to us as parent. */
        (void) unit_enqueue_rewatch_pids(UNIT(s));

        return 0;
}

_pure_ static UnitActiveState scope_active_state(Unit *u) {
        assert(u);

        return state_translation_table[SCOPE(u)->state];
}

_pure_ static const char *scope_sub_state_to_string(Unit *u) {
        assert(u);

        return scope_state_to_string(SCOPE(u)->state);
}

static void scope_enumerate_perpetual(Manager *m) {
        Unit *u;
        int r;

        assert(m);

        /* Let's unconditionally add the "init.scope" special unit
         * that encapsulates PID 1. Note that PID 1 already is in the
         * cgroup for this, we hence just need to allocate the object
         * for it and that's it. */

        u = manager_get_unit(m, SPECIAL_INIT_SCOPE);
        if (!u) {
                r = unit_new_for_name(m, sizeof(Scope), SPECIAL_INIT_SCOPE, &u);
                if (r < 0)  {
                        log_error_errno(r, "Failed to allocate the special " SPECIAL_INIT_SCOPE " unit: %m");
                        return;
                }
        }

        u->transient = true;
        u->perpetual = true;
        SCOPE(u)->deserialized_state = SCOPE_RUNNING;

        unit_add_to_load_queue(u);
        unit_add_to_dbus_queue(u);
}

static const char* const scope_result_table[_SCOPE_RESULT_MAX] = {
        [SCOPE_SUCCESS] = "success",
        [SCOPE_FAILURE_RESOURCES] = "resources",
        [SCOPE_FAILURE_TIMEOUT] = "timeout",
};

DEFINE_STRING_TABLE_LOOKUP(scope_result, ScopeResult);

const UnitVTable scope_vtable = {
        .object_size = sizeof(Scope),
        .cgroup_context_offset = offsetof(Scope, cgroup_context),
        .kill_context_offset = offsetof(Scope, kill_context),

        .sections =
                "Unit\0"
                "Scope\0"
                "Install\0",
        .private_section = "Scope",

        .can_transient = true,
        .can_delegate = true,
        .once_only = true,

        .init = scope_init,
        .load = scope_load,
        .done = scope_done,

        .coldplug = scope_coldplug,

        .dump = scope_dump,

        .start = scope_start,
        .stop = scope_stop,

        .kill = scope_kill,

        .freeze = unit_freeze_vtable_common,
        .thaw = unit_thaw_vtable_common,

        .get_timeout = scope_get_timeout,

        .serialize = scope_serialize,
        .deserialize_item = scope_deserialize_item,

        .active_state = scope_active_state,
        .sub_state_to_string = scope_sub_state_to_string,

        .sigchld_event = scope_sigchld_event,

        .reset_failed = scope_reset_failed,

        .notify_cgroup_empty = scope_notify_cgroup_empty_event,

        .bus_vtable = bus_scope_vtable,
        .bus_set_property = bus_scope_set_property,
        .bus_commit_properties = bus_scope_commit_properties,

        .enumerate_perpetual = scope_enumerate_perpetual,
};
