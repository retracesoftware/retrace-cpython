import os
import queue
import sys
import threading
import time
import _thread

sys.path.insert(0, os.path.dirname(__file__))

from thread_schedule import (
    ThreadScheduleConfig,
    enter_schedule_control,
    exit_schedule_control,
    main,
)


CASE_ITERATIONS = 8
CASE_COUNT = 5
CONFIG = ThreadScheduleConfig(
    name="thread-schedule-primitives",
    thread_count=4,
    iterations=CASE_ITERATIONS * CASE_COUNT,
    timeout=20,
    switch_interval=0.001,
)
SCHEDULE_PAUSE = 0.001


def schedule_pause():
    time.sleep(SCHEDULE_PAUSE)


def run_workload(module, controller, config):
    events = []
    done = queue.Queue()

    unlocked = {"value": 0}
    locked = {"value": 0}
    counter_lock = threading.Lock()

    gate = threading.Semaphore(2)
    active = set()
    active_lock = threading.Lock()
    max_active = {"value": 0}

    buffer_capacity = 2
    empty_slots = threading.Semaphore(buffer_capacity)
    filled_slots = threading.Semaphore(0)
    buffer_lock = threading.Lock()
    buffer = []
    consumed = []

    condition = threading.Condition()
    turn = {"value": 0}
    condition_trace = []

    def report(result):
        enter_schedule_control()
        try:
            done.put(result)
        finally:
            exit_schedule_control()

    def note(tid):
        events.append(tid)

    def run_unlocked_counter(tid):
        for _ in range(CASE_ITERATIONS):
            before = unlocked["value"]
            schedule_pause()
            unlocked["value"] = before + 1
            note(tid)
            schedule_pause()

    def run_locked_counter(tid):
        enter_schedule_control()
        try:
            for _ in range(CASE_ITERATIONS):
                schedule_pause()
                with counter_lock:
                    before = locked["value"]
                    locked["value"] = before + 1
                    note(tid)
                schedule_pause()
        finally:
            exit_schedule_control()

    def run_semaphore_gate(tid):
        enter_schedule_control()
        try:
            for _ in range(CASE_ITERATIONS):
                schedule_pause()
                gate.acquire()
                try:
                    with active_lock:
                        active.add(tid)
                        max_active["value"] = max(
                            max_active["value"],
                            len(active),
                        )
                        assert len(active) <= 2, tuple(sorted(active))
                    with active_lock:
                        assert tid in active, tuple(sorted(active))
                        active.remove(tid)
                        note(tid)
                finally:
                    gate.release()
                schedule_pause()
        finally:
            exit_schedule_control()

    def run_bounded_buffer(tid):
        enter_schedule_control()
        try:
            for step in range(CASE_ITERATIONS):
                item = (tid, step)
                schedule_pause()
                empty_slots.acquire()
                with buffer_lock:
                    buffer.append(item)
                    assert len(buffer) <= buffer_capacity, tuple(buffer)
                filled_slots.release()

                filled_slots.acquire()
                with buffer_lock:
                    consumed.append((tid, buffer.pop(0)))
                    note(tid)
                empty_slots.release()
                schedule_pause()
        finally:
            exit_schedule_control()

    def run_condition_handoff(tid):
        enter_schedule_control()
        try:
            for step in range(CASE_ITERATIONS):
                with condition:
                    while turn["value"] != tid:
                        condition.wait()
                    condition_trace.append((tid, step))
                    note(tid)
                    turn["value"] = (tid + 1) % config.thread_count
                    condition.notify_all()
                schedule_pause()
        finally:
            exit_schedule_control()

    def worker(tid):
        try:
            controller.register_thread(tid)
            controller.yield_until_turn()
            run_unlocked_counter(tid)
            run_locked_counter(tid)
            run_semaphore_gate(tid)
            run_bounded_buffer(tid)
            run_condition_handoff(tid)
        except BaseException as exc:
            report(exc)
        else:
            report(None)

    controller.start()
    for tid in range(config.thread_count):
        _thread.start_new_thread(worker, (tid,))

    controller.run_replay()

    for _ in range(config.thread_count):
        try:
            result = done.get(timeout=config.timeout)
        except queue.Empty as exc:
            raise AssertionError("worker did not finish") from exc
        if result is not None:
            raise result

    expected = config.thread_count * CASE_ITERATIONS
    assert unlocked["value"] <= expected, unlocked["value"]
    assert locked["value"] == expected, locked["value"]
    assert max_active["value"] <= 2, max_active["value"]
    assert not active, active
    assert not buffer, buffer
    assert len(consumed) == expected, consumed
    assert len(condition_trace) == expected, condition_trace
    for index, (tid, step) in enumerate(condition_trace):
        assert tid == index % config.thread_count, condition_trace
        assert step == index // config.thread_count, condition_trace

    return events


if __name__ == "__main__":
    raise SystemExit(
        main(
            CONFIG,
            __file__,
            run_workload,
            "thread_schedule_primitives",
            include_schedule_count=True,
            compare_events=False,
        )
    )
