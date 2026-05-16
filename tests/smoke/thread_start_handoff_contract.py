import importlib.util
import _thread
import threading


PROBE_MODULE = "retrace"


def check_thread_switch_callback_does_not_hide_thread_body(module):
    old_switch = module.callbacks.thread_switch

    switch_entered = threading.Event()
    body_entered = threading.Event()
    errors = []
    seen_ids = []

    def on_switch(previous_delta, next_thread_id):
        try:
            seen_ids.append(next_thread_id)
            switch_entered.set()
        except BaseException as exc:
            errors.append(("switch", repr(exc)))

    def worker():
        try:
            seen_ids.append(_thread.get_ident())
            body_entered.set()
        except BaseException as exc:
            errors.append(("worker", repr(exc)))

    try:
        module.callbacks.thread_switch = on_switch
        thread = threading.Thread(
            target=worker,
            name="thread-switch-handoff-contract",
        )
        thread.start()
        assert switch_entered.wait(5.0), "thread_switch callback did not run"
        assert body_entered.wait(5.0), "worker body did not run"
        thread.join(5.0)
        assert not thread.is_alive(), "worker did not finish"
    finally:
        module.callbacks.thread_switch = old_switch

    assert not errors, errors
    assert len(set(seen_ids)) == 1, seen_ids


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
        print("thread_start_handoff_contract=unavailable")
        return 0

    check_thread_switch_callback_does_not_hide_thread_body(module)

    print("thread_start_handoff_contract=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
