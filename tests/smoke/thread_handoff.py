import importlib.util
import _thread
import threading
import time


PROBE_MODULE = "_retrace"


def join_or_fail(thread, timeout=5.0):
    thread.join(timeout)
    assert not thread.is_alive(), f"thread did not finish: {thread.name}"


def require_no_errors(errors):
    assert not errors, errors


def assert_raises(exc_type, func, message=None):
    try:
        func()
    except exc_type as exc:
        if message is not None:
            assert str(exc) == message
        return
    raise AssertionError(f"{func!r} did not raise {exc_type.__name__}")


def check_api(module):
    handoff = module.ThreadHandoff()
    assert not callable(handoff)
    assert not hasattr(handoff, "wake")
    assert handoff.to(_thread.get_ident()) is None
    assert module.ThreadHandoff(timeout=None).to(_thread.get_ident()) is None
    assert module.ThreadHandoff(0).to(_thread.get_ident()) is None

    assert_raises(TypeError, lambda: handoff.start(1))
    assert_raises(TypeError, lambda: handoff.to())
    assert_raises(TypeError, lambda: handoff.to(1, 2))
    assert_raises(ValueError, lambda: handoff.to(0))
    assert_raises(TypeError, lambda: module.ThreadHandoff(timeout="soon"))
    assert_raises(ValueError, lambda: module.ThreadHandoff(timeout=-1))

    handoff.close()
    handoff.close()
    target = _thread.get_ident() ^ 1
    if target == 0:
        target = 1
    assert_raises(RuntimeError, handoff.start, "thread handoff is closed")
    assert_raises(RuntimeError, lambda: handoff.to(target),
                  "thread handoff is closed")


def check_timeout(module):
    handoff = module.ThreadHandoff(timeout=0)
    target = _thread.get_ident() ^ 1
    if target == 0:
        target = 1

    assert_raises(TimeoutError, handoff.start, "thread handoff timed out")
    assert_raises(TimeoutError, lambda: handoff.to(target),
                  "thread handoff timed out")
    handoff.close()


def check_close_wakes_start_waiter(module):
    handoff = module.ThreadHandoff()
    parked = threading.Event()
    returned = threading.Event()
    errors = []

    def worker():
        try:
            parked.set()
            handoff.start()
            returned.set()
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-close-start")
    thread.start()
    try:
        assert parked.wait(5.0)
        time.sleep(0.05)
        assert not returned.is_set()
        handoff.close()
        join_or_fail(thread)
        assert returned.is_set()
        require_no_errors(errors)
    finally:
        handoff.close()
        thread.join(1.0)


