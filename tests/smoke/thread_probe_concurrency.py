import importlib.util
import _thread
import sys
import threading
import time


PROBE_MODULE = "_retrace"


def join_or_fail(thread, timeout=5.0):
    thread.join(timeout)
    assert not thread.is_alive(), f"thread did not finish: {thread.name}"


def apply_delta(stack, delta):
    common = delta[0]
    assert 0 <= common <= len(stack), (common, stack, delta)
    del stack[common:]
    stack.extend(delta[1:])


def run_busy_loop(stop, ready=None):
    value = 0
    if ready is not None:
        ready.set()
    while not stop.is_set():
        value ^= 0x9E3779B9
        value = (value * 1103515245 + 12345) & ((1 << 64) - 1)
    return value


def check_thread_start_callback_before_first_frame_is_empty(module):
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)

    done = _thread.allocate_lock()
    done.acquire()
    child_ident = None
    worker_entered = False
    start_hits = []
    resume_hits = []
    worker_coordinates = []
    errors = []

    def start_callback():
        try:
            ident = _thread.get_ident()
            helper_coordinates = []

            def helper(depth):
                helper_coordinates.append(tuple(module.coordinates()))
                if depth:
                    helper(depth - 1)

            helper(4)
            snapshot = {
                "ident": ident,
                "worker_entered": worker_entered,
                "coordinates": tuple(module.coordinates()),
                "delta": tuple(module.thread_delta()),
                "helper_coordinates": helper_coordinates,
            }
            start_hits.append(snapshot)
        except BaseException as exc:
            errors.append(("start callback", repr(exc)))

    def resume_callback():
        try:
            resume_hits.append({
                "ident": _thread.get_ident(),
                "worker_entered": worker_entered,
                "coordinates": tuple(module.coordinates()),
            })
        except BaseException as exc:
            errors.append(("resume callback", repr(exc)))

    def worker():
        nonlocal worker_entered
        try:
            worker_coordinates.append(tuple(module.coordinates()))
            worker_entered = True
            for _ in range(16):
                time.sleep(0)
        except BaseException as exc:
            errors.append(("worker", repr(exc)))
        finally:
            done.release()

    try:
        module.set_thread_start_callback(start_callback)
        module.set_thread_resume_callback(resume_callback)
        child_ident = _thread.start_new_thread(worker, ())
        assert done.acquire(timeout=5.0), "worker did not finish"
    finally:
        module.set_thread_start_callback(None)
        module.set_thread_resume_callback(None)
        sys.setswitchinterval(old_interval)

    assert not errors, errors
    child_hits = [
        hit for hit in start_hits
        if hit["ident"] == child_ident
    ]
    assert child_hits, (child_ident, start_hits)

    first_child_hit = child_hits[0]
    assert first_child_hit["coordinates"] == (), first_child_hit
    assert first_child_hit["delta"] == (0,), first_child_hit
    assert first_child_hit["helper_coordinates"], first_child_hit
    assert all(
        coordinates == ()
        for coordinates in first_child_hit["helper_coordinates"]
    ), first_child_hit
    assert not first_child_hit["worker_entered"], first_child_hit
    assert worker_coordinates and worker_coordinates[0] != (), worker_coordinates
    early_resumes = [
        hit for hit in resume_hits
        if hit["ident"] == child_ident and not hit["worker_entered"]
    ]
    assert not early_resumes, early_resumes


def check_call_at_hits_intended_thread(module):
    hits = []
    overshoots = []
    last_idents = None
    last_coordinates = None

    def attempt(delta):
        overshoot_start = len(overshoots)
        go = threading.Event()
        ready = [threading.Event(), threading.Event()]
        idents = {}
        errors = []

        def worker(name, index):
            try:
                idents[name] = _thread.get_ident()
                ready[index].set()
                assert go.wait(5.0)
                value = 0
                for inner in range(200):
                    value += inner
                    value ^= inner
                    value += 1
            except BaseException as exc:
                errors.append((name, repr(exc)))

        threads = [
            threading.Thread(target=worker, args=("bystander", 0),
                             name="call-at-bystander"),
            threading.Thread(target=worker, args=("target", 1),
                             name="call-at-target"),
        ]
        for thread in threads:
            thread.start()
        for event in ready:
            assert event.wait(5.0)

        captured_coordinates = {
            name: tuple(module.coordinates(ident))
            for name, ident in idents.items()
        }
        target_ident = idents["target"]
        base = captured_coordinates["target"]
        target = (*base[:-1], base[-1] + delta)

        def hit_callback():
            hits.append(_thread.get_ident())

        def overshoot_callback():
            overshoots.append(_thread.get_ident())

        try:
            module.call_at(
                target_ident, target, hit_callback, overshoot_callback)
        except ValueError:
            go.set()
            for thread in threads:
                join_or_fail(thread)
            assert not errors, errors
            return idents, captured_coordinates

        try:
            go.set()
            for thread in threads:
                join_or_fail(thread)
        finally:
            module.call_at(None)

        assert not errors, errors
        assert all(item == target_ident for item in overshoots[overshoot_start:]), (
            overshoots[overshoot_start:],
            target_ident,
            idents,
        )
        return idents, captured_coordinates

    for delta in range(1, 32):
        last_idents, last_coordinates = attempt(delta)
        if hits:
            break

    assert set(last_coordinates) == {"target", "bystander"}, last_coordinates
    assert hits, "call_at callback did not fire"
    target_ident = last_idents["target"]
    assert set(hits) == {target_ident}, (hits, target_ident, last_idents)


