import concurrent.futures
import importlib.util
import _thread
import threading


PROBE_MODULE = "retrace"


def check_threadpool_worker_switches_before_work(module):
    old_switch = module.callbacks.thread_switch

    events = []
    lock = threading.Lock()
    worker_ident = []

    def note(kind, ident=None):
        with lock:
            events.append((kind, _thread.get_ident() if ident is None else ident))

    def on_switch(previous_delta, next_thread_id):
        note("switch", next_thread_id)

    def warmup():
        worker_ident.append(_thread.get_ident())
        return None

    def work():
        assert worker_ident and worker_ident[0] == _thread.get_ident()
        note("body")
        return 1

    try:
        module.callbacks.thread_switch = on_switch
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            assert executor.submit(warmup).result(timeout=5.0) is None
            assert worker_ident, events
            assert executor.submit(work).result(timeout=5.0) == 1
    finally:
        module.callbacks.thread_switch = old_switch

    child = worker_ident[0]
    child_events = [kind for kind, ident in events if ident == child]
    assert "switch" in child_events, (child, events, child_events)
    assert "body" in child_events, (child, events, child_events)
    assert child_events.index("switch") < child_events.index("body"), (
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
    required = ("callbacks", "ThreadHandoff", "thread_delta")
    if not all(hasattr(module, name) for name in required):
        print("threadpool_schedule_contract=unavailable")
        return 0

    check_threadpool_worker_switches_before_work(module)

    print("threadpool_schedule_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
