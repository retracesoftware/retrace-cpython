import importlib.util
import json
import os
import queue
import subprocess
import sys
import threading
import time
import _thread
from dataclasses import dataclass


PROBE_MODULE = "retrace"
MASK = (1 << 64) - 1
GOLDEN = 0x9E3779B97F4A7C15
DEBUG = bool(os.environ.get("RETRACE_THREAD_SCHEDULE_DEBUG"))


def debug(message):
    if DEBUG:
        print(message, file=sys.stderr, flush=True)


@dataclass(frozen=True)
class ThreadScheduleConfig:
    name: str
    thread_count: int
    iterations: int
    timeout: float
    switch_interval: float = 1e-6


def digest_events(events):
    acc = 0x123456789ABCDEF0
    for index, tid in enumerate(events):
        word = (((tid + 1) * GOLDEN) ^ (index * 0xBF58476D1CE4E5B9)) & MASK
        acc ^= word
        acc = ((acc << 11) | (acc >> 53)) & MASK
        acc = (acc * 0xD6E8FEB86659FD93 + 0xA5A5A5A5A5A5A5A5) & MASK
    return acc


def apply_delta(stack, delta):
    common = delta[0]
    del stack[common:]
    stack.extend(delta[1:])
    return tuple(stack)


def delta_from_cursor(previous, current):
    common = 0
    limit = min(len(previous), len(current))
    while common < limit and previous[common] == current[common]:
        common += 1
    return (common, *current[common:])


_thread_local = threading.local()


def current_logical_tid():
    return getattr(_thread_local, "tid", None)


def current_schedule_id():
    return getattr(_thread_local, "schedule_id", None)


def thread_in_schedule_control():
    return getattr(_thread_local, "schedule_control_depth", 0) > 0


def enter_schedule_control():
    _thread_local.schedule_control_depth = (
        getattr(_thread_local, "schedule_control_depth", 0) + 1
    )


def exit_schedule_control():
    depth = getattr(_thread_local, "schedule_control_depth", 0)
    _thread_local.schedule_control_depth = max(0, depth - 1)