def check_call_at_overshoot(module):
    hits = []
    overshoots = []

    def run_body():
        value = 0
        for index in range(2000):
            value += index
            value ^= index
            value += 1
        return value

    for delta in range(1, 1000):
        base = tuple(module.coordinates())
        target = (*base[:-1], base[-1] + delta)
        try:
            module.call_at(
                _thread.get_ident(),
                target,
                lambda: hits.append(target),
                lambda: overshoots.append(target),
            )
        except ValueError:
            continue
        run_body()
        module.call_at(None)
        assert not (hits and overshoots), (hits, overshoots)
        if overshoots:
            break

    assert overshoots, "overshoot callback did not fire"


def check_call_at_past_overshoot(module):
    hits = []
    overshoots = []
    ident = _thread.get_ident()

    coordinates = tuple(module.coordinates())
    for _ in range(1000):
        if coordinates[-1] > 0:
            break
        coordinates = tuple(module.coordinates())
    assert coordinates[-1] > 0, coordinates

    past = (*coordinates[:-1], coordinates[-1] - 1)

    def hit_callback():
        hits.append(_thread.get_ident())

    def overshoot_callback():
        overshoots.append(_thread.get_ident())

    try:
        module.call_at(
            ident, past, hit_callback, overshoot_callback)
        deadline = time.monotonic() + 5.0
        value = 0
        while not overshoots and time.monotonic() < deadline:
            for index in range(200):
                value += index
                value ^= index
                value += 1
            time.sleep(0)
    finally:
        module.call_at(None)

    assert not hits, hits
    assert overshoots == [ident], (overshoots, ident)


def check_thread_delta_is_per_thread(module):
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)
    start = threading.Barrier(3)
    errors = []
    roots = {}

    def worker(index):
        stack = []
        try:
            roots[index] = tuple(module.coordinates())[0]
            start.wait()
            for _ in range(200):
                delta = tuple(module.thread_delta())
                assert delta, delta
                apply_delta(stack, delta)
                live = tuple(module.coordinates())
                assert tuple(stack[:-1]) == live[:-1], (
                    index,
                    stack,
                    live,
                    delta,
                )
                time.sleep(0)
        except BaseException as exc:
            errors.append((index, repr(exc)))

    threads = [
        threading.Thread(target=worker, args=(index,), name=f"delta-{index}")
        for index in range(2)
    ]
    try:
        for thread in threads:
            thread.start()
        start.wait()
        for thread in threads:
            join_or_fail(thread)
    finally:
        sys.setswitchinterval(old_interval)

    assert not errors, errors
    assert set(roots) == {0, 1}, roots


def check_schedule_callback_identities(module):
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)
    stop = threading.Event()
    ready = threading.Barrier(3)
    hits = []
    errors = []
    worker_idents = set()

    def snapshot(kind):
        try:
            ident = _thread.get_ident()
            active = set(getattr(threading, "_active", {}).keys())
            limbo = {
                thread.ident
                for thread in getattr(threading, "_limbo", {}).values()
                if thread.ident is not None
            }
            hits.append((kind, ident, ident in active, ident in limbo))
        except BaseException as exc:
            errors.append((kind, repr(exc)))

    def yield_callback():
        snapshot("yield")

    def resume_callback():
        snapshot("resume")

    def worker():
        worker_idents.add(_thread.get_ident())
        ready.wait()
        run_busy_loop(stop)

    threads = [
        threading.Thread(target=worker, name=f"callback-identity-{index}")
        for index in range(2)
    ]
    try:
        module.set_thread_yield_callback(yield_callback)
        module.set_thread_resume_callback(resume_callback)
        for thread in threads:
            thread.start()
        ready.wait()

        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline and len(hits) < 10:
            time.sleep(0)
    finally:
        stop.set()
        for thread in threads:
            join_or_fail(thread)
        module.set_thread_yield_callback(None)
        module.set_thread_resume_callback(None)
        sys.setswitchinterval(old_interval)

    assert not errors, errors
    assert hits, "no scheduling callbacks observed"
    unknown = [hit for hit in hits if not (hit[2] or hit[3])]
    assert not unknown, unknown[:10]
    assert worker_idents & {ident for _kind, ident, _active, _limbo in hits}, (
        worker_idents,
        hits[:10],
    )


def main() -> int:
    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is None:
        print("_retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    required = (
        "coordinates",
        "call_at",
        "set_thread_start_callback",
        "set_thread_resume_callback",
        "set_thread_yield_callback",
        "thread_delta",
    )
    if not all(hasattr(module, name) for name in required):
        print("thread_probe_concurrency=unavailable")
        return 0

    check_call_at_hits_intended_thread(module)
    check_call_at_overshoot(module)
    check_call_at_past_overshoot(module)
    check_thread_start_callback_before_first_frame_is_empty(module)
    check_thread_delta_is_per_thread(module)
    check_schedule_callback_identities(module)

    print("_retrace_module=builtin")
    print("call_at_intended_thread=available")
    print("call_at_overshoot=available")
    print("call_at_past_overshoot=available")
    print("thread_start_callback_empty_bootstrap_cursor=available")
    print("thread_delta_per_thread=available")
    print("schedule_callback_identity=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
