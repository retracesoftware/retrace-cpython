import os
import queue
import sys
import _thread

sys.path.insert(0, os.path.dirname(__file__))

from thread_schedule import (
    MASK,
    ThreadScheduleConfig,
    enter_schedule_control,
    exit_schedule_control,
    main,
)


CONFIG = ThreadScheduleConfig(
    name="thread-schedule-stress",
    thread_count=2,
    iterations=128,
    timeout=20,
)


def run_loop(events, tid, iterations):
    value = tid
    for _ in range(iterations):
        events.append(tid)
        value = (value * 1103515245 + 12345) & MASK
    return value


def run_workload(module, controller, config):
    events = []
    done = queue.Queue()

    def report(result):
        enter_schedule_control()
        try:
            done.put(result)
        finally:
            exit_schedule_control()

    def worker(tid):
        try:
            controller.register_thread(tid)
            controller.yield_until_turn()
            run_loop(events, tid, config.iterations)
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
    return events


if __name__ == "__main__":
    raise SystemExit(
        main(
            CONFIG,
            __file__,
            run_workload,
            "thread_schedule",
            compare_events=False,
        )
    )
