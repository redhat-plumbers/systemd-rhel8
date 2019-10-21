/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libudev.h"

#include "alloc-util.h"
#include "dbus-swap.h"
#include "device.h"
#include "escape.h"
#include "exit-status.h"
#include "fd-util.h"
#include "format-util.h"
#include "fstab-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "special.h"
#include "string-table.h"
#include "string-util.h"
#include "swap.h"
#include "udev-util.h"
#include "unit-name.h"
#include "unit.h"
#include "virt.h"

static const UnitActiveState state_translation_table[_SWAP_STATE_MAX] = {
        [SWAP_DEAD] = UNIT_INACTIVE,
        [SWAP_ACTIVATING] = UNIT_ACTIVATING,
        [SWAP_ACTIVATING_DONE] = UNIT_ACTIVE,
        [SWAP_ACTIVE] = UNIT_ACTIVE,
        [SWAP_DEACTIVATING] = UNIT_DEACTIVATING,
        [SWAP_DEACTIVATING_SIGTERM] = UNIT_DEACTIVATING,
        [SWAP_DEACTIVATING_SIGKILL] = UNIT_DEACTIVATING,
        [SWAP_FAILED] = UNIT_FAILED
};

static int swap_dispatch_timer(sd_event_source *source, usec_t usec, void *userdata);
static int swap_dispatch_io(sd_event_source *source, int fd, uint32_t revents, void *userdata);
static int swap_process_proc_swaps(Manager *m);

static bool SWAP_STATE_WITH_PROCESS(SwapState state) {
        return IN_SET(state,
                      SWAP_ACTIVATING,
                      SWAP_ACTIVATING_DONE,
                      SWAP_DEACTIVATING,
                      SWAP_DEACTIVATING_SIGTERM,
                      SWAP_DEACTIVATING_SIGKILL);
}

static void swap_unset_proc_swaps(Swap *s) {
        assert(s);

        if (!s->from_proc_swaps)
                return;

        s->parameters_proc_swaps.what = mfree(s->parameters_proc_swaps.what);

        s->from_proc_swaps = false;
}

static int swap_set_devnode(Swap *s, const char *devnode) {
        Hashmap *swaps;
        Swap *first;
        int r;

        assert(s);

        r = hashmap_ensure_allocated(&UNIT(s)->manager->swaps_by_devnode, &path_hash_ops);
        if (r < 0)
                return r;

        swaps = UNIT(s)->manager->swaps_by_devnode;

        if (s->devnode) {
                first = hashmap_get(swaps, s->devnode);

                LIST_REMOVE(same_devnode, first, s);
                if (first)
                        hashmap_replace(swaps, first->devnode, first);
                else
                        hashmap_remove(swaps, s->devnode);

                s->devnode = mfree(s->devnode);
        }

        if (devnode) {
                s->devnode = strdup(devnode);
                if (!s->devnode)
                        return -ENOMEM;

                first = hashmap_get(swaps, s->devnode);
                LIST_PREPEND(same_devnode, first, s);

                return hashmap_replace(swaps, first->devnode, first);
        }

        return 0;
}

static void swap_init(Unit *u) {
        Swap *s = SWAP(u);

        assert(s);
        assert(UNIT(s)->load_state == UNIT_STUB);

        s->timeout_usec = u->manager->default_timeout_start_usec;

        s->exec_context.std_output = u->manager->default_std_output;
        s->exec_context.std_error = u->manager->default_std_error;

        s->parameters_proc_swaps.priority = s->parameters_fragment.priority = -1;

        s->control_command_id = _SWAP_EXEC_COMMAND_INVALID;

        u->ignore_on_isolate = true;
}

static void swap_unwatch_control_pid(Swap *s) {
        assert(s);

        if (s->control_pid <= 0)
                return;

        unit_unwatch_pid(UNIT(s), s->control_pid);
        s->control_pid = 0;
}

static void swap_done(Unit *u) {
        Swap *s = SWAP(u);

        assert(s);

        swap_unset_proc_swaps(s);
        swap_set_devnode(s, NULL);

        s->what = mfree(s->what);
        s->parameters_fragment.what = mfree(s->parameters_fragment.what);
        s->parameters_fragment.options = mfree(s->parameters_fragment.options);

        s->exec_runtime = exec_runtime_unref(s->exec_runtime, false);
        exec_command_done_array(s->exec_command, _SWAP_EXEC_COMMAND_MAX);
        s->control_command = NULL;

        dynamic_creds_unref(&s->dynamic_creds);

        swap_unwatch_control_pid(s);

        s->timer_event_source = sd_event_source_unref(s->timer_event_source);
}

static int swap_arm_timer(Swap *s, usec_t usec) {
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
                        swap_dispatch_timer, s);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(s->timer_event_source, "swap-timer");

        return 0;
}

static int swap_add_device_dependencies(Swap *s) {
        assert(s);

        if (!s->what)
                return 0;

        if (!s->from_fragment)
                return 0;

        if (is_device_path(s->what))
                return unit_add_node_dependency(UNIT(s), s->what, MANAGER_IS_SYSTEM(UNIT(s)->manager), UNIT_BINDS_TO, UNIT_DEPENDENCY_FILE);
        else
                /* File based swap devices need to be ordered after
                 * systemd-remount-fs.service, since they might need a
                 * writable file system. */
                return unit_add_dependency_by_name(UNIT(s), UNIT_AFTER, SPECIAL_REMOUNT_FS_SERVICE, NULL, true, UNIT_DEPENDENCY_FILE);
}

