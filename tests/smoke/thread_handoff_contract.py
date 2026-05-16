import importlib.util
import _thread
import threading
import time


PROBE_MODULE = "retrace"


def join_or_fail(thread, timeout=5.0):
    thread.join(timeout)
    assert not thread.is_alive(), f"thread did not finish: {thread.name}"


def check_handoff_to_running_thread_parks_on_next_yield(module):
    handoff = module.ThreadHandoff(timeout=1.0)
    main_ident = _thread.get_ident()
    ready = threading.Event()
    after_yield = threading.Event()
    worker_ident = []
    events = []
    errors = []
    lock = threading.Lock()

    def note(name):
        with lock:
            events.append(name)

    def worker():
        try:
            worker_ident.append(_thread.get_ident())
            ready.set()
            time.sleep(0.05)
            note("worker-before-yield")
            handoff.to(main_ident)
            note("worker-after-yield")
            after_yield.set()
        except BaseException as exc:
            errors.append(exc)

    thread = threading.Thread(
        target=worker,
        name="thread-handoff-running-target",
    )
    thread.start()
    try:
        assert ready.wait(1.0)
        handoff.to(worker_ident[0])
        note("main-after-worker-yield")

        if after_yield.wait(0.1):
            raise AssertionError(
                "worker did not park after yielding back to main: "
                f"events={events!r}"
            )
    finally:
        handoff.close()

    join_or_fail(thread)
    if errors:
        raise errors[0]

    assert events[:2] == [
        "worker-before-yield",
        "main-after-worker-yield",
    ], events


def main() -> int:
    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is None:
        print("retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    if not hasattr(module, "ThreadHandoff"):
        print("thread_handoff_contract=unavailable")
        return 0

    check_handoff_to_running_thread_parks_on_next_yield(module)

    print("thread_handoff_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
