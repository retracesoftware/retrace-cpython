import importlib.util
import inspect


PROBE_MODULE = "_retrace"


def require_wrappers(module):
    missing = [
        name for name in ("exclude", "include")
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


def check_run_transparent(module):
    def visible():
        before = module.coordinates()

        def hidden():
            return module.coordinates()

        inside = module.run_transparent(hidden)
        after = module.coordinates()
        return before, inside, after

    before, inside, after = visible()
    assert len(inside) == len(before) - 2
    assert inside == after


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
    check_method_binding(module)
    check_exception_restores_state(module)
    check_run_transparent(module)

    print("_retrace_module=builtin")
    print("coordinate_wrappers=available")
    print("coordinate_exclude=available")
    print("coordinate_include=available")
    print("coordinate_wrapper_methods=available")
    print("run_transparent=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
