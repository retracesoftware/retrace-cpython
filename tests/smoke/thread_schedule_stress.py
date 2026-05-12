import os
import sys
import threading

sys.path.insert(0, os.path.dirname(__file__))

from thread_schedule import MASK, ThreadScheduleConfig, main


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

    def worker(tid):
        controller.register_thread(tid)
        controller.yield_until_turn()
        run_loop(events, tid, config.iterations)

    controller.start()
    workers = [
        threading.Thread(target=worker, args=(tid,))
        for tid in range(config.thread_count)
    ]
    for worker_thread in workers:
        worker_thread.start()

    for worker_thread in workers:
        worker_thread.join()
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
