import importlib.util
import json
import os
import subprocess
import sys
import threading
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
    def __init__(self, module, schedule, thread_ids, thread_roots):
        self.module = module
        self.schedule = schedule
        self.thread_ids = thread_ids
        self.thread_roots = thread_roots
        self.lock = threading.RLock()
        self.handoff = module.ThreadHandoff()
        self.index = 0
        self.record_cursors = {}
        self.current_tid = None
        self.armed = None
        self.started_threads = set()
        self.done = False

    def replay_ident(self, tid):
        return self.thread_ids.get(str(tid), tid)

    def translate_cursor(self, tid, cursor):
        if not cursor:
            return cursor
        root = self.thread_roots.get(str(tid))
        if root is None:
            return None
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
            if str(tid) not in self.thread_roots:
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
                self.module.call_at(
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
        self.module.call_at(None)

    def target_tid_locked(self):
        if self.current_tid is not None:
            return self.current_tid
        if self.index >= len(self.schedule):
            return None
        item = self.schedule[self.index]
        if item[0] in {"start", "resume", "yield"}:
            return item[1]
        raise AssertionError(f"unknown schedule item: {item!r}")

    def waiting_for_future_start_locked(self):
        return (
            self.current_tid is None and
            self.index < len(self.schedule) and
            self.schedule[self.index][0] == "start" and
            self.schedule[self.index][1] not in self.started_threads
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
                if self.waiting_for_future_start_locked():
                    debug(
                        f"continue tid={tid!r} waiting for future "
                        f"start target={target_tid!r}"
                    )
                    return
                target_ident = self.replay_ident(target_tid)
                debug(
                    f"wait tid={tid!r} target={target_tid!r} "
                    f"cursor={tuple(self.module.coordinates())!r}"
                )
            self.handoff(target_ident)

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
                if self.waiting_for_future_start_locked():
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
            self.handoff(target_ident)

    def note_thread_start(self, ident):
        with self.lock:
            self.thread_ids.setdefault(str(ident), ident)
            self.started_threads.add(ident)
            self.arm_locked()

    def note_thread_registered(self, ident):
        with self.lock:
            self.thread_ids.setdefault(str(ident), ident)
            self.started_threads.add(ident)
            self.arm_locked()

    def on_call_at(self):
        debug("call_at callback")
        self.pause_current_thread()
        return None

    def on_overshoot(self):
        debug("overshoot callback")
        self.pause_current_thread()
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
        root = tuple(self.module.coordinates())[0]
        self.thread_ids[str(ident)] = ident
        self.thread_roots[str(tid)] = root
        self.thread_roots[str(ident)] = root
        self.id_to_tid[ident] = tid
        if self.scheduler is not None:
            self.scheduler.note_thread_registered(ident)
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
        run_transparent = getattr(self.module, "run_transparent", None)
        if self.mode == "replay" and run_transparent is not None:
            run_transparent(self.replay_yield_until_turn)
        else:
            self.replay_yield_until_turn()

    def replay_resume(self):
        if thread_in_schedule_control():
            return
        self.replay_yield_until_turn()

    def replay_start(self):
        if self.scheduler is not None:
            self.scheduler.note_thread_start(_thread.get_ident())

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
