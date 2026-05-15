import importlib.util
import _thread
import threading


PUBLIC_MODULE = "retrace"


def assert_coordinates(value):
    assert type(value) is tuple
    assert len(value) >= 2
    assert len(value) % 2 == 0
    assert all(type(item) is int and 0 <= item < 2**64
               for item in value)


def frame_pairs(coordinates):
    assert_coordinates(coordinates)
    return tuple(zip(coordinates[0::2], coordinates[1::2]))


def advance_leaf_instruction(coordinates, delta):
    return (*coordinates[:-2], coordinates[-2] + delta, 0)


def apply_delta(previous, delta):
    common = delta[0]
    assert type(common) is int
    assert 0 <= common <= len(previous), (previous, delta)
    return (*previous[:common], *delta[1:])


def busy_loop(iterations=1000):
    value = 0
    for index in range(iterations):
        value += index
        value ^= index
        value += 1
    return value


def check_coordinate_shape(module):
    coordinates = tuple(module.coordinates())
    pairs = frame_pairs(coordinates)
    assert len(pairs) >= 1
    for instruction_coordinate, call_ordinal in pairs:
        assert type(instruction_coordinate) is int
        assert type(call_ordinal) is int


def check_nested_call_adds_visible_frame_pair(module):
    def inner():
        return tuple(module.coordinates())

    def outer():
        before = tuple(module.coordinates())
        inside = inner()
        after = tuple(module.coordinates())
        return before, inside, after

    before, inside, after = outer()
    assert_coordinates(before)
    assert_coordinates(inside)
    assert_coordinates(after)
    assert len(inside) == len(before) + 2
    assert len(after) == len(before)


def check_leaf_pair_order_is_instruction_then_call_ordinal(module):
    def sample():
        first = tuple(module.coordinates())
        busy_loop(32)
        second = tuple(module.coordinates())
        return first, second

    first, second = sample()
    first_pairs = frame_pairs(first)
    second_pairs = frame_pairs(second)
    assert first_pairs[-1][1] == 0
    assert second_pairs[-1][1] == 0
    assert second_pairs[-1][0] >= first_pairs[-1][0]


def check_repeated_call_site_coordinates_are_unique(module):
    def probe():
        return tuple(module.coordinates())

    def caller():
        return [probe() for _ in range(5)]

    coordinates = caller()
    assert len(set(coordinates)) == len(coordinates)
    assert all(len(item) >= 2 and len(item) % 2 == 0
               for item in coordinates)


def check_parent_call_ordinal_counts_repeated_children(module):
    coordinates = []

    def probe(value):
        coordinates.append(tuple(module.coordinates()))
        return value

    list(map(probe, range(3)))
    assert len(coordinates) == 3
    pairs = [frame_pairs(item) for item in coordinates]
    assert all(len(item) >= 2 for item in pairs)
    min_depth = min(len(item) for item in pairs)
    ordinal_owner = None
    for position in range(min_depth):
        column = [item[position] for item in pairs]
        if (
            len({pair[0] for pair in column}) == 1
            and [pair[1] for pair in column] == [0, 1, 2]
        ):
            ordinal_owner = position
            break
    assert ordinal_owner is not None, pairs
    for item in pairs:
        assert all(pair[1] == 0 for pair in item[ordinal_owner + 1:])
    assert len(set(coordinates)) == len(coordinates)


def check_call_ordinal_resets_after_parent_instruction(module):
    captures = []

    def probe(value):
        captures.append(tuple(module.coordinates()))
        return value

    def caller():
        list(map(probe, range(3)))
        return tuple(module.coordinates())

    after = caller()
    assert len(captures) == 3
    after_pairs = frame_pairs(after)
    assert after_pairs[-1][1] == 0


def check_thread_coordinates_are_thread_local(module):
    results = {}
    barrier = threading.Barrier(2)

    def worker(name):
        barrier.wait(timeout=5)
        ident = _thread.get_ident()
        implicit = tuple(module.coordinates())
        explicit = tuple(module.coordinates(ident))
        results[name] = (ident, implicit, explicit)

    threads = [
        threading.Thread(target=worker, args=("left",)),
        threading.Thread(target=worker, args=("right",)),
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=5)
        assert not thread.is_alive()

    assert set(results) == {"left", "right"}
    assert results["left"][0] != results["right"][0]
    for _, implicit, explicit in results.values():
        assert_coordinates(implicit)
        assert_coordinates(explicit)


def check_thread_delta_is_isolated_per_thread(module):
    results = {}
    barrier = threading.Barrier(2)

    def worker(name):
        state = ()
        snapshots = []
        for _ in range(3):
            barrier.wait(timeout=5)
            delta = tuple(module.thread_delta())
            state = apply_delta(state, delta)
            snapshots.append((delta, state))
            barrier.wait(timeout=5)
        results[name] = snapshots

    threads = [
        threading.Thread(target=worker, args=("left",)),
        threading.Thread(target=worker, args=("right",)),
    ]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=5)
        assert not thread.is_alive()

    assert set(results) == {"left", "right"}
    for snapshots in results.values():
        first_delta, _ = snapshots[0]
        assert first_delta[0] == 0
        for _, state in snapshots:
            assert_coordinates(state)