def check_start_parks_until_to(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    ids = {}
    events = []
    ready = threading.Event()
    errors = []

    def worker():
        try:
            ids["worker"] = _thread.get_ident()
            events.append("worker-before-start")
            ready.set()
            handoff.start()
            events.append("worker-after-start")
            handoff.to(main_ident)
            events.append("worker-after-to")
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-start-park")
    thread.start()
    try:
        assert ready.wait(5.0)
        time.sleep(0.05)
        assert events == ["worker-before-start"], events
        handoff.to(ids["worker"])
        events.append("main-after-to")
        handoff.close()
        join_or_fail(thread)
        require_no_errors(errors)
        assert events == [
            "worker-before-start",
            "worker-after-start",
            "main-after-to",
            "worker-after-to",
        ], events
    finally:
        handoff.close()
        thread.join(1.0)


def check_to_before_start_is_durable(module):
    handoff = module.ThreadHandoff()
    ids = {}
    events = []
    target_ready = threading.Event()
    controller_ready = threading.Event()
    controller_entered = threading.Event()
    allow_target_start = threading.Event()
    errors = []

    def target():
        try:
            ids["target"] = _thread.get_ident()
            target_ready.set()
            assert allow_target_start.wait(5.0)
            handoff.start()
            events.append("target-start-returned")
            handoff.to(ids["controller"])
            events.append("target-after-to")
        except BaseException as exc:
            errors.append(("target", repr(exc)))

    def controller():
        try:
            ids["controller"] = _thread.get_ident()
            controller_ready.set()
            controller_entered.set()
            handoff.to(ids["target"])
            events.append("controller-returned")
        except BaseException as exc:
            errors.append(("controller", repr(exc)))

    target_thread = threading.Thread(target=target,
                                     name="handoff-durable-target")
    controller_thread = threading.Thread(target=controller,
                                         name="handoff-durable-controller")
    target_thread.start()
    try:
        assert target_ready.wait(5.0)
        controller_thread.start()
        assert controller_ready.wait(5.0)
        assert controller_entered.wait(5.0)
        time.sleep(0.05)
        assert events == [], events
        allow_target_start.set()
        join_or_fail(controller_thread)
        assert events == [
            "target-start-returned",
            "controller-returned",
        ], events
        handoff.close()
        join_or_fail(target_thread)
        require_no_errors(errors)
        assert events[-1] == "target-after-to", events
    finally:
        handoff.close()
        controller_thread.join(1.0)
        target_thread.join(1.0)


def check_to_releases_waiter_once(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    ids = {}
    events = []
    ready = threading.Event()
    errors = []

    def worker():
        try:
            ids["worker"] = _thread.get_ident()
            ready.set()
            handoff.start()
            events.append("first-turn")
            handoff.to(main_ident)
            events.append("second-turn")
            handoff.to(main_ident)
            events.append("after-final-close")
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-release-once")
    thread.start()
    try:
        assert ready.wait(5.0)
        time.sleep(0.05)
        handoff.to(ids["worker"])
        assert events == ["first-turn"], events
        time.sleep(0.05)
        assert events == ["first-turn"], events
        handoff.to(ids["worker"])
        assert events == ["first-turn", "second-turn"], events
        handoff.close()
        join_or_fail(thread)
        require_no_errors(errors)
        assert events == [
            "first-turn",
            "second-turn",
            "after-final-close",
        ], events
    finally:
        handoff.close()
        thread.join(1.0)


def check_waiter_releases_gil(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    ids = {}
    parked = threading.Event()
    returned = threading.Event()
    errors = []

    def worker():
        try:
            ids["worker"] = _thread.get_ident()
            parked.set()
            handoff.start()
            returned.set()
            handoff.to(main_ident)
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-gil-release")
    thread.start()
    try:
        assert parked.wait(5.0)
        deadline = time.monotonic() + 0.25
        spins = 0
        while time.monotonic() < deadline:
            spins += 1
        assert spins > 0
        assert not returned.is_set()
        handoff.to(ids["worker"])
        assert returned.is_set()
        handoff.close()
        join_or_fail(thread)
        require_no_errors(errors)
    finally:
        handoff.close()
        thread.join(1.0)


def check_ping_pong(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    ids = {}
    events = []
    errors = []
    ready = threading.Barrier(3)
    rounds = 25

    def worker(label):
        try:
            ids[label] = _thread.get_ident()
            ready.wait()
            handoff.start()
            for index in range(rounds):
                events.append((label, index))
                handoff.to(main_ident)
        except BaseException as exc:
            errors.append((label, repr(exc)))

    thread_a = threading.Thread(target=worker, args=("a",),
                                name="handoff-ping-a")
    thread_b = threading.Thread(target=worker, args=("b",),
                                name="handoff-ping-b")
    thread_a.start()
    thread_b.start()
    try:
        ready.wait()
        expected = []
        for index in range(rounds):
            for label in ("a", "b"):
                expected.append((label, index))
                handoff.to(ids[label])
                assert events == expected, events

        handoff.close()
        join_or_fail(thread_a)
        join_or_fail(thread_b)
        require_no_errors(errors)
    finally:
        handoff.close()
        thread_a.join(1.0)
        thread_b.join(1.0)


def main() -> int:
    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is None:
        print("_retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    if not hasattr(module, "ThreadHandoff"):
        print("thread_handoff=unavailable")
        return 0

    check_api(module)
    check_timeout(module)
    check_close_wakes_start_waiter(module)
    check_start_parks_until_to(module)
    check_to_before_start_is_durable(module)
    check_to_releases_waiter_once(module)
    check_waiter_releases_gil(module)
    check_ping_pong(module)

    print("_retrace_module=builtin")
    print("thread_handoff=available")
    print("thread_handoff_close=available")
    print("thread_handoff_timeout=available")
    print("thread_handoff_start=available")
    print("thread_handoff_to=available")
    print("thread_handoff_durable_to=available")
    print("thread_handoff_to_once=available")
    print("thread_handoff_releases_gil=available")
    print("thread_handoff_ping_pong=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
