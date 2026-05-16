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
WORKLOAD_TIMEOUT = 30.0


def run_asyncio_threadsafe_workload():
    import asyncio

    events = []
    errors = queue.Queue()
    loop_ready = queue.Queue(maxsize=1)

    async def coro():
        events.append("coro-start")
        await asyncio.sleep(0)
        events.append("coro-after-sleep")
        return 123

    def loop_runner():
        try:
            events.append("loop-start")
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            loop_ready.put(loop)
            events.append("loop-before-run")
            try:
                loop.run_forever()
            finally:
                events.append("loop-closing")
                loop.close()
                events.append("loop-closed")
        except BaseException as exc:
            errors.put(exc)

    def submitter():
        try:
            events.append("submitter-start")
            loop = loop_ready.get(timeout=WORKLOAD_TIMEOUT)
            events.append("submitter-got-loop")
            future = asyncio.run_coroutine_threadsafe(coro(), loop)
            events.append("submitter-posted-coro")
            result = future.result(timeout=WORKLOAD_TIMEOUT)
            events.append(f"submitter-result-{result}")
            loop.call_soon_threadsafe(loop.stop)
            events.append("submitter-stop-posted")
        except BaseException as exc:
            errors.put(exc)

    loop_thread = threading.Thread(
        target=loop_runner,
        name="asyncio-thread-schedule-loop",
        daemon=True,
    )
    submit_thread = threading.Thread(
        target=submitter,
        name="asyncio-thread-schedule-submitter",
        daemon=True,
    )

    loop_thread.start()
    submit_thread.start()
    loop_thread.join(timeout=WORKLOAD_TIMEOUT)
    submit_thread.join(timeout=WORKLOAD_TIMEOUT)

    if loop_thread.is_alive() or submit_thread.is_alive():
        raise AssertionError(
            "asyncio worker threads did not finish: "
            f"loop_alive={loop_thread.is_alive()} "
            f"submitter_alive={submit_thread.is_alive()} "
            f"events={events!r}"
        )
    if not errors.empty():
        raise errors.get()
    return events


def check_asyncio_threadsafe_schedule(module):
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
        run_asyncio_threadsafe_workload,
        timeout=TIMEOUT,
        coordinate_space=record_space,
    )
    if not schedule:
        raise AssertionError("record did not capture any thread switches")

    replay_result = replay_thread_schedule(
        module,
        schedule,
        run_asyncio_threadsafe_workload,
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
        print("asyncio_thread_schedule_contract=unavailable")
        return 0

    for attempt in range(ATTEMPTS):
        try:
            check_asyncio_threadsafe_schedule(module)
        except BaseException as exc:
            raise AssertionError(
                "asyncio run_coroutine_threadsafe schedule replay failed "
                f"on attempt {attempt + 1}"
            ) from exc

    print("asyncio_thread_schedule_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