static int swap_add_default_dependencies(Swap *s) {
        int r;

        assert(s);

        if (!UNIT(s)->default_dependencies)
                return 0;

        if (!MANAGER_IS_SYSTEM(UNIT(s)->manager))
                return 0;

        if (detect_container() > 0)
                return 0;

        /* swap units generated for the swap dev links are missing the
         * ordering dep against the swap target. */
        r = unit_add_dependency_by_name(UNIT(s), UNIT_BEFORE, SPECIAL_SWAP_TARGET, NULL, true, UNIT_DEPENDENCY_DEFAULT);
        if (r < 0)
                return r;

        return unit_add_two_dependencies_by_name(UNIT(s), UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_UMOUNT_TARGET, NULL, true, UNIT_DEPENDENCY_DEFAULT);
}

static int swap_verify(Swap *s) {
        _cleanup_free_ char *e = NULL;
        int r;

        if (UNIT(s)->load_state != UNIT_LOADED)
                return 0;

        r = unit_name_from_path(s->what, ".swap", &e);
        if (r < 0)
                return log_unit_error_errno(UNIT(s), r, "Failed to generate unit name from path: %m");

        if (!unit_has_name(UNIT(s), e)) {
                log_unit_error(UNIT(s), "Value of What= and unit name do not match, not loading.");
                return -ENOEXEC;
        }

        if (s->exec_context.pam_name && s->kill_context.kill_mode != KILL_CONTROL_GROUP) {
                log_unit_error(UNIT(s), "Unit has PAM enabled. Kill mode must be set to 'control-group'. Refusing to load.");
                return -ENOEXEC;
        }

        return 0;
}

static int swap_load_devnode(Swap *s) {
        _cleanup_(udev_device_unrefp) struct udev_device *d = NULL;
        struct stat st;
        const char *p;
        int r;

        assert(s);

        if (stat(s->what, &st) < 0 || !S_ISBLK(st.st_mode))
                return 0;

        r = udev_device_new_from_stat_rdev(UNIT(s)->manager->udev, &st, &d);
        if (r < 0) {
                log_unit_full(UNIT(s), r == -ENOENT ? LOG_DEBUG : LOG_WARNING, r,
                              "Failed to allocate udev device for swap %s: %m", s->what);
                return 0;
        }

        p = udev_device_get_devnode(d);
        if (!p)
                return 0;

        return swap_set_devnode(s, p);
}

static int swap_load(Unit *u) {
        int r;
        Swap *s = SWAP(u);

        assert(s);
        assert(u->load_state == UNIT_STUB);

        /* Load a .swap file */
        if (SWAP(u)->from_proc_swaps)
                r = unit_load_fragment_and_dropin_optional(u);
        else
                r = unit_load_fragment_and_dropin(u);
        if (r < 0)
                return r;

        if (u->load_state == UNIT_LOADED) {

                if (UNIT(s)->fragment_path)
                        s->from_fragment = true;

                if (!s->what) {
                        if (s->parameters_fragment.what)
                                s->what = strdup(s->parameters_fragment.what);
                        else if (s->parameters_proc_swaps.what)
                                s->what = strdup(s->parameters_proc_swaps.what);
                        else {
                                r = unit_name_to_path(u->id, &s->what);
                                if (r < 0)
                                        return r;
                        }

                        if (!s->what)
                                return -ENOMEM;
                }

                path_simplify(s->what, false);

                if (!UNIT(s)->description) {
                        r = unit_set_description(u, s->what);
                        if (r < 0)
                                return r;
                }

                r = unit_require_mounts_for(UNIT(s), s->what, UNIT_DEPENDENCY_IMPLICIT);
                if (r < 0)
                        return r;

                r = swap_add_device_dependencies(s);
                if (r < 0)
                        return r;

                r = swap_load_devnode(s);
                if (r < 0)
                        return r;

                r = unit_patch_contexts(u);
                if (r < 0)
                        return r;

                r = unit_add_exec_dependencies(u, &s->exec_context);
                if (r < 0)
                        return r;

                r = unit_set_default_slice(u);
                if (r < 0)
                        return r;

                r = swap_add_default_dependencies(s);
                if (r < 0)
                        return r;
        }

        return swap_verify(s);
}

