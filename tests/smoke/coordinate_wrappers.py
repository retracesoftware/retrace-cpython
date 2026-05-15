import importlib.util
import inspect
import _thread
import threading


PROBE_MODULE = "_retrace"
THREAD_ID_SPACE_SHIFT = 48
THREAD_ID_HASH_MASK = (1 << THREAD_ID_SPACE_SHIFT) - 1
THREAD_ID_SPACE_MASK = (1 << 16) - 1


def require_wrappers(module):
    missing = [
        name for name in (
            "disable",
            "enable",
            "exclude",
            "include",
            "with_new_coordinates",
        )
        if not hasattr(module, name)
    ]
    if missing:
        print("coordinate_wrappers=unavailable")
        return False
    return True


def check_exclude_decorator(module):
    def child():
        return module.coordinates()

    @module.exclude
    def hidden(a, b=2, *, c=3):
        return a + b + c, module.coordinates(), child()

    def visible():
        before = module.coordinates()
        value, inside, child_inside = hidden(1, c=4)
        after = module.coordinates()
        return value, before, inside, child_inside, after

    value, before, inside, child_inside, after = visible()
    assert value == 7
    assert len(inside) == len(before)
    assert len(child_inside) == len(before)
    assert len(after) == len(before)
    assert inspect.unwrap(hidden).__name__ == "hidden"


def check_include_decorator(module):
    @module.include
    def visible_leaf():
        return module.coordinates()

    @module.exclude
    def hidden():
        hidden_coords = module.coordinates()
        included_coords = visible_leaf()
        return hidden_coords, included_coords, module.coordinates()

    def visible():
        before = module.coordinates()
        hidden_coords, included_coords, after_include = hidden()
        after = module.coordinates()
        return before, hidden_coords, included_coords, after_include, after

    before, hidden_coords, included_coords, after_include, after = visible()
    assert len(hidden_coords) == len(before)
    assert len(included_coords) == len(before) + 2
    assert len(after_include) == len(before)
    assert len(after) == len(before)


def check_disable_enable_decorators(module):
    calls = []

    def child():
        return module.coordinates()

    @module.disable
    def hidden(value):
        calls.append(value)
        return module.coordinates(), child(), value

    assert calls == []
    assert inspect.unwrap(hidden).__name__ == "hidden"

    def visible():
        before = module.coordinates()
        hidden_coords, child_coords, value = hidden(17)
        after = module.coordinates()
        return before, hidden_coords, child_coords, value, after

    before, hidden_coords, child_coords, value, after = visible()
    assert value == 17
    assert calls == [17]
    assert len(hidden_coords) == len(before)
    assert len(child_coords) == len(before)
    assert len(after) == len(before)

    @module.enable
    def visible_leaf():
        return module.coordinates()

    @module.disable
    def disabled_parent():
        disabled_coords = module.coordinates()
        enabled_coords = visible_leaf()
        return disabled_coords, enabled_coords, module.coordinates()

    before = module.coordinates()
    disabled_coords, enabled_coords, after_enable = disabled_parent()
    after = module.coordinates()
    assert len(disabled_coords) == len(before)
    assert len(enabled_coords) == len(before) + 2
    assert len(after_enable) == len(before)
    assert len(after) == len(before)


def check_method_binding(module):
    class Target:
        def __init__(self):
            self.value = 11

        @module.exclude
        def hidden(self, extra=0):
            return self.value + extra, module.coordinates()

        @module.include
        def visible(self):
            return self.value, module.coordinates()

    target = Target()

    def visible_call():
        before = module.coordinates()
        value, hidden_coords = target.hidden(extra=5)
        unbound_value, unbound_coords = Target.hidden(target, 7)
        visible_value, visible_coords = target.visible()
        after = module.coordinates()
        return (
            before,
            value,
            hidden_coords,
            unbound_value,
            unbound_coords,
            visible_value,
            visible_coords,
            after,
        )

    (
        before,
        value,
        hidden_coords,
        unbound_value,
        unbound_coords,
        visible_value,
        visible_coords,
        after,
    ) = visible_call()
    assert value == 16
    assert len(hidden_coords) == len(before)
    assert unbound_value == 18
    assert len(unbound_coords) == len(before)
    assert visible_value == 11
    assert len(visible_coords) == len(before) + 2
    assert len(after) == len(before)


def check_exception_restores_state(module):
    class Marker(Exception):
        pass

    @module.exclude
    def hidden_raises():
        raise Marker

    def visible_len():
        return len(module.coordinates())

    before = visible_len()
    try:
        hidden_raises()
    except Marker:
        pass
    else:
        raise AssertionError("excluded callable did not raise")
    after = visible_len()
    assert after == before


