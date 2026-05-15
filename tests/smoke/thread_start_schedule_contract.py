import importlib.util
import _thread
import threading
import time


PROBE_MODULE = "retrace"


def join_or_fail(thread, timeout=5.0):
    thread.join(timeout)
    assert not thread.is_alive(), f"thread did not finish: {thread.name}"


def check_thread_start_enters_target_without_initial_resume(module):
    old_start = module.callbacks.thread_start
    old_yield = module.callbacks.thread_yield
    old_resume = module.callbacks.thread_resume

    events = []
    errors = []
    lock = threading.Lock()
    worker_ident = []

    def note(kind):
        with lock:
            events.append((kind, _thread.get_ident()))

    def worker():
        try:
            worker_ident.append(_thread.get_ident())
            note("body")
            time.sleep(0)
        except BaseException as exc:
            errors.append(("worker", repr(exc)))

    try:
        module.callbacks.thread_start = lambda: note("start")
        module.callbacks.thread_yield = lambda: note("yield")
        module.callbacks.thread_resume = lambda: note("resume")

        thread = threading.Thread(
            target=worker,
            name="thread-start-schedule-contract",
        )
        thread.start()
        join_or_fail(thread)
    finally:
        module.callbacks.thread_start = old_start
        module.callbacks.thread_yield = old_yield
        module.callbacks.thread_resume = old_resume

    assert not errors, errors
    assert worker_ident, events

    child = worker_ident[0]
    child_events = [kind for kind, ident in events if ident == child]
    assert "body" in child_events, (child, events, child_events)

    assert child_events[0] == "start", (child, events, child_events)

    body_index = child_events.index("body")
    assert child_events[:body_index] == ["start"], (
        child,
        events,
        child_events,
    )


def main() -> int:
    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is None:
        print("retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    required = (
        "callbacks",
        "ThreadHandoff",
        "thread_delta",
    )
    if not all(hasattr(module, name) for name in required):
        print("thread_start_schedule_contract=unavailable")
        return 0

    check_thread_start_enters_target_without_initial_resume(module)

    print("thread_start_schedule_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
