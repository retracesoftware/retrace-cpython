import importlib.util
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))

from thread_schedule import record_thread_schedule, replay_thread_schedule


PROBE_MODULE = "retrace"
ATTEMPTS = 3
TIMEOUT = 3.0
WORKLOAD_TIMEOUT = 5.0


def run_lock_wait_handoff_workload():
    events = []
    gate = threading.Lock()
    gate.acquire()
    errors = []

    def waiter():
        try:
            events.append("waiter-start")
            if not gate.acquire(timeout=WORKLOAD_TIMEOUT):
                raise TimeoutError("gate acquire timed out")
            events.append("waiter-acquired")
        except BaseException as exc:
            errors.append(exc)

    waiter_thread = threading.Thread(
        target=waiter,
        name="thread-lock-wait-schedule-waiter",
    )
    waiter_thread.start()
    time.sleep(0.01)

    events.append("release")
    gate.release()

    waiter_thread.join(WORKLOAD_TIMEOUT)

    if waiter_thread.is_alive():
        raise AssertionError(
            "worker threads did not finish: "
            f"waiter_alive={waiter_thread.is_alive()} "
            f"events={events!r}"
        )
    if errors:
        raise errors[0]
    return events


def check_lock_wait_schedule(module):
    record_space = (
        module.CoordinateSpace()
        if hasattr(module, "CoordinateSpace")
        else None
    )
    replay_space = (
        module.CoordinateSpace()
        if hasattr(module, "CoordinateSpace")
        else None
    )

    schedule, record_result = record_thread_schedule(
        module,
        run_lock_wait_handoff_workload,
        timeout=TIMEOUT,
        coordinate_space=record_space,
    )
    if not schedule:
        raise AssertionError("record did not capture any thread switches")

    replay_result = replay_thread_schedule(
        module,
        schedule,
        run_lock_wait_handoff_workload,
        timeout=TIMEOUT,
        coordinate_space=replay_space,
    )
    if replay_result != record_result:
        raise AssertionError(
            f"record result {record_result!r} != replay result "
            f"{replay_result!r}"
        )


def main() -> int:
    if importlib.util.find_spec(PROBE_MODULE) is None:
        print("retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    required = (
        "callbacks",
        "ThreadHandoff",
        "call_at",
        "thread_delta",
        "with_new_coordinates",
    )
    if not all(hasattr(module, name) for name in required):
        print("thread_wait_schedule_contract=unavailable")
        return 0

    for attempt in range(ATTEMPTS):
        try:
            check_lock_wait_schedule(module)
        except BaseException as exc:
            raise AssertionError(
                "lock wait thread schedule replay failed "
                f"on attempt {attempt + 1}"
            ) from exc

    print("thread_wait_schedule_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
