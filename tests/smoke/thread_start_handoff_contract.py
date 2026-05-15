import importlib.util
import _thread
import threading


PROBE_MODULE = "retrace"


def join_or_fail(thread, timeout=5.0):
    thread.join(timeout)
    assert not thread.is_alive(), f"thread did not finish: {thread.name}"


def check_thread_start_handoff_does_not_block_thread_start(module):
    old_start = module.callbacks.thread_start
    handoff = module.ThreadHandoff()

    starter_ready = threading.Event()
    start_worker = threading.Event()
    start_callback_entered = threading.Event()
    thread_start_returned = threading.Event()
    target_ran = threading.Event()
    errors = []

    def start_callback():
        start_callback_entered.set()
        handoff.start()

    worker = threading.Thread(
        target=target_ran.set,
        name="thread-start-handoff-worker",
    )

    def starter():
        starter_ready.set()
        start_worker.wait()
        try:
            worker.start()
        except BaseException as exc:
            errors.append(("worker.start", repr(exc)))
        finally:
            thread_start_returned.set()

    starter_thread = threading.Thread(
        target=starter,
        name="thread-start-handoff-starter",
    )

    try:
        starter_thread.start()
        assert starter_ready.wait(5.0), "starter thread did not start"

        module.callbacks.thread_start = start_callback
        start_worker.set()

        assert start_callback_entered.wait(5.0), "thread_start callback did not run"
        assert thread_start_returned.wait(0.25), (
            "threading.Thread.start() stayed blocked while thread_start "
            "callback was parked in ThreadHandoff.start()"
        )
    finally:
        module.callbacks.thread_start = old_start
        handoff.close()
        join_or_fail(starter_thread)
        if worker.ident is not None:
            join_or_fail(worker)

    assert not errors, errors
    assert target_ran.is_set(), "worker target did not run"


def main() -> int:
    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is None:
        print("retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    required = ("callbacks", "ThreadHandoff")
    if not all(hasattr(module, name) for name in required):
        print("thread_start_handoff_contract=unavailable")
        return 0

    check_thread_start_handoff_does_not_block_thread_start(module)

    print("thread_start_handoff_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