class ReplayScheduler:
    def __init__(
        self,
        module,
        schedule,
        thread_ids,
        thread_roots,
        *,
        literal_coordinates=False,
        allow_current_before_future_start=False,
        coordinate_space=None,
        timeout=None,
    ):
        self.module = module
        self.space = coordinate_space if coordinate_space is not None else module
        self.schedule = schedule
        self.thread_ids = thread_ids
        self.thread_roots = thread_roots
        self.lock = threading.RLock()
        self.condition = threading.Condition(self.lock)
        self.handoff = module.ThreadHandoff(timeout=timeout)
        self.literal_coordinates = literal_coordinates
        self.allow_current_before_future_start = (
            allow_current_before_future_start)
        self.index = 0
        self.record_cursors = {}
        self.current_tid = None
        self.armed = None
        self.started_threads = set()
        self.done = False
        self.deferred_pause = False
        self.recorded_start_tids = [
            item[1] for item in schedule if item[0] == "start"]
        self.recorded_start_index = 0

    def lock_owned_by_current_thread(self):
        is_owned = getattr(self.lock, "_is_owned", None)
        return bool(is_owned is not None and is_owned())

    def pause_or_defer_current_thread(self):
        if self.lock_owned_by_current_thread():
            debug("defer pause while scheduler lock is held")
            self.deferred_pause = True
            return
        self.pause_current_thread()

    def run_deferred_pause(self):
        while current_schedule_id() is not None:
            with self.lock:
                if not self.deferred_pause:
                    return
                self.deferred_pause = False
            self.pause_current_thread()

    def replay_ident(self, tid):
        return self.thread_ids.get(str(tid), tid)

    def bind_next_recorded_start(self, ident):
        with self.lock:
            if self.recorded_start_index < len(self.recorded_start_tids):
                tid = self.recorded_start_tids[self.recorded_start_index]
                self.recorded_start_index += 1
            else:
                tid = ident
            self.thread_ids[str(tid)] = ident
            self.thread_ids.setdefault(str(ident), ident)
            debug(f"bind start tid={tid!r} ident={ident!r}")
            return tid

    def translate_cursor(self, tid, cursor):
        if self.literal_coordinates:
            return cursor
        if not cursor:
            return cursor
        root = self.thread_roots.get(str(tid))
        if root is None:
            return None
        if isinstance(root, tuple):
            return (*root, *cursor[len(root):])
        return (root, *cursor[1:])

    def cursor_after_delta(self, tid, delta):
        stack = self.record_cursors.setdefault(tid, [])
        return apply_delta(stack, delta)

    def start(self):
        with self.lock:
            self.arm_locked()

    def arm_locked(self):
        if self.armed is not None:
            return

        while self.index < len(self.schedule):
            item = self.schedule[self.index]
            debug(f"arm item={item!r} index={self.index + 1}")

            if item[0] == "start":
                tid = item[1]
                if tid not in self.started_threads:
                    debug(f"wait start tid={tid!r}")
                    return
                self.index += 1
                self.current_tid = tid
                debug(f"start tid={self.current_tid!r}")
                continue

            if item[0] == "resume":
                self.index += 1
                self.current_tid = item[1]
                debug(f"current tid={self.current_tid!r}")
                continue

            if item[0] != "yield":
                raise AssertionError(f"unknown schedule item: {item!r}")

            tid = item[1]
            if (
                not self.literal_coordinates and
                str(tid) not in self.thread_roots
            ):
                debug(f"wait root tid={tid!r}")
                return

            record_cursor = self.cursor_after_delta(tid, item[2])
            replay_cursor = self.translate_cursor(tid, record_cursor)
            if replay_cursor is None:
                debug(f"wait cursor root tid={tid!r}")
                return
            self.armed = (tid, replay_cursor)
            debug(f"call_at tid={tid!r} cursor={replay_cursor!r}")
            self.index += 1
            try:
                self.space.call_at(
                    self.replay_ident(tid),
                    replay_cursor,
                    self.on_call_at,
                    self.on_overshoot,
                )
            except LookupError:
                debug(f"skip stale call_at tid={tid!r}")
                self.armed = None
                continue
            return

        self.done = True
        self.current_tid = None
        debug("schedule done")
        self.space.call_at(None)
        self.handoff.close()

    def target_tid_locked(self):
        if self.current_tid is not None:
            return self.current_tid
        if self.index >= len(self.schedule):
            return None
        item = self.schedule[self.index]
        if item[0] in {"start", "resume", "yield"}:
            return item[1]
        raise AssertionError(f"unknown schedule item: {item!r}")

    def next_future_start_locked(self):
        return (
            self.index < len(self.schedule) and
            self.schedule[self.index][0] == "start" and
            self.schedule[self.index][1] not in self.started_threads
        )

    def waiting_for_future_start_locked(self):
        return self.current_tid is None and self.next_future_start_locked()

    def future_start_blocks_handoff_locked(self):
        return (
            self.next_future_start_locked() and
            not (
                self.allow_current_before_future_start and
                self.current_tid is not None
            )
        )

    def yield_until_turn(self):
        tid = current_schedule_id()
        if tid is None:
            return

        while True:
            with self.lock:
                self.arm_locked()
                if self.done or self.current_tid == tid:
                    debug(f"run tid={tid!r} done={self.done}")
                    return
                target_tid = self.target_tid_locked()
                if target_tid is None or target_tid == tid:
                    return
                if self.next_future_start_locked():
                    debug(
                        f"continue tid={tid!r} waiting for future "
                        f"start target={target_tid!r}"
                    )
                    return
                target_ident = self.replay_ident(target_tid)
                debug(
                    f"wait tid={tid!r} target={target_tid!r} "
                    f"cursor={tuple(self.space.coordinates())!r}"
                )
            self.handoff.to(target_ident)

    def pause_current_thread(self):
        tid = current_schedule_id()
        if tid is None:
            return

        with self.lock:
            if self.armed is None or self.armed[0] != tid:
                debug(f"ignore call_at tid={tid!r} armed={self.armed!r}")
                return
            self.armed = None
            debug(f"pause tid={tid!r}")
            self.current_tid = None
            self.arm_locked()
            close_handoff = self.done
            if close_handoff:
                target_ident = None
            else:
                target_tid = self.target_tid_locked()
                if self.next_future_start_locked():
                    target_ident = None
                elif target_tid is not None and target_tid != tid:
                    target_ident = self.replay_ident(target_tid)
                else:
                    target_ident = None

        if close_handoff:
            debug(f"close handoff tid={tid!r}")
            self.handoff.close()
        elif target_ident is not None:
            debug(f"handoff tid={tid!r} target={target_ident!r}")
            self.handoff.to(target_ident)

    def note_thread_start(self, ident):
        with self.lock:
            debug(f"thread start ident={ident!r}")
            self.thread_ids.setdefault(str(ident), ident)
            self.started_threads.add(ident)
            self.arm_locked()
            self.condition.notify_all()
        self.run_deferred_pause()

    def start_current_thread(self):
        with self.lock:
            if self.done:
                debug(f"thread start skip park ident={_thread.get_ident()!r}")
                return
        debug(f"thread start park ident={_thread.get_ident()!r}")
        self.handoff.start()
        debug(f"thread start run ident={_thread.get_ident()!r}")

    def note_thread_registered(self, ident):
        with self.lock:
            debug(f"thread registered ident={ident!r}")
            self.thread_ids.setdefault(str(ident), ident)
            self.started_threads.add(ident)
            self.arm_locked()
            self.condition.notify_all()
        self.run_deferred_pause()

    def run_until_done(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        with self.condition:
            while True:
                self.arm_locked()
                if self.done:
                    self.handoff.close()
                    return
                target_tid = self.target_tid_locked()
                if (
                    target_tid is not None and
                    not self.future_start_blocks_handoff_locked()
                ):
                    target_ident = self.replay_ident(target_tid)
                    debug(
                        f"initial handoff target_tid={target_tid!r} "
                        f"target_ident={target_ident!r}"
                    )
                    break
                if deadline is None:
                    self.condition.wait(0.01)
                else:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("replay scheduler timed out")
                    self.condition.wait(min(0.01, remaining))

        self.handoff.to(target_ident)
        debug("initial handoff returned")

        with self.lock:
            self.arm_locked()
            if not self.done:
                raise AssertionError("replay handoff returned before done")

    def on_call_at(self):
        debug("call_at callback")
        self.pause_or_defer_current_thread()
        return None

    def on_overshoot(self):
        debug("overshoot callback")
        self.pause_or_defer_current_thread()
        return None

    def close(self):
        self.handoff.close()


class ThreadScheduleController:
    def __init__(self, module, mode, schedule=None):
        self.module = module
        self.mode = mode
        self.schedule = schedule
        self.thread_ids = {}
        self.thread_roots = {}
        self.id_to_tid = {}
        self.record_schedule = []
        self.record_cursors = {}
        self.record_call_at_cursors = {}
        self.lock = threading.RLock()
        self.scheduled_tid = None
        self.record_started_threads = set()
        self.record_resume_count = 0
        self.scheduler = None
        self.started = False

    def register_thread(self, tid):
        _thread_local.tid = tid
        ident = _thread.get_ident()
        _thread_local.schedule_id = ident
        root = tuple(self.module.coordinates())[:2]
        self.thread_ids[str(ident)] = ident
        self.thread_roots[str(tid)] = root
        self.thread_roots[str(ident)] = root
        self.id_to_tid[ident] = tid
        if self.scheduler is not None:
            enter_schedule_control()
            try:
                self.scheduler.note_thread_registered(ident)
            finally:
                exit_schedule_control()
        return ident

    def start(self):
        if self.started:
            raise AssertionError("thread schedule controller already started")
        self.started = True
        if self.mode == "record":
            self.module.callbacks.thread_start = self.record_start
            self.module.callbacks.thread_yield = self.record_yield
            self.module.callbacks.thread_resume = self.record_resume
            return
        if self.mode == "replay":
            self.scheduler = ReplayScheduler(
                self.module,
                self.schedule,
                self.thread_ids,
                self.thread_roots,
            )
            self.module.callbacks.thread_start = self.replay_start
            self.module.callbacks.thread_resume = self.replay_resume
            self.scheduler.start()
            return
        raise AssertionError(f"unknown thread schedule mode: {self.mode!r}")

    def close(self):
        self.module.callbacks.thread_start = None
        self.module.callbacks.thread_yield = None
        self.module.callbacks.thread_resume = None
        self.module.call_at(None)
        if self.scheduler is not None:
            self.scheduler.close()

    def yield_until_turn(self):
        run_disabled = getattr(self.module, "run_disabled", None)
        if self.mode == "replay" and run_disabled is not None:
            run_disabled(self.replay_yield_until_turn)
        else:
            self.replay_yield_until_turn()

    def replay_resume(self):
        if thread_in_schedule_control():
            return
        self.replay_yield_until_turn()

    def replay_start(self):
        if self.scheduler is not None:
            enter_schedule_control()
            try:
                self.scheduler.note_thread_start(_thread.get_ident())
                self.scheduler.start_current_thread()
            finally:
                exit_schedule_control()

    def run_replay(self):
        if self.mode != "replay" or self.scheduler is None:
            return
        enter_schedule_control()
        try:
            self.scheduler.run_until_done()
        finally:
            exit_schedule_control()

    def replay_yield_until_turn(self):
        enter_schedule_control()
        try:
            if self.scheduler is not None:
                self.scheduler.yield_until_turn()
        finally:
            exit_schedule_control()

    def record_yield(self):
        tid = self.id_to_tid.get(_thread.get_ident())
        if (
            tid is None or
            thread_in_schedule_control()
        ):
            return
        with self.lock:
            ident = _thread.get_ident()
            if self.scheduled_tid != ident:
                self.scheduled_tid = ident
                self.record_schedule.append(["resume", ident])
            raw_delta = tuple(self.module.thread_delta())
            raw_cursor = apply_delta(
                self.record_cursors.setdefault(ident, []), raw_delta)
            call_at_cursor = raw_cursor
            call_at_stack = self.record_call_at_cursors.setdefault(ident, [])
            delta = delta_from_cursor(call_at_stack, call_at_cursor)
            self.record_schedule.append(["yield", ident, list(delta)])
            apply_delta(call_at_stack, delta)
            self.scheduled_tid = None

    def record_start(self):
        ident = _thread.get_ident()
        with self.lock:
            self.thread_ids.setdefault(str(ident), ident)
            if ident in self.record_started_threads:
                return
            self.record_started_threads.add(ident)
            self.scheduled_tid = ident
            self.record_schedule.append(["start", ident])

    def record_resume(self):
        tid = self.id_to_tid.get(_thread.get_ident())
        if (
            tid is None or
            thread_in_schedule_control()
        ):
            return
        with self.lock:
            self.record_resume_count += 1

    def result(self, events, config):
        seen = [0] * config.thread_count
        for tid in events:
            seen[tid] += 1
        expected = [config.iterations] * config.thread_count
        assert seen == expected, seen

        result = {
            "events": events,
            "digest": f"{digest_events(events):016x}",
        }
        if self.mode == "record":
            result["schedule"] = self.record_schedule
            result["resume_callbacks"] = self.record_resume_count
        elif self.mode == "replay":
            assert self.scheduler is not None and self.scheduler.done
        return result


def _run_raw_thread(target, args, kwargs):
    done = queue.Queue(maxsize=1)

    def runner():
        try:
            result = target(*args, **kwargs)
        except BaseException as exc:
            done.put((False, exc))
        else:
            done.put((True, result))

    _thread.start_new_thread(runner, ())
    return done


def _get_raw_thread_result(done, timeout):
    try:
        ok, value = done.get(timeout=timeout)
    except queue.Empty as exc:
        raise TimeoutError("scheduled target did not finish") from exc
    if ok:
        return value
    raise value


class FreshScheduleRecorder:
    def __init__(self, module, coordinate_space=None):
        self.module = module
        self.space = coordinate_space if coordinate_space is not None else module
        self.lock = threading.RLock()
        self.schedule = []
        self.scheduled_tid = None
        self.started_threads = set()
        self.record_cursors = {}
        self.call_at_cursors = {}
        self.previous_start = None
        self.previous_yield = None
        self.previous_resume = None
        self.skip_launcher_start = True

    def start(self):
        self.previous_start = self.module.callbacks.thread_start
        self.previous_yield = self.module.callbacks.thread_yield
        self.previous_resume = self.module.callbacks.thread_resume
        set_thread_start = getattr(
            self.module.callbacks, "set_thread_start", None)
        set_thread_yield = getattr(
            self.module.callbacks, "set_thread_yield", None)
        set_thread_resume = getattr(
            self.module.callbacks, "set_thread_resume", None)
        if set_thread_start is not None and self.space is not self.module:
            set_thread_start(self.record_start, self.space)
            set_thread_yield(self.record_yield, self.space)
            set_thread_resume(self.record_resume, self.space)
        else:
            self.module.callbacks.thread_start = self.record_start
            self.module.callbacks.thread_yield = self.record_yield
            self.module.callbacks.thread_resume = self.record_resume

    def close(self):
        set_thread_start = getattr(
            self.module.callbacks, "set_thread_start", None)
        set_thread_yield = getattr(
            self.module.callbacks, "set_thread_yield", None)
        set_thread_resume = getattr(
            self.module.callbacks, "set_thread_resume", None)
        if set_thread_start is not None and self.space is not self.module:
            set_thread_start(None, self.space)
            set_thread_yield(None, self.space)
            set_thread_resume(None, self.space)
        self.module.callbacks.thread_start = self.previous_start
        self.module.callbacks.thread_yield = self.previous_yield
        self.module.callbacks.thread_resume = self.previous_resume

    def record_start(self):
        ident = _thread.get_ident()
        if self.skip_launcher_start:
            self.skip_launcher_start = False
            return
        _thread_local.schedule_id = ident
        with self.lock:
            if ident in self.started_threads:
                return
            self.started_threads.add(ident)
            self.scheduled_tid = ident
            self.schedule.append(["start", ident])

    def record_yield(self):
        ident = current_schedule_id()
        if ident is None or thread_in_schedule_control():
            return
        with self.lock:
            if ident not in self.started_threads:
                return
            if self.scheduled_tid != ident:
                self.scheduled_tid = ident
                self.schedule.append(["resume", ident])
            raw_delta = tuple(self.space.thread_delta())
            raw_cursor = apply_delta(
                self.record_cursors.setdefault(ident, []), raw_delta)
            call_at_stack = self.call_at_cursors.setdefault(ident, [])
            delta = delta_from_cursor(call_at_stack, raw_cursor)
            self.schedule.append(["yield", ident, list(delta)])
            apply_delta(call_at_stack, delta)
            self.scheduled_tid = None

    def record_resume(self):
        return None

    def after_target_start(self, timeout):
        return None

    def finish(self, result):
        return self.schedule, result


class FreshScheduleReplay:
    def __init__(self, module, schedule, timeout, coordinate_space=None):
        self.module = module
        self.space = (
            coordinate_space if coordinate_space is not None else module)
        self.scheduler = ReplayScheduler(
            module,
            schedule,
            {},
            {},
            literal_coordinates=True,
            allow_current_before_future_start=True,
            coordinate_space=coordinate_space,
            timeout=timeout,
        )
        self.previous_start = None
        self.previous_yield = None
        self.previous_resume = None
        self.skip_launcher_start = True

    def start(self):
        self.previous_start = self.module.callbacks.thread_start
        self.previous_yield = self.module.callbacks.thread_yield
        self.previous_resume = self.module.callbacks.thread_resume
        set_thread_start = getattr(
            self.module.callbacks, "set_thread_start", None)
        set_thread_yield = getattr(
            self.module.callbacks, "set_thread_yield", None)
        set_thread_resume = getattr(
            self.module.callbacks, "set_thread_resume", None)
        if set_thread_start is not None and self.space is not self.module:
            set_thread_start(self.replay_start, self.space)
            set_thread_yield(None, self.space)
            set_thread_resume(self.replay_resume, self.space)
        else:
            self.module.callbacks.thread_start = self.replay_start
            self.module.callbacks.thread_yield = None
            self.module.callbacks.thread_resume = self.replay_resume
        self.scheduler.start()

    def close(self):
        set_thread_start = getattr(
            self.module.callbacks, "set_thread_start", None)
        set_thread_yield = getattr(
            self.module.callbacks, "set_thread_yield", None)
        set_thread_resume = getattr(
            self.module.callbacks, "set_thread_resume", None)
        if set_thread_start is not None and self.space is not self.module:
            set_thread_start(None, self.space)
            set_thread_yield(None, self.space)
            set_thread_resume(None, self.space)
        self.module.callbacks.thread_start = self.previous_start
        self.module.callbacks.thread_yield = self.previous_yield
        self.module.callbacks.thread_resume = self.previous_resume
        self.space.call_at(None)
        self.scheduler.close()

    def replay_start(self):
        ident = _thread.get_ident()
        if self.skip_launcher_start:
            self.skip_launcher_start = False
            return
        schedule_id = self.scheduler.bind_next_recorded_start(ident)
        _thread_local.schedule_id = schedule_id
        enter_schedule_control()
        try:
            self.scheduler.note_thread_start(schedule_id)
            self.scheduler.start_current_thread()
        finally:
            exit_schedule_control()

    def replay_resume(self):
        if thread_in_schedule_control():
            return
        ident = current_schedule_id()
        if ident is None:
            return
        enter_schedule_control()
        try:
            self.scheduler.yield_until_turn()
        finally:
            exit_schedule_control()

    def run_until_done(self, timeout):
        enter_schedule_control()
        try:
            self.scheduler.run_until_done(timeout=timeout)
        finally:
            exit_schedule_control()

    def after_target_start(self, timeout):
        self.run_until_done(timeout)

    def finish(self, result):
        return result


def record_thread_schedule(
    module,
    target,
    *args,
    timeout=10.0,
    switch_interval=1e-6,
    coordinate_space=None,
    **kwargs,
):
    """Run target under fresh coordinates and record target-created threads."""
    runner = FreshScheduleRecorder(module, coordinate_space)
    run = (
        coordinate_space.run
        if coordinate_space is not None
        else module.with_new_coordinates
    )
    return run(
        _run_fresh_thread_schedule,
        runner,
        target,
        args,
        kwargs,
        timeout,
        switch_interval,
    )


def replay_thread_schedule(
    module,
    schedule,
    target,
    *args,
    timeout=10.0,
    switch_interval=1e-6,
    coordinate_space=None,
    **kwargs,
):
    """Replay target-created thread schedule under fresh coordinates."""
    runner = FreshScheduleReplay(module, schedule, timeout, coordinate_space)
    run = (
        coordinate_space.run
        if coordinate_space is not None
        else module.with_new_coordinates
    )
    return run(
        _run_fresh_thread_schedule,
        runner,
        target,
        args,
        kwargs,
        timeout,
        switch_interval,
    )


def test_thread_schedule(retrace, function, args, kwargs):
    """Record and replay function, raising if replay returns a different value."""
    args = tuple(args)
    kwargs = dict(kwargs)
    coordinate_space_type = getattr(retrace, "CoordinateSpace", None)
    record_space = (
        coordinate_space_type()
        if coordinate_space_type is not None
        else None
    )
    replay_space = (
        coordinate_space_type()
        if coordinate_space_type is not None
        else None
    )
    schedule, record_result = record_thread_schedule(
        retrace, function, *args,
        coordinate_space=record_space,
        **kwargs)
    if not schedule:
        raise AssertionError("record did not capture any thread switches")
    replay_result = replay_thread_schedule(
        retrace, schedule, function, *args,
        coordinate_space=replay_space,
        **kwargs)
    if replay_result != record_result:
        raise AssertionError(
            f"record result {record_result!r} != replay result "
            f"{replay_result!r}"
        )
    return None


def _run_fresh_thread_schedule(
    runner,
    target,
    args,
    kwargs,
    timeout,
    switch_interval,
):
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(switch_interval)
    try:
        runner.start()
        done = _run_raw_thread(target, args, kwargs)
        runner.after_target_start(timeout)
        result = _get_raw_thread_result(done, timeout)
        return runner.finish(result)
    finally:
        runner.close()
        sys.setswitchinterval(old_interval)


def run_workload(module, mode, schedule, config, workload):
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(config.switch_interval)
    controller = ThreadScheduleController(module, mode, schedule)
    try:
        events = workload(module, controller, config)
        return controller.result(events, config)
    finally:
        controller.close()
        sys.setswitchinterval(old_interval)


def run_child(config, script_path, mode, payload=None, seed=None):
    env = os.environ.copy()
    env["RETRACE_ROOT_SEED"] = seed or config.name
    env["PYTHONFAULTHANDLER"] = "1"
    child_payload = {"mode": mode}
    if payload is not None:
        child_payload["schedule"] = payload["schedule"]
    try:
        proc = subprocess.run(
            [sys.executable, script_path, mode],
            input=json.dumps(child_payload),
            capture_output=True,
            text=True,
            timeout=config.timeout + 2.0,
            env=env,
        )
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", "replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", "replace")
        raise AssertionError(
            f"{mode} timed out\nstdout:\n{stdout}\nstderr:\n{stderr}"
        ) from exc
    if proc.returncode != 0:
        raise AssertionError(
            f"{mode} failed with exit {proc.returncode}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    return json.loads(proc.stdout)


def main(config, script_path, workload, output_prefix,
         include_schedule_count=False, compare_events=True):
    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is None:
        print("retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    if len(sys.argv) == 2 and sys.argv[1] in {"record", "replay"}:
        import faulthandler

        faulthandler.dump_traceback_later(config.timeout, file=sys.stderr)
        payload = json.loads(sys.stdin.read())
        child_mode = payload["mode"]
        result = run_workload(
            module,
            child_mode,
            payload.get("schedule"),
            config,
            workload,
        )
        print(json.dumps(result, separators=(",", ":")))
        return 0

    record = None
    record_seed = config.name
    for attempt in range(10):
        record_seed = config.name if attempt == 0 else f"{config.name}-{attempt}"
        candidate = run_child(config, script_path, "record", seed=record_seed)
        if candidate["schedule"]:
            record = candidate
            break
    if record is None:
        raise AssertionError("record did not capture any thread switches")

    replay = run_child(config, script_path, "replay", record, seed=record_seed)
    if compare_events:
        assert replay["events"] == record["events"], (
            f"record digest={record['digest']} replay digest={replay['digest']}"
        )
    print("retrace_module=available")
    print(f"{output_prefix}_events={len(record['events'])}")
    print(f"{output_prefix}_digest={record['digest']}")
    if include_schedule_count:
        print(f"{output_prefix}_schedule={len(record['schedule'])}")
    print(f"{output_prefix}_replay=available")
    return 0