def check_with_new_coordinates(module):
    space_id = module.allocate_space_id()

    def leaf(label, *, scale):
        return (
            module.coordinates(None, 0, space_id),
            module.hash(space_id),
            module.coordinates(),
            label,
            scale,
        )

    def parent():
        before = module.coordinates()
        assert module.coordinates(None, 0, space_id) is None
        first = module.run_in_space(space_id, leaf, "first", scale=2)
        middle = module.coordinates()
        second = module.run_in_space(space_id, leaf, "second", scale=3)
        after = module.coordinates()
        return before, middle, after, first, second

    before, middle, after, first, second = parent()
    first_coords, first_hash, first_root, first_label, first_scale = first
    second_coords, second_hash, second_root, second_label, second_scale = second

    assert len(first_coords) == 2
    assert len(second_coords) == 2
    assert first_hash != second_hash
    assert len(first_root) == len(before)
    assert len(second_root) == len(before)
    assert first_label == "first"
    assert first_scale == 2
    assert second_label == "second"
    assert second_scale == 3
    assert len(middle) == len(before)
    assert len(after) == len(before)

    nested_space_id = module.allocate_space_id()

    def nested_parent():
        parent_coords = module.coordinates(None, 0, nested_space_id)

        def child():
            return module.coordinates(None, 0, nested_space_id)

        return parent_coords, child()

    root_coords, child_coords = module.run_in_space(
        nested_space_id, nested_parent)
    assert len(root_coords) == 2
    assert len(child_coords) == 4
    assert child_coords[1] >= root_coords[1]


def check_with_new_coordinates_restores_after_exception(module):
    class Marker(Exception):
        pass

    space_id = module.allocate_space_id()

    def raises():
        coords = module.coordinates(None, 0, space_id)
        assert len(coords) == 2
        raise Marker

    before = module.coordinates()
    try:
        module.run_in_space(space_id, raises)
    except Marker:
        pass
    else:
        raise AssertionError("with_new_coordinates callable did not raise")
    after = module.coordinates()
    assert len(after) == len(before)


def check_with_new_coordinates_thread_ids(module):
    def child_thread_id():
        ids = []

        def worker():
            ids.append(_thread.get_ident())

        thread = threading.Thread(target=worker,
                                  name="fresh-coordinate-thread")
        thread.start()
        thread.join(5.0)
        assert not thread.is_alive()
        assert len(ids) == 1
        return ids[0]

    first = module.with_new_coordinates(child_thread_id)
    second = module.with_new_coordinates(child_thread_id)
    assert type(first) is int and first > 0
    assert type(second) is int and second > 0


def check_fresh_spaces_match(module):
    def sample(space_id):
        ids = []

        def worker():
            ids.append(_thread.get_ident())

        def body():
            done = _thread.allocate_lock()
            done.acquire()

            def locked_worker():
                try:
                    worker()
                finally:
                    done.release()

            coords = module.coordinates(None, 0, space_id)
            coordinate_hash = module.hash(space_id)
            ident = _thread.start_new_thread(locked_worker, ())
            assert done.acquire(timeout=5.0)
            assert len(ids) == 1
            assert ids[0] == ident
            return coords, coordinate_hash, ids[0]

        return module.run_in_space(space_id, body)

    for _ in range(4):
        sample(module.allocate_space_id())

    seen = {}
    samples = []
    for _ in range(12):
        space_id = module.allocate_space_id()
        current = sample(space_id)
        samples.append(current)
        assert current[2] >> THREAD_ID_SPACE_SHIFT == (
            space_id & THREAD_ID_SPACE_MASK)
        assert current[2] & THREAD_ID_HASH_MASK != 0
        key = (*current[:2], current[2] & THREAD_ID_HASH_MASK)
        previous = seen.get(key)
        if previous is None:
            seen[key] = current
            continue
        assert previous[:2] == current[:2], (previous, current)
        assert (previous[2] & THREAD_ID_HASH_MASK) == (
            current[2] & THREAD_ID_HASH_MASK), (previous, current)
        assert previous[2] != current[2], (previous, current)
        return
    raise AssertionError(samples)


def main() -> int:
    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is None:
        print("_retrace_module=unavailable")
        return 0

    module = __import__(PROBE_MODULE)
    if not require_wrappers(module):
        return 0

    check_exclude_decorator(module)
    check_include_decorator(module)
    check_disable_enable_decorators(module)
    check_method_binding(module)
    check_exception_restores_state(module)
    check_with_new_coordinates(module)
    check_with_new_coordinates_restores_after_exception(module)
    check_with_new_coordinates_thread_ids(module)
    check_fresh_spaces_match(module)

    print("_retrace_module=builtin")
    print("coordinate_wrappers=available")
    print("coordinate_exclude=available")
    print("coordinate_include=available")
    print("coordinate_disable=available")
    print("coordinate_enable=available")
    print("coordinate_wrapper_methods=available")
    print("with_new_coordinates=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
