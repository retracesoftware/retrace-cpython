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


_thread_local = threading.local()


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


class _ThreadSwitchCapture:
    def __init__(self, module, coordinate_space=None):
        self.module = module
        self.space = coordinate_space
        self.schedule = []
        self.previous_root_callback = None
        self.lock = threading.RLock()

    def start(self):
        if self.space is None or self.space is self.module:
            self.previous_root_callback = self.module.callbacks.thread_switch
            self.module.callbacks.thread_switch = self.on_thread_switch
            return
        self.module.callbacks.set_thread_switch(self.on_thread_switch, self.space)

    def close(self):
        if self.space is None or self.space is self.module:
            self.module.callbacks.thread_switch = self.previous_root_callback
            return
        self.module.callbacks.set_thread_switch(None, self.space)

    def on_thread_switch(self, previous_delta, next_thread_id):
        if thread_in_schedule_control():
            return None
        with self.lock:
            self.schedule.append([
                "switch",
                next_thread_id,
                list(previous_delta),
            ])
        return None


class ThreadScheduleController:
    def __init__(self, module, mode, schedule=None):
        self.module = module
        self.mode = mode
        self.input_schedule = schedule or []
        self.capture = _ThreadSwitchCapture(module)
        self.thread_ids = {}

    def start(self):
        self.capture.start()

    def close(self):
        self.capture.close()

    def register_thread(self, tid):
        ident = _thread.get_ident()
        _thread_local.tid = tid
        _thread_local.schedule_id = ident
        self.thread_ids[str(ident)] = ident
        return ident

    def yield_until_turn(self):
        return None

    def run_replay(self):
        return None

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
            result["schedule"] = self.capture.schedule
        elif self.mode == "replay":
            result["schedule"] = self.input_schedule
        return result


class _FreshThreadSwitchRunner:
    def __init__(self, module, coordinate_space=None):
        self.capture = _ThreadSwitchCapture(module, coordinate_space)

    def start(self):
        self.capture.start()

    def close(self):
        self.capture.close()

    def after_target_start(self, timeout):
        return None

    def finish(self, result):
        return self.capture.schedule, result


class _FreshThreadSwitchReplay:
    def __init__(self, module, schedule, coordinate_space=None):
        self.schedule = schedule
        self.capture = _ThreadSwitchCapture(module, coordinate_space)

    def start(self):
        self.capture.start()

    def close(self):
        self.capture.close()

    def after_target_start(self, timeout):
        return None

    def finish(self, result):
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


def record_thread_schedule(
    module,
    target,
    *args,
    timeout=10.0,
    switch_interval=1e-6,
    coordinate_space=None,
    **kwargs,
):
    """Run target under fresh coordinates and record thread_switch callbacks."""
    runner = _FreshThreadSwitchRunner(module, coordinate_space)
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
    """Run target while observing thread_switch callbacks under fresh coordinates."""
    runner = _FreshThreadSwitchReplay(module, schedule, coordinate_space)
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
    """Record and rerun function, raising if the second run returns differently."""
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
