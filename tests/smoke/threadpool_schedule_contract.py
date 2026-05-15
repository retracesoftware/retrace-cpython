import concurrent.futures
import importlib.util
import _thread
import threading
import time


PROBE_MODULE = "retrace"


def check_threadpool_worker_yields_before_work(module):
    old_start = module.callbacks.thread_start
    old_yield = module.callbacks.thread_yield
    old_resume = module.callbacks.thread_resume

    events = []
    lock = threading.Lock()
    worker_ident = []
    worker_idle = threading.Event()

    def note(kind):
        ident = _thread.get_ident()
        with lock:
            events.append((kind, ident))
            if kind == "yield" and worker_ident and ident == worker_ident[0]:
                worker_idle.set()

    def warmup():
        worker_ident.append(_thread.get_ident())
        return None

    def work():
        assert worker_ident and worker_ident[0] == _thread.get_ident()
        note("body")
        time.sleep(0)
        return 1

    try:
        module.callbacks.thread_start = lambda: note("start")
        module.callbacks.thread_yield = lambda: note("yield")
        module.callbacks.thread_resume = lambda: note("resume")

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
            assert executor.submit(warmup).result(timeout=5.0) is None
            assert worker_ident, events
            assert worker_idle.wait(5.0), events
            assert executor.submit(work).result(timeout=5.0) == 1
    finally:
        module.callbacks.thread_start = old_start
        module.callbacks.thread_yield = old_yield
        module.callbacks.thread_resume = old_resume

    assert worker_ident, events
    child = worker_ident[0]
    child_events = [kind for kind, ident in events if ident == child]
    assert "body" in child_events, (child, events, child_events)

    body_index = child_events.index("body")
    assert child_events[:body_index] == ["start", "yield", "resume"], (
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

    check_threadpool_worker_yields_before_work(module)

    print("threadpool_schedule_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