def check_exclude_and_include(module):
    @module.exclude
    def hidden():
        pinned = tuple(module.coordinates())

        def nested():
            return tuple(module.coordinates())

        return pinned, nested()

    pinned, nested = hidden()
    assert nested == pinned

    @module.exclude
    def hidden_with_include():
        pinned = tuple(module.coordinates())

        def visible():
            return tuple(module.coordinates())

        visible_coordinates = module.include(visible)()
        after = tuple(module.coordinates())
        return pinned, visible_coordinates, after

    pinned, visible_coordinates, after = hidden_with_include()
    assert after[:-1] == pinned[:-1]
    assert after[-1] >= pinned[-1]
    assert visible_coordinates[: len(pinned)] == pinned
    assert len(visible_coordinates) > len(pinned)


def check_call_at_past_rejected(module):
    coordinates = tuple(module.coordinates())
    for _ in range(1000):
        if coordinates[-2] > 0:
            break
        coordinates = tuple(module.coordinates())
    assert coordinates[-2] > 0

    past = (*coordinates[:-2], coordinates[-2] - 1, 0)
    try:
        module.call_at(_thread.get_ident(), past, lambda: None)
    except ValueError:
        pass
    else:
        module.call_at(None)
        raise AssertionError("past call_at coordinates were accepted")


def check_call_at_overshoot(module):
    hits = []
    overshoots = []

    def attempt(delta):
        hits.clear()
        overshoots.clear()
        base = tuple(module.coordinates())
        target = advance_leaf_instruction(base, delta)
        try:
            module.call_at(
                _thread.get_ident(),
                target,
                lambda: hits.append(target),
                lambda: overshoots.append(target),
            )
        except ValueError as exc:
            assert str(exc) == "call_at coordinates are in the past"
            return False
        try:
            busy_loop()
            assert not (hits and overshoots)
            return bool(overshoots)
        finally:
            module.call_at(None)

    for delta in range(1, 10000):
        if attempt(delta):
            break

    assert overshoots


def check_call_at_callback_is_coordinate_transparent(module):
    hits = []
    last_target_id = None

    def callback():
        pinned = tuple(module.coordinates())
        hits.append((_thread.get_ident(), pinned))

        def helper(depth):
            assert tuple(module.coordinates()) == pinned
            if depth:
                helper(depth - 1)

        helper(4)

        delta = tuple(module.thread_delta())
        assert apply_delta((), delta) == pinned

        def transparent_generator():
            assert tuple(module.coordinates()) == pinned
            yield tuple(module.coordinates())

        assert next(transparent_generator()) == pinned

    def attempt(delta):
        nonlocal last_target_id
        hits.clear()
        ready = threading.Event()
        go = threading.Event()
        errors = []
        idents = {}

        def worker():
            try:
                idents["target"] = _thread.get_ident()
                ready.set()
                assert go.wait(5)
                busy_loop()
            except BaseException as exc:
                errors.append(exc)

        thread = threading.Thread(target=worker)
        thread.start()
        assert ready.wait(5)

        last_target_id = idents["target"]
        base = tuple(module.coordinates(last_target_id))
        target = advance_leaf_instruction(base, delta)

        try:
            module.call_at(last_target_id, target, callback, lambda: None)
        except ValueError as exc:
            assert str(exc) == "call_at coordinates are in the past"
            go.set()
            thread.join(timeout=5)
            assert not thread.is_alive()
            assert not errors
            return False
        try:
            go.set()
            thread.join(timeout=5)
            assert not thread.is_alive()
            assert not errors
            return bool(hits)
        finally:
            module.call_at(None)

    for delta in range(1, 64):
        if attempt(delta):
            break

    assert hits
    assert hits[0][0] == last_target_id


def main() -> int:
    if importlib.util.find_spec(PUBLIC_MODULE) is None:
        print("retrace_module=unavailable")
        return 0

    module = __import__(PUBLIC_MODULE)
    required = (
        "coordinates",
        "thread_delta",
        "exclude",
        "include",
        "call_at",
    )
    if not all(hasattr(module, name) for name in required):
        print("coordinate_contracts=unavailable")
        return 0

    check_coordinate_shape(module)
    check_nested_call_adds_visible_frame_pair(module)
    check_leaf_pair_order_is_instruction_then_call_ordinal(module)
    check_repeated_call_site_coordinates_are_unique(module)
    check_parent_call_ordinal_counts_repeated_children(module)
    check_call_ordinal_resets_after_parent_instruction(module)
    check_thread_coordinates_are_thread_local(module)
    check_thread_delta_is_isolated_per_thread(module)
    check_exclude_and_include(module)
    check_call_at_past_rejected(module)
    check_call_at_overshoot(module)
    check_call_at_callback_is_coordinate_transparent(module)

    print("retrace_module=available")
    print("coordinate_contracts=available")
    print("thread_delta_isolation=available")
    print("call_at_coordinate_transparency=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
