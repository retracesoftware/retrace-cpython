import importlib.util
import _thread
import threading


PROBE_MODULE = "retrace"


def join_or_fail(thread, timeout=5.0):
    thread.join(timeout)
    assert not thread.is_alive(), f"thread did not finish: {thread.name}"


def check_thread_switch_enters_target_before_body(module):
    old_switch = module.callbacks.thread_switch

    events = []
    errors = []
    lock = threading.Lock()
    worker_ident = []

    def note(kind, ident=None):
        with lock:
            events.append((kind, _thread.get_ident() if ident is None else ident))

    def on_switch(previous_delta, next_thread_id):
        try:
            note("switch", next_thread_id)
            assert type(previous_delta) is tuple
            assert previous_delta
        except BaseException as exc:
            errors.append(("switch", repr(exc)))

    def worker():
        try:
            worker_ident.append(_thread.get_ident())
            note("body")
        except BaseException as exc:
            errors.append(("worker", repr(exc)))

    try:
        module.callbacks.thread_switch = on_switch
        thread = threading.Thread(
            target=worker,
            name="thread-switch-start-contract",
        )
        thread.start()
        join_or_fail(thread)
    finally:
        module.callbacks.thread_switch = old_switch

    assert not errors, errors
    assert worker_ident, events

    child = worker_ident[0]
    child_events = [kind for kind, ident in events if ident == child]
    assert child_events[:2] == ["switch", "body"], (
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

    check_thread_switch_enters_target_before_body(module)

    print("thread_start_schedule_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
