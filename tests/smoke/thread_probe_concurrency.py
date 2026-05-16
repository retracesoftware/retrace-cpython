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


def advance_leaf_instruction(coordinates, delta):
    return (*coordinates[:-2], coordinates[-2] + delta, 0)


def run_busy_loop(stop, ready=None):
    value = 0
    if ready is not None:
        ready.set()
    while not stop.is_set():
        value ^= 0x9E3779B9
        value = (value * 1103515245 + 12345) & ((1 << 64) - 1)
    return value


def check_thread_switch_callback_coordinates(module):
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)

    stop = threading.Event()
    ready = threading.Barrier(3)
    hits = []
    errors = []

    def switch_callback(previous_delta, next_thread_id):
        try:
            coordinates = tuple(module.coordinates())
            coordinate_hash = module.hash()
            helper_coordinates = []

            def helper(depth):
                helper_coordinates.append(
                    (tuple(module.coordinates()), module.hash()))
                if depth:
                    helper(depth - 1)

            helper(4)
            hits.append({
                "ident": _thread.get_ident(),
                "next_thread_id": next_thread_id,
                "previous_delta": tuple(previous_delta),
                "coordinates": coordinates,
                "hash": coordinate_hash,
                "helper_coordinates": helper_coordinates,
            })
        except BaseException as exc:
            errors.append(("thread_switch callback", repr(exc)))

    def worker():
        try:
            ready.wait()
            run_busy_loop(stop)
        except BaseException as exc:
            errors.append(("worker", repr(exc)))

    threads = [
        threading.Thread(target=worker, name=f"switch-coordinates-{index}")
        for index in range(2)
    ]

    try:
        module.set_thread_switch_callback(switch_callback)
        for thread in threads:
            thread.start()
        ready.wait()
        deadline = time.monotonic() + 2.0
        while not hits and time.monotonic() < deadline:
            time.sleep(0)
    finally:
        stop.set()
        for thread in threads:
            join_or_fail(thread)
        module.set_thread_switch_callback(None)
        sys.setswitchinterval(old_interval)

    assert not errors, errors
    assert hits, "no thread_switch callbacks observed"
    for hit in hits:
        assert hit["previous_delta"]
        assert hit["previous_delta"][0] >= 0
        assert hit["next_thread_id"] == hit["ident"], hit
        assert all(
            coordinates == hit["coordinates"] and
            coordinate_hash == hit["hash"]
            for coordinates, coordinate_hash in hit["helper_coordinates"]
        ), hit


def check_thread_switch_callback_space_filter(module):
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)

    space_id = module.allocate_space_id()
    errors = []

    def run_phase(callback_space_id, expected_label):
        hits = []
        worker_idents = {"root": set(), "space": set()}
        ready = threading.Barrier(3)
        stop = threading.Event()

        def switch_callback(previous_delta, next_thread_id):
            try:
                hits.append((_thread.get_ident(), next_thread_id))
            except BaseException as exc:
                errors.append(("thread_switch callback", repr(exc)))

        def worker(label):
            try:
                worker_idents[label].add(_thread.get_ident())
                ready.wait()
                run_busy_loop(stop)
            except BaseException as exc:
                errors.append(("worker", label, repr(exc)))

        root_thread = threading.Thread(
            target=worker, args=("root",), name="root-space-callback-filter")
        space_thread = threading.Thread(
            target=worker, args=("space",), name="extra-space-callback-filter")

        try:
            root_thread.start()
            module.run_in_space(space_id, space_thread.start)
            ready.wait()
            module.set_thread_switch_callback(
                switch_callback, callback_space_id)

            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                hit_idents = {ident for ident, _next_ident in hits}
                if worker_idents[expected_label] & hit_idents:
                    break
                time.sleep(0)
        finally:
            stop.set()
            join_or_fail(root_thread)
            join_or_fail(space_thread)
            module.set_thread_switch_callback(None, callback_space_id)

        return worker_idents, hits

    try:
        worker_idents, hits = run_phase(None, "root")
        hit_idents = {ident for ident, _next_ident in hits}
        assert worker_idents["root"] & hit_idents, (worker_idents, hits)
        assert not (worker_idents["space"] & hit_idents), (worker_idents, hits)

        worker_idents, hits = run_phase(space_id, "space")
        hit_idents = {ident for ident, _next_ident in hits}
        assert not (worker_idents["root"] & hit_idents), (worker_idents, hits)
        assert worker_idents["space"] & hit_idents, (worker_idents, hits)
    finally:
        module.set_thread_switch_callback(None)
        module.set_thread_switch_callback(None, space_id)
        sys.setswitchinterval(old_interval)

    assert not errors, errors


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
        target = advance_leaf_instruction(base, delta)

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
        target = advance_leaf_instruction(base, delta)
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
        if coordinates[-2] > 0:
            break
        coordinates = tuple(module.coordinates())
    assert coordinates[-2] > 0, coordinates

    past = (*coordinates[:-2], coordinates[-2] - 1, 0)

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
                assert len(stack) == len(live), (
                    index,
                    stack,
                    live,
                    delta,
                )
                assert len(stack) % 2 == 0, (index, stack)
                assert all(type(item) is int and 0 <= item < 2**64
                           for item in stack), (index, stack)
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


def check_thread_switch_callback_identities(module):
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

    def switch_callback(previous_delta, next_thread_id):
        snapshot("switch")

    def worker():
        worker_idents.add(_thread.get_ident())
        ready.wait()
        run_busy_loop(stop)

    threads = [
        threading.Thread(target=worker, name=f"callback-identity-{index}")
        for index in range(2)
    ]
    try:
        for thread in threads:
            thread.start()
        ready.wait()
        module.set_thread_switch_callback(switch_callback)

        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline and len(hits) < 10:
            time.sleep(0)
    finally:
        stop.set()
        for thread in threads:
            join_or_fail(thread)
        module.set_thread_switch_callback(None)
        sys.setswitchinterval(old_interval)

    assert not errors, errors
    assert hits, "no scheduling callbacks observed"
    unknown = [
        hit for hit in hits
        if not (hit[2] or hit[3]) and hit[1] not in worker_idents
    ]
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
        "set_thread_switch_callback",
        "get_thread_switch_callback",
        "allocate_space_id",
        "run_in_space",
        "thread_delta",
    )
    if not all(hasattr(module, name) for name in required):
        print("thread_probe_concurrency=unavailable")
        return 0

    check_call_at_hits_intended_thread(module)
    check_call_at_overshoot(module)
    check_call_at_past_overshoot(module)
    check_thread_switch_callback_coordinates(module)
    check_thread_switch_callback_space_filter(module)
    check_thread_delta_is_per_thread(module)
    check_thread_switch_callback_identities(module)

    print("_retrace_module=builtin")
    print("call_at_intended_thread=available")
    print("call_at_overshoot=available")
    print("call_at_past_overshoot=available")
    print("thread_switch_callback_coordinates=available")
    print("thread_switch_callback_space_filter=available")
    print("thread_delta_per_thread=available")
    print("thread_switch_callback_identity=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
