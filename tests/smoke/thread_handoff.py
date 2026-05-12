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


def check_api(module):
    handoff = module.ThreadHandoff()
    assert handoff(_thread.get_ident()) is None

    for args in [(), (1, 2)]:
        try:
            handoff(*args)
        except TypeError:
            pass
        else:
            raise AssertionError(f"accepted invalid args: {args!r}")

    try:
        module.ThreadHandoff(1)
    except TypeError:
        pass
    else:
        raise AssertionError("ThreadHandoff accepted constructor arguments")

    handoff.close()
    handoff.close()
    target = _thread.get_ident() ^ 1
    if target == 0:
        target = 1
    try:
        handoff(target)
    except RuntimeError as exc:
        assert str(exc) == "thread handoff is closed"
    else:
        raise AssertionError("closed ThreadHandoff accepted a handoff")
    try:
        handoff.wake(target)
    except RuntimeError as exc:
        assert str(exc) == "thread handoff is closed"
    else:
        raise AssertionError("closed ThreadHandoff accepted a wake")


def check_close_wakes_waiter(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    sleeping = threading.Event()
    returned = threading.Event()
    errors = []

    def worker():
        try:
            sleeping.set()
            handoff(main_ident)
            returned.set()
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-close-waiter")
    thread.start()
    assert sleeping.wait(5.0)
    time.sleep(0.05)
    handoff.close()
    join_or_fail(thread)
    assert returned.is_set()
    require_no_errors(errors)


def check_late_target_permit_and_gil_release(module):
    handoff = module.ThreadHandoff()
    ids = {}
    events = []
    errors = []
    ready = threading.Barrier(3)
    a_entered = threading.Event()

    def worker_a():
        try:
            ids["a"] = _thread.get_ident()
            ready.wait()
            events.append("a-before")
            a_entered.set()
            handoff(ids["b"])
            events.append("a-after")
            handoff.close()
        except BaseException as exc:
            errors.append(("a", repr(exc)))

    def worker_b():
        try:
            ids["b"] = _thread.get_ident()
            ready.wait()
            assert a_entered.wait(5.0)
            time.sleep(0.05)
            events.append("b-before")
            handoff(ids["a"])
            events.append("b-after")
        except BaseException as exc:
            errors.append(("b", repr(exc)))

    thread_a = threading.Thread(target=worker_a, name="handoff-late-a")
    thread_b = threading.Thread(target=worker_b, name="handoff-late-b")
    thread_a.start()
    thread_b.start()
    ready.wait()
    join_or_fail(thread_a)
    join_or_fail(thread_b)
    require_no_errors(errors)
    assert events[:2] == ["a-before", "b-before"], events
    assert sorted(events[2:]) == ["a-after", "b-after"], events


def check_wake_before_park_is_durable(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    ids = {}
    ready = threading.Event()
    enter = threading.Event()
    returned = threading.Event()
    errors = []

    def worker():
        try:
            ids["worker"] = _thread.get_ident()
            ready.set()
            assert enter.wait(5.0)
            handoff(main_ident)
            returned.set()
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-durable-wake")
    try:
        thread.start()
        assert ready.wait(5.0)
        handoff.wake(ids["worker"])
        enter.set()
        assert returned.wait(5.0)
        join_or_fail(thread)
        require_no_errors(errors)
    finally:
        handoff.close()
        thread.join(1.0)


def check_wake_releases_waiter_once(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    ids = {}
    ready = threading.Event()
    parked = threading.Event()
    first_returned = threading.Event()
    second_parked = threading.Event()
    second_returned = threading.Event()
    errors = []

    def worker():
        try:
            ids["worker"] = _thread.get_ident()
            ready.set()
            parked.set()
            handoff(main_ident)
            first_returned.set()
            second_parked.set()
            handoff(main_ident)
            second_returned.set()
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-wake-once")
    try:
        thread.start()
        assert ready.wait(5.0)
        assert parked.wait(5.0)
        time.sleep(0.05)
        handoff.wake(ids["worker"])
        assert first_returned.wait(5.0)
        assert second_parked.wait(5.0)
        time.sleep(0.05)
        assert not second_returned.is_set()
        handoff.wake(ids["worker"])
        assert second_returned.wait(5.0)
        join_or_fail(thread)
        require_no_errors(errors)
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
            handoff(main_ident)
            returned.set()
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-gil-release")
    try:
        thread.start()
        assert parked.wait(5.0)
        deadline = time.monotonic() + 0.25
        spins = 0
        while time.monotonic() < deadline:
            spins += 1
        assert spins > 0
        assert not returned.is_set()
        handoff.wake(ids["worker"])
        assert returned.wait(5.0)
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
    condition = threading.Condition()
    waiting = {}
    ready = threading.Barrier(3)
    rounds = 25

    def wait_until(predicate, message):
        deadline = time.monotonic() + 5.0
        with condition:
            while not predicate():
                remaining = deadline - time.monotonic()
                assert remaining > 0, message
                condition.wait(remaining)

    def worker(label):
        try:
            ids[label] = _thread.get_ident()
            ready.wait()
            for index in range(rounds):
                with condition:
                    waiting[label] = index
                    condition.notify_all()
                handoff(main_ident)
                with condition:
                    events.append((label, index))
                    condition.notify_all()
        except BaseException as exc:
            with condition:
                errors.append((label, repr(exc)))
                condition.notify_all()

    thread_a = threading.Thread(target=worker, args=("a",),
                                name="handoff-ping-a")
    thread_b = threading.Thread(target=worker, args=("b",),
                                name="handoff-ping-b")
    thread_a.start()
    thread_b.start()
    ready.wait()
    expected = []
    for index in range(rounds):
        for label in ("a", "b"):
            expected.append((label, index))
            wait_until(lambda: waiting.get(label) == index,
                       f"{label} did not park for round {index}")
            handoff.wake(ids[label])
            wait_until(lambda: expected[-1] in events,
                       f"{label} did not run round {index}")

    join_or_fail(thread_a)
    join_or_fail(thread_b)
    handoff.close()
    require_no_errors(errors)
    assert events == expected, events


def check_wake_does_not_park_current(module):
    handoff = module.ThreadHandoff()
    main_ident = _thread.get_ident()
    ids = {}
    events = []
    errors = []
    sleeping = threading.Event()

    def worker():
        try:
            ids["worker"] = _thread.get_ident()
            events.append("worker-before")
            sleeping.set()
            handoff(main_ident)
            events.append("worker-after")
        except BaseException as exc:
            errors.append(repr(exc))

    thread = threading.Thread(target=worker, name="handoff-wake-worker")
    thread.start()
    assert sleeping.wait(5.0)
    time.sleep(0.05)
    events.append("main-before")
    handoff.wake(ids["worker"])
    events.append("main-after")
    join_or_fail(thread)
    handoff.close()
    require_no_errors(errors)
    assert events[:3] == [
        "worker-before",
        "main-before",
        "main-after",
    ], events
    assert events[-1] == "worker-after", events


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
    check_close_wakes_waiter(module)
    check_late_target_permit_and_gil_release(module)
    check_wake_before_park_is_durable(module)
    check_wake_releases_waiter_once(module)
    check_waiter_releases_gil(module)
    check_ping_pong(module)
    check_wake_does_not_park_current(module)

    print("_retrace_module=builtin")
    print("thread_handoff=available")
    print("thread_handoff_close=available")
    print("thread_handoff_late_target=available")
    print("thread_handoff_durable_wake=available")
    print("thread_handoff_wake_once=available")
    print("thread_handoff_releases_gil=available")
    print("thread_handoff_ping_pong=available")
    print("thread_handoff_wake=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
