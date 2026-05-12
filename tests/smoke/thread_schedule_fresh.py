import importlib.util
import os
import queue
import sys
import time
import _thread

sys.path.insert(0, os.path.dirname(__file__))

from thread_schedule import (
    MASK,
    ThreadScheduleConfig,
    digest_events,
    record_thread_schedule,
    replay_thread_schedule,
    test_thread_schedule,
)


PROBE_MODULE = "retrace"

CONFIG = ThreadScheduleConfig(
    name="thread-schedule-fresh",
    thread_count=1,
    iterations=1,
    timeout=10,
)


def run_loop(events, tid, iterations):
    value = tid
    for _ in range(iterations):
        events.append(tid)
        value = (value * 1103515245 + 12345) & MASK
        time.sleep(0)
    return value


def target(thread_count, iterations, timeout):
    events = []
    done = queue.Queue()

    def worker(tid):
        try:
            run_loop(events, tid, iterations)
        except BaseException as exc:
            done.put(exc)
        else:
            done.put(None)

    for tid in range(thread_count):
        _thread.start_new_thread(worker, (tid,))

    for _ in range(thread_count):
        try:
            result = done.get(timeout=timeout)
        except queue.Empty as exc:
            raise AssertionError("worker did not finish") from exc
        if result is not None:
            raise result
    return events


def assert_event_counts(name, events):
    expected_length = CONFIG.thread_count * CONFIG.iterations
    assert len(events) == expected_length, (
        f"{name} length={len(events)} expected={expected_length}"
    )
    seen = [0] * CONFIG.thread_count
    for tid in events:
        seen[tid] += 1
    expected = [CONFIG.iterations] * CONFIG.thread_count
    assert seen == expected, f"{name} counts={seen} expected={expected}"


def main() -> int:
    if importlib.util.find_spec(PROBE_MODULE) is None:
        print("retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    if not hasattr(module, "with_new_coordinates"):
        print("with_new_coordinates=unavailable")
        return 0

    record = None
    for _ in range(10):
        schedule, result = record_thread_schedule(
            module,
            target,
            CONFIG.thread_count,
            CONFIG.iterations,
            CONFIG.timeout,
            timeout=CONFIG.timeout,
            switch_interval=CONFIG.switch_interval,
        )
        if schedule:
            record = (schedule, result)
            break
    if record is None:
        raise AssertionError("record did not capture any thread switches")

    schedule, record_events = record
    replay_events = replay_thread_schedule(
        module,
        schedule,
        target,
        CONFIG.thread_count,
        CONFIG.iterations,
        CONFIG.timeout,
        timeout=CONFIG.timeout,
        switch_interval=CONFIG.switch_interval,
    )

    assert_event_counts("record", record_events)
    assert_event_counts("replay", replay_events)
    test_thread_schedule(
        module,
        target,
        (CONFIG.thread_count, CONFIG.iterations, CONFIG.timeout),
        {},
    )

    print("retrace_module=available")
    print(f"thread_schedule_fresh_events={len(record_events)}")
    print(f"thread_schedule_fresh_digest={digest_events(record_events):016x}")
    replay_digest = digest_events(replay_events)
    print(f"thread_schedule_fresh_replay_digest={replay_digest:016x}")
    print(f"thread_schedule_fresh_schedule={len(schedule)}")
    print("thread_schedule_fresh_replay=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
