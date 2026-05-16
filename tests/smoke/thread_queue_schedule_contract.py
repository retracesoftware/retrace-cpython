import importlib.util
import os
import queue
import sys
import threading

sys.path.insert(0, os.path.dirname(__file__))

from thread_schedule import record_thread_schedule, replay_thread_schedule


PROBE_MODULE = "retrace"
ATTEMPTS = 8
TIMEOUT = 3.0
WORKLOAD_TIMEOUT = 5.0


def run_queue_handoff_workload():
    values = queue.Queue(maxsize=1)
    consumer_ready = threading.Event()
    events = []
    errors = []

    def consumer():
        try:
            events.append("consumer-start")
            consumer_ready.set()
            item = values.get(timeout=WORKLOAD_TIMEOUT)
            events.append(f"consumer-got-{item}")
        except BaseException as exc:
            errors.append(exc)

    def producer():
        try:
            events.append("producer-start")
            values.put("value", timeout=WORKLOAD_TIMEOUT)
            events.append("producer-put")
        except BaseException as exc:
            errors.append(exc)

    consumer_thread = threading.Thread(
        target=consumer,
        name="thread-queue-schedule-consumer",
    )
    consumer_thread.start()
    if not consumer_ready.wait(WORKLOAD_TIMEOUT):
        raise AssertionError(f"consumer did not start: events={events!r}")

    producer_thread = threading.Thread(
        target=producer,
        name="thread-queue-schedule-producer",
    )
    producer_thread.start()

    consumer_thread.join(WORKLOAD_TIMEOUT)
    producer_thread.join(WORKLOAD_TIMEOUT)

    if consumer_thread.is_alive() or producer_thread.is_alive():
        raise AssertionError(
            "worker threads did not finish: "
            f"consumer_alive={consumer_thread.is_alive()} "
            f"producer_alive={producer_thread.is_alive()} "
            f"events={events!r}"
        )
    if errors:
        raise errors[0]
    return events


def check_thread_switch_schedule(schedule):
    for index, item in enumerate(schedule, 1):
        if item[0] != "switch":
            raise AssertionError(
                f"unexpected schedule item at {index}: {item!r}"
            )
        _kind, next_thread_id, previous_delta = item
        assert type(next_thread_id) is int and next_thread_id > 0, item
        assert previous_delta and previous_delta[0] >= 0, item


def check_queue_wait_schedule(module):
    record_space = module.CoordinateSpace()
    replay_space = module.CoordinateSpace()

    schedule, record_result = record_thread_schedule(
        module,
        run_queue_handoff_workload,
        timeout=TIMEOUT,
        coordinate_space=record_space,
    )
    if not schedule:
        raise AssertionError("record did not capture any thread switches")
    check_thread_switch_schedule(schedule)

    replay_result = replay_thread_schedule(
        module,
        schedule,
        run_queue_handoff_workload,
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
        "CoordinateSpace",
        "ThreadHandoff",
        "callbacks",
        "thread_delta",
    )
    if not all(hasattr(module, name) for name in required):
        print("thread_queue_schedule_contract=unavailable")
        return 0

    for attempt in range(ATTEMPTS):
        try:
            check_queue_wait_schedule(module)
        except BaseException as exc:
            raise AssertionError(
                "queue wait schedule replay failed "
                f"on attempt {attempt + 1}"
            ) from exc

    print("thread_queue_schedule_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