static int swap_setup_unit(
                Manager *m,
                const char *what,
                const char *what_proc_swaps,
                int priority,
                bool set_flags) {

        _cleanup_free_ char *e = NULL;
        bool delete = false;
        Unit *u = NULL;
        int r;
        SwapParameters *p;

        assert(m);
        assert(what);
        assert(what_proc_swaps);

        r = unit_name_from_path(what, ".swap", &e);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to generate unit name from path: %m");

        u = manager_get_unit(m, e);

        if (u &&
            SWAP(u)->from_proc_swaps &&
            !path_equal(SWAP(u)->parameters_proc_swaps.what, what_proc_swaps)) {
                log_error("Swap %s appeared twice with different device paths %s and %s", e, SWAP(u)->parameters_proc_swaps.what, what_proc_swaps);
                return -EEXIST;
        }

        if (!u) {
                delete = true;

                r = unit_new_for_name(m, sizeof(Swap), e, &u);
                if (r < 0)
                        goto fail;

                SWAP(u)->what = strdup(what);
                if (!SWAP(u)->what) {
                        r = -ENOMEM;
                        goto fail;
                }

                unit_add_to_load_queue(u);
        } else
                delete = false;

        p = &SWAP(u)->parameters_proc_swaps;

        if (!p->what) {
                p->what = strdup(what_proc_swaps);
                if (!p->what) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        if (set_flags) {
                SWAP(u)->is_active = true;
                SWAP(u)->just_activated = !SWAP(u)->from_proc_swaps;
        }

        SWAP(u)->from_proc_swaps = true;

        p->priority = priority;

        unit_add_to_dbus_queue(u);
        return 0;

fail:
        log_unit_warning_errno(u, r, "Failed to load swap unit: %m");

        if (delete)
                unit_free(u);

        return r;
}

static int swap_process_new(Manager *m, const char *device, int prio, bool set_flags) {
        _cleanup_(udev_device_unrefp) struct udev_device *d = NULL;
        struct udev_list_entry *item = NULL, *first = NULL;
        const char *dn;
        struct stat st;
        int r;

        assert(m);

        r = swap_setup_unit(m, device, device, prio, set_flags);
        if (r < 0)
                return r;

        /* If this is a block device, then let's add duplicates for
         * all other names of this block device */
        if (stat(device, &st) < 0 || !S_ISBLK(st.st_mode))
                return 0;

        r = udev_device_new_from_stat_rdev(m->udev, &st, &d);
        if (r < 0) {
                log_full_errno(r == -ENOENT ? LOG_DEBUG : LOG_WARNING, r,
                               "Failed to allocate udev device for swap %s: %m", device);
                return 0;
        }

        /* Add the main device node */
        dn = udev_device_get_devnode(d);
        if (dn && !streq(dn, device))
                swap_setup_unit(m, dn, device, prio, set_flags);

        /* Add additional units for all symlinks */
        first = udev_device_get_devlinks_list_entry(d);
        udev_list_entry_foreach(item, first) {
                const char *p;

                /* Don't bother with the /dev/block links */
                p = udev_list_entry_get_name(item);

                if (streq(p, device))
                        continue;

                if (path_startswith(p, "/dev/block/"))
                        continue;

                if (stat(p, &st) >= 0)
                        if (!S_ISBLK(st.st_mode) ||
                            st.st_rdev != udev_device_get_devnum(d))
                                continue;

                swap_setup_unit(m, p, device, prio, set_flags);
        }

        return r;
}

static void swap_set_state(Swap *s, SwapState state) {
        SwapState old_state;
        Swap *other;

        assert(s);

        old_state = s->state;
        s->state = state;

        if (!SWAP_STATE_WITH_PROCESS(state)) {
                s->timer_event_source = sd_event_source_unref(s->timer_event_source);
                swap_unwatch_control_pid(s);
                s->control_command = NULL;
                s->control_command_id = _SWAP_EXEC_COMMAND_INVALID;
        }

        if (state != old_state)
                log_unit_debug(UNIT(s), "Changed %s -> %s", swap_state_to_string(old_state), swap_state_to_string(state));

        unit_notify(UNIT(s), state_translation_table[old_state], state_translation_table[state], 0);

        /* If there other units for the same device node have a job
           queued it might be worth checking again if it is runnable
           now. This is necessary, since swap_start() refuses
           operation with EAGAIN if there's already another job for
           the same device node queued. */
        LIST_FOREACH_OTHERS(same_devnode, other, s)
                if (UNIT(other)->job)
                        job_add_to_run_queue(UNIT(other)->job);
}

static int swap_coldplug(Unit *u) {
        Swap *s = SWAP(u);
        SwapState new_state = SWAP_DEAD;
        int r;

        assert(s);
        assert(s->state == SWAP_DEAD);

        if (s->deserialized_state != s->state)
                new_state = s->deserialized_state;
        else if (s->from_proc_swaps)
                new_state = SWAP_ACTIVE;

        if (new_state == s->state)
                return 0;

        if (s->control_pid > 0 &&
            pid_is_unwaited(s->control_pid) &&
            SWAP_STATE_WITH_PROCESS(new_state)) {

                r = unit_watch_pid(UNIT(s), s->control_pid, false);
                if (r < 0)
                        return r;

                r = swap_arm_timer(s, usec_add(u->state_change_timestamp.monotonic, s->timeout_usec));
                if (r < 0)
                        return r;
        }

        if (!IN_SET(new_state, SWAP_DEAD, SWAP_FAILED)) {
                (void) unit_setup_dynamic_creds(u);
                (void) unit_setup_exec_runtime(u);
        }

        swap_set_state(s, new_state);
        return 0;
}

static void swap_dump(Unit *u, FILE *f, const char *prefix) {
        char buf[FORMAT_TIMESPAN_MAX];
        Swap *s = SWAP(u);
        SwapParameters *p;

        assert(s);
        assert(f);

        if (s->from_proc_swaps)
                p = &s->parameters_proc_swaps;
        else if (s->from_fragment)
                p = &s->parameters_fragment;
        else
                p = NULL;

        fprintf(f,
                "%sSwap State: %s\n"
                "%sResult: %s\n"
                "%sWhat: %s\n"
                "%sFrom /proc/swaps: %s\n"
                "%sFrom fragment: %s\n",
                prefix, swap_state_to_string(s->state),
                prefix, swap_result_to_string(s->result),
                prefix, s->what,
                prefix, yes_no(s->from_proc_swaps),
                prefix, yes_no(s->from_fragment));

        if (s->devnode)
                fprintf(f, "%sDevice Node: %s\n", prefix, s->devnode);

        if (p)
                fprintf(f,
                        "%sPriority: %i\n"
                        "%sOptions: %s\n",
                        prefix, p->priority,
                        prefix, strempty(p->options));

        fprintf(f,
                "%sTimeoutSec: %s\n",
                prefix, format_timespan(buf, sizeof(buf), s->timeout_usec, USEC_PER_SEC));

        if (s->control_pid > 0)
                fprintf(f,
                        "%sControl PID: "PID_FMT"\n",
                        prefix, s->control_pid);

        exec_context_dump(&s->exec_context, f, prefix);
        kill_context_dump(&s->kill_context, f, prefix);
        cgroup_context_dump(&s->cgroup_context, f, prefix);
}

static int swap_spawn(Swap *s, ExecCommand *c, pid_t *_pid) {

        ExecParameters exec_params = {
                .flags     = EXEC_APPLY_SANDBOXING|EXEC_APPLY_CHROOT|EXEC_APPLY_TTY_STDIN,
                .stdin_fd  = -1,
                .stdout_fd = -1,
                .stderr_fd = -1,
                .exec_fd   = -1,
        };
        pid_t pid;
        int r;

        assert(s);
        assert(c);
        assert(_pid);

        r = unit_prepare_exec(UNIT(s));
        if (r < 0)
                return r;

        r = swap_arm_timer(s, usec_add(now(CLOCK_MONOTONIC), s->timeout_usec));
        if (r < 0)
                goto fail;

        unit_set_exec_params(UNIT(s), &exec_params);

        r = exec_spawn(UNIT(s),
                       c,
                       &s->exec_context,
                       &exec_params,
                       s->exec_runtime,
                       &s->dynamic_creds,
                       &pid);
        if (r < 0)
                goto fail;

        r = unit_watch_pid(UNIT(s), pid, true);
        if (r < 0)
                goto fail;

        *_pid = pid;

        return 0;

fail:
        s->timer_event_source = sd_event_source_unref(s->timer_event_source);

        return r;
}

static void swap_enter_dead(Swap *s, SwapResult f) {
        assert(s);

        if (s->result == SWAP_SUCCESS)
                s->result = f;

        if (s->result != SWAP_SUCCESS)
                log_unit_warning(UNIT(s), "Failed with result '%s'.", swap_result_to_string(s->result));

        swap_set_state(s, s->result != SWAP_SUCCESS ? SWAP_FAILED : SWAP_DEAD);

        s->exec_runtime = exec_runtime_unref(s->exec_runtime, true);

        exec_context_destroy_runtime_directory(&s->exec_context, UNIT(s)->manager->prefix[EXEC_DIRECTORY_RUNTIME]);

        unit_unref_uid_gid(UNIT(s), true);

        dynamic_creds_destroy(&s->dynamic_creds);
}

static void swap_enter_active(Swap *s, SwapResult f) {
        assert(s);

        if (s->result == SWAP_SUCCESS)
                s->result = f;

        swap_set_state(s, SWAP_ACTIVE);
}

static void swap_enter_dead_or_active(Swap *s, SwapResult f) {
        assert(s);

        if (s->from_proc_swaps)
                swap_enter_active(s, f);
        else
                swap_enter_dead(s, f);
}

static void swap_enter_signal(Swap *s, SwapState state, SwapResult f) {
        int r;
        KillOperation kop;

        assert(s);

        if (s->result == SWAP_SUCCESS)
                s->result = f;

        if (state == SWAP_DEACTIVATING_SIGTERM)
                kop = KILL_TERMINATE;
        else
                kop = KILL_KILL;

        r = unit_kill_context(UNIT(s), &s->kill_context, kop, -1, s->control_pid, false);
        if (r < 0)
                goto fail;

        if (r > 0) {
                r = swap_arm_timer(s, usec_add(now(CLOCK_MONOTONIC), s->timeout_usec));
                if (r < 0)
                        goto fail;

                swap_set_state(s, state);
        } else if (state == SWAP_DEACTIVATING_SIGTERM && s->kill_context.send_sigkill)
                swap_enter_signal(s, SWAP_DEACTIVATING_SIGKILL, SWAP_SUCCESS);
        else
                swap_enter_dead_or_active(s, SWAP_SUCCESS);

        return;

fail:
        log_unit_warning_errno(UNIT(s), r, "Failed to kill processes: %m");
        swap_enter_dead_or_active(s, SWAP_FAILURE_RESOURCES);
}

static void swap_enter_activating(Swap *s) {
        _cleanup_free_ char *opts = NULL;
        int r;

        assert(s);

        unit_warn_leftover_processes(UNIT(s));

        s->control_command_id = SWAP_EXEC_ACTIVATE;
        s->control_command = s->exec_command + SWAP_EXEC_ACTIVATE;

        if (s->from_fragment) {
                int priority = -1;

                r = fstab_find_pri(s->parameters_fragment.options, &priority);
                if (r < 0)
                        log_warning_errno(r, "Failed to parse swap priority \"%s\", ignoring: %m", s->parameters_fragment.options);
                else if (r == 1 && s->parameters_fragment.priority >= 0)
                        log_warning("Duplicate swap priority configuration by Priority and Options fields.");

                if (r <= 0 && s->parameters_fragment.priority >= 0) {
                        if (s->parameters_fragment.options)
                                r = asprintf(&opts, "%s,pri=%i", s->parameters_fragment.options, s->parameters_fragment.priority);
                        else
                                r = asprintf(&opts, "pri=%i", s->parameters_fragment.priority);
                        if (r < 0)
                                goto fail;
                }
        }

        r = exec_command_set(s->control_command, "/sbin/swapon", NULL);
        if (r < 0)
                goto fail;

        if (s->parameters_fragment.options || opts) {
                r = exec_command_append(s->control_command, "-o",
                                opts ? : s->parameters_fragment.options, NULL);
                if (r < 0)
                        goto fail;
        }

        r = exec_command_append(s->control_command, s->what, NULL);
        if (r < 0)
                goto fail;

        swap_unwatch_control_pid(s);

        r = swap_spawn(s, s->control_command, &s->control_pid);
        if (r < 0)
                goto fail;

        swap_set_state(s, SWAP_ACTIVATING);

        return;

fail:
        log_unit_warning_errno(UNIT(s), r, "Failed to run 'swapon' task: %m");
        swap_enter_dead_or_active(s, SWAP_FAILURE_RESOURCES);
}

static void swap_enter_deactivating(Swap *s) {
        int r;

        assert(s);

        s->control_command_id = SWAP_EXEC_DEACTIVATE;
        s->control_command = s->exec_command + SWAP_EXEC_DEACTIVATE;

        r = exec_command_set(s->control_command,
                             "/sbin/swapoff",
                             s->what,
                             NULL);
        if (r < 0)
                goto fail;

        swap_unwatch_control_pid(s);

        r = swap_spawn(s, s->control_command, &s->control_pid);
        if (r < 0)
                goto fail;

        swap_set_state(s, SWAP_DEACTIVATING);

        return;

fail:
        log_unit_warning_errno(UNIT(s), r, "Failed to run 'swapoff' task: %m");
        swap_enter_dead_or_active(s, SWAP_FAILURE_RESOURCES);
}

static int swap_start(Unit *u) {
        Swap *s = SWAP(u), *other;
        int r;

        assert(s);

        /* We cannot fulfill this request right now, try again later please! */
        if (IN_SET(s->state,
                   SWAP_DEACTIVATING,
                   SWAP_DEACTIVATING_SIGTERM,
                   SWAP_DEACTIVATING_SIGKILL))
                return -EAGAIN;

        /* Already on it! */
        if (s->state == SWAP_ACTIVATING)
                return 0;

        assert(IN_SET(s->state, SWAP_DEAD, SWAP_FAILED));

        if (detect_container() > 0)
                return -EPERM;

        /* If there's a job for another swap unit for the same node
         * running, then let's not dispatch this one for now, and wait
         * until that other job has finished. */
        LIST_FOREACH_OTHERS(same_devnode, other, s)
                if (UNIT(other)->job && UNIT(other)->job->state == JOB_RUNNING)
                        return -EAGAIN;

        r = unit_start_limit_test(u);
        if (r < 0) {
                swap_enter_dead(s, SWAP_FAILURE_START_LIMIT_HIT);
                return r;
        }

        r = unit_acquire_invocation_id(u);
        if (r < 0)
                return r;

        s->result = SWAP_SUCCESS;

        u->reset_accounting = true;

        swap_enter_activating(s);
        return 1;
}

static int swap_stop(Unit *u) {
        Swap *s = SWAP(u);

        assert(s);

        switch (s->state) {

        case SWAP_DEACTIVATING:
        case SWAP_DEACTIVATING_SIGTERM:
        case SWAP_DEACTIVATING_SIGKILL:
                /* Already on it */
                return 0;

        case SWAP_ACTIVATING:
        case SWAP_ACTIVATING_DONE:
                /* There's a control process pending, directly enter kill mode */
                swap_enter_signal(s, SWAP_DEACTIVATING_SIGTERM, SWAP_SUCCESS);
                return 0;

        case SWAP_ACTIVE:
                if (detect_container() > 0)
                        return -EPERM;

                swap_enter_deactivating(s);
                return 1;

        default:
                assert_not_reached("Unexpected state.");
        }
}

static int swap_serialize(Unit *u, FILE *f, FDSet *fds) {
        Swap *s = SWAP(u);

        assert(s);
        assert(f);
        assert(fds);

        unit_serialize_item(u, f, "state", swap_state_to_string(s->state));
        unit_serialize_item(u, f, "result", swap_result_to_string(s->result));

        if (s->control_pid > 0)
                unit_serialize_item_format(u, f, "control-pid", PID_FMT, s->control_pid);

        if (s->control_command_id >= 0)
                unit_serialize_item(u, f, "control-command", swap_exec_command_to_string(s->control_command_id));

        return 0;
}

static int swap_deserialize_item(Unit *u, const char *key, const char *value, FDSet *fds) {
        Swap *s = SWAP(u);

        assert(s);
        assert(fds);

        if (streq(key, "state")) {
                SwapState state;

                state = swap_state_from_string(value);
                if (state < 0)
                        log_unit_debug(u, "Failed to parse state value: %s", value);
                else
                        s->deserialized_state = state;
        } else if (streq(key, "result")) {
                SwapResult f;

                f = swap_result_from_string(value);
                if (f < 0)
                        log_unit_debug(u, "Failed to parse result value: %s", value);
                else if (f != SWAP_SUCCESS)
                        s->result = f;
        } else if (streq(key, "control-pid")) {
                pid_t pid;

                if (parse_pid(value, &pid) < 0)
                        log_unit_debug(u, "Failed to parse control-pid value: %s", value);
                else
                        s->control_pid = pid;

        } else if (streq(key, "control-command")) {
                SwapExecCommand id;

                id = swap_exec_command_from_string(value);
                if (id < 0)
                        log_unit_debug(u, "Failed to parse exec-command value: %s", value);
                else {
                        s->control_command_id = id;
                        s->control_command = s->exec_command + id;
                }
        } else
                log_unit_debug(u, "Unknown serialization key: %s", key);

        return 0;
}

_pure_ static UnitActiveState swap_active_state(Unit *u) {
        assert(u);

        return state_translation_table[SWAP(u)->state];
}

_pure_ static const char *swap_sub_state_to_string(Unit *u) {
        assert(u);

        return swap_state_to_string(SWAP(u)->state);
}

_pure_ static bool swap_may_gc(Unit *u) {
        Swap *s = SWAP(u);

        assert(s);

        if (s->from_proc_swaps)
                return false;

        return true;
}

static void swap_sigchld_event(Unit *u, pid_t pid, int code, int status) {
        Swap *s = SWAP(u);
        SwapResult f;

        assert(s);
        assert(pid >= 0);

        if (pid != s->control_pid)
                return;

        /* Let's scan /proc/swaps before we process SIGCHLD. For the reasoning see the similar code in
         * mount.c */
        (void) swap_process_proc_swaps(u->manager);

        s->control_pid = 0;

        if (is_clean_exit(code, status, EXIT_CLEAN_COMMAND, NULL))
                f = SWAP_SUCCESS;
        else if (code == CLD_EXITED)
                f = SWAP_FAILURE_EXIT_CODE;
        else if (code == CLD_KILLED)
                f = SWAP_FAILURE_SIGNAL;
        else if (code == CLD_DUMPED)
                f = SWAP_FAILURE_CORE_DUMP;
        else
                assert_not_reached("Unknown code");

        if (s->result == SWAP_SUCCESS)
                s->result = f;

        if (s->control_command) {
                exec_status_exit(&s->control_command->exec_status, &s->exec_context, pid, code, status);

                s->control_command = NULL;
                s->control_command_id = _SWAP_EXEC_COMMAND_INVALID;
        }

        log_unit_full(u, f == SWAP_SUCCESS ? LOG_DEBUG : LOG_NOTICE, 0,
                      "Swap process exited, code=%s status=%i", sigchld_code_to_string(code), status);

        switch (s->state) {

        case SWAP_ACTIVATING:
        case SWAP_ACTIVATING_DONE:

                if (f == SWAP_SUCCESS || s->from_proc_swaps)
                        swap_enter_active(s, f);
                else
                        swap_enter_dead(s, f);
                break;

        case SWAP_DEACTIVATING:
        case SWAP_DEACTIVATING_SIGKILL:
        case SWAP_DEACTIVATING_SIGTERM:

                swap_enter_dead_or_active(s, f);
                break;

        default:
                assert_not_reached("Uh, control process died at wrong time.");
        }

        /* Notify clients about changed exit status */
        unit_add_to_dbus_queue(u);
}

static int swap_dispatch_timer(sd_event_source *source, usec_t usec, void *userdata) {
        Swap *s = SWAP(userdata);

        assert(s);
        assert(s->timer_event_source == source);

        switch (s->state) {

        case SWAP_ACTIVATING:
        case SWAP_ACTIVATING_DONE:
                log_unit_warning(UNIT(s), "Activation timed out. Stopping.");
                swap_enter_signal(s, SWAP_DEACTIVATING_SIGTERM, SWAP_FAILURE_TIMEOUT);
                break;

        case SWAP_DEACTIVATING:
                log_unit_warning(UNIT(s), "Deactivation timed out. Stopping.");
                swap_enter_signal(s, SWAP_DEACTIVATING_SIGTERM, SWAP_FAILURE_TIMEOUT);
                break;

        case SWAP_DEACTIVATING_SIGTERM:
                if (s->kill_context.send_sigkill) {
                        log_unit_warning(UNIT(s), "Swap process timed out. Killing.");
                        swap_enter_signal(s, SWAP_DEACTIVATING_SIGKILL, SWAP_FAILURE_TIMEOUT);
                } else {
                        log_unit_warning(UNIT(s), "Swap process timed out. Skipping SIGKILL. Ignoring.");
                        swap_enter_dead_or_active(s, SWAP_FAILURE_TIMEOUT);
                }
                break;

        case SWAP_DEACTIVATING_SIGKILL:
                log_unit_warning(UNIT(s), "Swap process still around after SIGKILL. Ignoring.");
                swap_enter_dead_or_active(s, SWAP_FAILURE_TIMEOUT);
                break;

        default:
                assert_not_reached("Timeout at wrong time.");
        }

        return 0;
}

static int swap_load_proc_swaps(Manager *m, bool set_flags) {
        unsigned i;
        int r = 0;

        assert(m);

        rewind(m->proc_swaps);

        (void) fscanf(m->proc_swaps, "%*s %*s %*s %*s %*s\n");

        for (i = 1;; i++) {
                _cleanup_free_ char *dev = NULL, *d = NULL;
                int prio = 0, k;

                k = fscanf(m->proc_swaps,
                           "%ms "  /* device/file */
                           "%*s "  /* type of swap */
                           "%*s "  /* swap size */
                           "%*s "  /* used */
                           "%i\n", /* priority */
                           &dev, &prio);
                if (k != 2) {
                        if (k == EOF)
                                break;

                        log_warning("Failed to parse /proc/swaps:%u.", i);
                        continue;
                }

                if (cunescape(dev, UNESCAPE_RELAX, &d) < 0)
                        return log_oom();

                device_found_node(m, d, DEVICE_FOUND_SWAP, DEVICE_FOUND_SWAP);

                k = swap_process_new(m, d, prio, set_flags);
                if (k < 0)
                        r = k;
        }

        return r;
}

static int swap_process_proc_swaps(Manager *m) {
        Unit *u;
        int r;

        assert(m);

        r = swap_load_proc_swaps(m, true);
        if (r < 0) {
                log_error_errno(r, "Failed to reread /proc/swaps: %m");

                /* Reset flags, just in case, for late calls */
                LIST_FOREACH(units_by_type, u, m->units_by_type[UNIT_SWAP]) {
                        Swap *swap = SWAP(u);

                        swap->is_active = swap->just_activated = false;
                }

                return 0;
        }

        manager_dispatch_load_queue(m);

        LIST_FOREACH(units_by_type, u, m->units_by_type[UNIT_SWAP]) {
                Swap *swap = SWAP(u);

                if (!swap->is_active) {
                        /* This has just been deactivated */

                        swap_unset_proc_swaps(swap);

                        switch (swap->state) {

                        case SWAP_ACTIVE:
                                swap_enter_dead(swap, SWAP_SUCCESS);
                                break;

                        default:
                                /* Fire again */
                                swap_set_state(swap, swap->state);
                                break;
                        }

                        if (swap->what)
                                device_found_node(m, swap->what, 0, DEVICE_FOUND_SWAP);

                } else if (swap->just_activated) {

                        /* New swap entry */

                        switch (swap->state) {

                        case SWAP_DEAD:
                        case SWAP_FAILED:
                                (void) unit_acquire_invocation_id(UNIT(swap));
                                swap_enter_active(swap, SWAP_SUCCESS);
                                break;

                        case SWAP_ACTIVATING:
                                swap_set_state(swap, SWAP_ACTIVATING_DONE);
                                break;

                        default:
                                /* Nothing really changed, but let's
                                 * issue an notification call
                                 * nonetheless, in case somebody is
                                 * waiting for this. */
                                swap_set_state(swap, swap->state);
                                break;
                        }
                }

                /* Reset the flags for later calls */
                swap->is_active = swap->just_activated = false;
        }

        return 1;
}

static int swap_dispatch_io(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;

        assert(m);
        assert(revents & EPOLLPRI);

        return swap_process_proc_swaps(m);
}

static Unit *swap_following(Unit *u) {
        Swap *s = SWAP(u);
        Swap *other, *first = NULL;

        assert(s);

        /* If the user configured the swap through /etc/fstab or
         * a device unit, follow that. */

        if (s->from_fragment)
                return NULL;

        LIST_FOREACH_OTHERS(same_devnode, other, s)
                if (other->from_fragment)
                        return UNIT(other);

        /* Otherwise, make everybody follow the unit that's named after
         * the swap device in the kernel */

        if (streq_ptr(s->what, s->devnode))
                return NULL;

        LIST_FOREACH_AFTER(same_devnode, other, s)
                if (streq_ptr(other->what, other->devnode))
                        return UNIT(other);

        LIST_FOREACH_BEFORE(same_devnode, other, s) {
                if (streq_ptr(other->what, other->devnode))
                        return UNIT(other);

                first = other;
        }

        /* Fall back to the first on the list */
        return UNIT(first);
}

static int swap_following_set(Unit *u, Set **_set) {
        Swap *s = SWAP(u), *other;
        _cleanup_set_free_ Set *set = NULL;
        int r;

        assert(s);
        assert(_set);

        if (LIST_JUST_US(same_devnode, s)) {
                *_set = NULL;
                return 0;
        }

        set = set_new(NULL);
        if (!set)
                return -ENOMEM;

        LIST_FOREACH_OTHERS(same_devnode, other, s) {
                r = set_put(set, other);
                if (r < 0)
                        return r;
        }

        *_set = TAKE_PTR(set);
        return 1;
}

static void swap_shutdown(Manager *m) {
        assert(m);

        m->swap_event_source = sd_event_source_unref(m->swap_event_source);
        m->proc_swaps = safe_fclose(m->proc_swaps);
        m->swaps_by_devnode = hashmap_free(m->swaps_by_devnode);
}

static void swap_enumerate(Manager *m) {
        int r;

        assert(m);

        if (!m->proc_swaps) {
                m->proc_swaps = fopen("/proc/swaps", "re");
                if (!m->proc_swaps) {
                        if (errno == ENOENT)
                                log_debug_errno(errno, "Not swap enabled, skipping enumeration.");
                        else
                                log_warning_errno(errno, "Failed to open /proc/swaps, ignoring: %m");

                        return;
                }

                r = sd_event_add_io(m->event, &m->swap_event_source, fileno(m->proc_swaps), EPOLLPRI, swap_dispatch_io, m);
                if (r < 0) {
                        log_error_errno(r, "Failed to watch /proc/swaps: %m");
                        goto fail;
                }

                /* Dispatch this before we dispatch SIGCHLD, so that
                 * we always get the events from /proc/swaps before
                 * the SIGCHLD of /sbin/swapon. */
                r = sd_event_source_set_priority(m->swap_event_source, SD_EVENT_PRIORITY_NORMAL-10);
                if (r < 0) {
                        log_error_errno(r, "Failed to change /proc/swaps priority: %m");
                        goto fail;
                }

                (void) sd_event_source_set_description(m->swap_event_source, "swap-proc");
        }

        r = swap_load_proc_swaps(m, false);
        if (r < 0)
                goto fail;

        return;

fail:
        swap_shutdown(m);
}

int swap_process_device_new(Manager *m, struct udev_device *dev) {
        struct udev_list_entry *item = NULL, *first = NULL;
        _cleanup_free_ char *e = NULL;
        const char *dn;
        Unit *u;
        int r = 0;

        assert(m);
        assert(dev);

        dn = udev_device_get_devnode(dev);
        if (!dn)
                return 0;

        r = unit_name_from_path(dn, ".swap", &e);
        if (r < 0)
                return r;

        u = manager_get_unit(m, e);
        if (u)
                r = swap_set_devnode(SWAP(u), dn);

        first = udev_device_get_devlinks_list_entry(dev);
        udev_list_entry_foreach(item, first) {
                _cleanup_free_ char *n = NULL;
                int q;

                q = unit_name_from_path(udev_list_entry_get_name(item), ".swap", &n);
                if (q < 0)
                        return q;

                u = manager_get_unit(m, n);
                if (u) {
                        q = swap_set_devnode(SWAP(u), dn);
                        if (q < 0)
                                r = q;
                }
        }

        return r;
}

int swap_process_device_remove(Manager *m, struct udev_device *dev) {
        const char *dn;
        int r = 0;
        Swap *s;

        dn = udev_device_get_devnode(dev);
        if (!dn)
                return 0;

        while ((s = hashmap_get(m->swaps_by_devnode, dn))) {
                int q;

                q = swap_set_devnode(s, NULL);
                if (q < 0)
                        r = q;
        }

        return r;
}

static void swap_reset_failed(Unit *u) {
        Swap *s = SWAP(u);

        assert(s);

        if (s->state == SWAP_FAILED)
                swap_set_state(s, SWAP_DEAD);

        s->result = SWAP_SUCCESS;
}

static int swap_kill(Unit *u, KillWho who, int signo, sd_bus_error *error) {
        return unit_kill_common(u, who, signo, -1, SWAP(u)->control_pid, error);
}

static int swap_get_timeout(Unit *u, usec_t *timeout) {
        Swap *s = SWAP(u);
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

static bool swap_supported(void) {
        static int supported = -1;

        /* If swap support is not available in the kernel, or we are
         * running in a container we don't support swap units, and any
         * attempts to starting one should fail immediately. */

        if (supported < 0)
                supported =
                        access("/proc/swaps", F_OK) >= 0 &&
                        detect_container() <= 0;

        return supported;
}

static int swap_control_pid(Unit *u) {
        Swap *s = SWAP(u);

        assert(s);

        return s->control_pid;
}

static const char* const swap_exec_command_table[_SWAP_EXEC_COMMAND_MAX] = {
        [SWAP_EXEC_ACTIVATE] = "ExecActivate",
        [SWAP_EXEC_DEACTIVATE] = "ExecDeactivate",
};

DEFINE_STRING_TABLE_LOOKUP(swap_exec_command, SwapExecCommand);

static const char* const swap_result_table[_SWAP_RESULT_MAX] = {
        [SWAP_SUCCESS] = "success",
        [SWAP_FAILURE_RESOURCES] = "resources",
        [SWAP_FAILURE_TIMEOUT] = "timeout",
        [SWAP_FAILURE_EXIT_CODE] = "exit-code",
        [SWAP_FAILURE_SIGNAL] = "signal",
        [SWAP_FAILURE_CORE_DUMP] = "core-dump",
        [SWAP_FAILURE_START_LIMIT_HIT] = "start-limit-hit",
};

DEFINE_STRING_TABLE_LOOKUP(swap_result, SwapResult);

const UnitVTable swap_vtable = {
        .object_size = sizeof(Swap),
        .exec_context_offset = offsetof(Swap, exec_context),
        .cgroup_context_offset = offsetof(Swap, cgroup_context),
        .kill_context_offset = offsetof(Swap, kill_context),
        .exec_runtime_offset = offsetof(Swap, exec_runtime),
        .dynamic_creds_offset = offsetof(Swap, dynamic_creds),

        .sections =
                "Unit\0"
                "Swap\0"
                "Install\0",
        .private_section = "Swap",

        .init = swap_init,
        .load = swap_load,
        .done = swap_done,

        .coldplug = swap_coldplug,

        .dump = swap_dump,

        .start = swap_start,
        .stop = swap_stop,

        .kill = swap_kill,

        .get_timeout = swap_get_timeout,

        .serialize = swap_serialize,
        .deserialize_item = swap_deserialize_item,

        .active_state = swap_active_state,
        .sub_state_to_string = swap_sub_state_to_string,

        .may_gc = swap_may_gc,

        .sigchld_event = swap_sigchld_event,

        .reset_failed = swap_reset_failed,

        .control_pid = swap_control_pid,

        .bus_vtable = bus_swap_vtable,
        .bus_set_property = bus_swap_set_property,
        .bus_commit_properties = bus_swap_commit_properties,

        .following = swap_following,
        .following_set = swap_following_set,

        .enumerate = swap_enumerate,
        .shutdown = swap_shutdown,
        .supported = swap_supported,

        .status_message_formats = {
                .starting_stopping = {
                        [0] = "Activating swap %s...",
                        [1] = "Deactivating swap %s...",
                },
                .finished_start_job = {
                        [JOB_DONE]       = "Activated swap %s.",
                        [JOB_FAILED]     = "Failed to activate swap %s.",
                        [JOB_TIMEOUT]    = "Timed out activating swap %s.",
                },
                .finished_stop_job = {
                        [JOB_DONE]       = "Deactivated swap %s.",
                        [JOB_FAILED]     = "Failed deactivating swap %s.",
                        [JOB_TIMEOUT]    = "Timed out deactivating swap %s.",
                },
        },
};
