import importlib.util
import inspect
import _thread
import threading


PUBLIC_MODULE = "retrace"
NATIVE_MODULE = "_retrace"


def advance_leaf_instruction(coordinates, delta):
    return (*coordinates[:-2], coordinates[-2] + delta, 0)


def require_modules():
    if importlib.util.find_spec(PUBLIC_MODULE) is None:
        print("retrace_module=unavailable")
        return None, None
    if importlib.util.find_spec(NATIVE_MODULE) is None:
        print("_retrace_module=unavailable")
        return None, None
    return __import__(PUBLIC_MODULE), __import__(NATIVE_MODULE)


def check_callback_registration(public, native):
    def switch_callback(previous_delta, next_thread_id):
        assert type(previous_delta) is tuple
        assert type(next_thread_id) is int
        return None

    try:
        public.callbacks.thread_switch = switch_callback
        stored_switch = native.get_thread_switch_callback()
        assert inspect.unwrap(stored_switch) is switch_callback
        assert public.callbacks.thread_switch is stored_switch
        public.callbacks.thread_switch = None
        assert public.callbacks.thread_switch is None

        space = public.CoordinateSpace()
        public.callbacks.set_thread_switch(switch_callback, space)
        stored_switch = native.get_thread_switch_callback(space.id)
        assert inspect.unwrap(stored_switch) is switch_callback
        public.callbacks.set_thread_switch(None, space)
        assert native.get_thread_switch_callback(space.id) is None
    finally:
        public.callbacks.thread_switch = None


def check_call_at_include(public):
    order = []
    control_coordinates = []
    visible_coordinates = []
    errors = []

    def visible():
        order.append("visible")
        current = tuple(public.coordinates())
        visible_coordinates.append(current)
        pinned = control_coordinates[0]
        assert current[: len(pinned)] == pinned
        assert len(current) > len(pinned)

    visible_from_call_at = public.include(visible)

    def callback():
        order.append("control-before")
        control_coordinates.append(tuple(public.coordinates()))
        visible_from_call_at()
        order.append("control-after")
        control_coordinates.append(tuple(public.coordinates()))

    def attempt(delta):
        ready = threading.Event()
        go = threading.Event()
        ident = None

        def worker():
            nonlocal ident
            try:
                ident = _thread.get_ident()
                ready.set()
                assert go.wait(5)
                value = 0
                for index in range(1000):
                    value += index
                    value ^= index
                    value += 1
            except BaseException as exc:
                errors.append(exc)

        thread = threading.Thread(target=worker)
        thread.start()
        assert ready.wait(5)

        base = tuple(public.coordinates(ident))
        target = advance_leaf_instruction(base, delta)
        try:
            public.call_at(ident, target, callback)
        except ValueError as exc:
            assert str(exc) == "call_at coordinates are in the past"
            go.set()
            thread.join(5)
            assert not thread.is_alive()
            return
        try:
            go.set()
            thread.join(5)
            assert not thread.is_alive()
        finally:
            public.call_at(None)

    try:
        for delta in range(1, 1000):
            attempt(delta)
            if visible_coordinates:
                break
    finally:
        public.call_at(None)

    assert not errors, errors
    assert order == ["control-before", "visible", "control-after"]
    assert control_coordinates[0][:-1] == control_coordinates[1][:-1]
    assert control_coordinates[1][-1] >= control_coordinates[0][-1]
    assert visible_coordinates


def check_call_at_overshoot(public):
    hits = []
    overshoots = []

    def callback():
        hits.append(tuple(public.coordinates()))

    def overshoot():
        overshoots.append(tuple(public.coordinates()))

    def attempt(delta):
        hits.clear()
        overshoots.clear()
        base = tuple(public.coordinates())
        target = advance_leaf_instruction(base, delta)
        try:
            public.call_at(
                _thread.get_ident(), target, callback, overshoot)
        except ValueError as exc:
            assert str(exc) == "call_at coordinates are in the past"
            return
        value = 0
        for index in range(1000):
            value += index
            value ^= index
            value += 1
        public.call_at(None)
        assert not (hits and overshoots)
        return bool(overshoots)

    try:
        for delta in range(1, 1000):
            if attempt(delta):
                break
    finally:
        public.call_at(None)

    assert overshoots


def check_coordinate_spaces(public):
    space = public.CoordinateSpace()
    assert space.coordinates() is None

    def leaf():
        return space.coordinates(), public.coordinates()

    space_coordinates, root_coordinates = space.run(leaf)
    assert type(space_coordinates) is tuple
    assert len(space_coordinates) >= 2
    assert len(space_coordinates) % 2 == 0
    assert type(root_coordinates) is tuple
    assert len(root_coordinates) >= 2
    assert space.coordinates() == ()

    assert public.disabled_space.coordinates() is None

    calls = []

    def disabled_leaf():
        calls.append("disabled_leaf")
        return public.disabled_space.coordinates(), public.coordinates()

    disabled_wrapped = public.disable(disabled_leaf)
    assert inspect.unwrap(disabled_wrapped) is disabled_leaf
    assert calls == []
    disabled_coordinates, root_from_disabled = disabled_wrapped()
    assert calls == ["disabled_leaf"]
    assert type(disabled_coordinates) is tuple
    assert len(disabled_coordinates) >= 2
    assert len(disabled_coordinates) % 2 == 0
    assert type(root_from_disabled) is tuple
    assert len(root_from_disabled) >= 2

    @public.disable
    def decorated_disabled_leaf():
        return public.disabled_space.coordinates(), public.coordinates()

    decorated_disabled, decorated_root = decorated_disabled_leaf()
    assert type(decorated_disabled) is tuple
    assert len(decorated_disabled) >= 2
    assert len(decorated_disabled) % 2 == 0
    assert type(decorated_root) is tuple
    assert len(decorated_root) >= 2

    def immediate_disabled_leaf():
        calls.append("immediate_disabled_leaf")
        return public.disabled_space.coordinates(), public.coordinates()

    immediate_disabled, immediate_root = public.run_disabled(
        immediate_disabled_leaf)
    assert calls[-1:] == ["immediate_disabled_leaf"]
    assert type(immediate_disabled) is tuple
    assert len(immediate_disabled) >= 2
    assert len(immediate_disabled) % 2 == 0
    assert type(immediate_root) is tuple
    assert len(immediate_root) >= 2

    @public.enable
    def enabled_leaf():
        return public.coordinates()

    @public.disable
    def disabled_parent():
        disabled_before = public.coordinates()
        enabled_coordinates = enabled_leaf()
        disabled_after = public.coordinates()
        return disabled_before, enabled_coordinates, disabled_after

    before = public.coordinates()
    disabled_before, enabled_coordinates, disabled_after = disabled_parent()
    after = public.coordinates()
    assert type(disabled_before) is tuple
    assert len(disabled_before) >= 2
    assert len(enabled_coordinates) == len(before) + 2
    assert len(disabled_after) == len(disabled_before)
    assert len(after) == len(before)


def check_space_wrappers(public, native):
    space = public.CoordinateSpace()
    calls = []

    def leaf(left, right=0):
        space_coordinates = space.coordinates()
        root_coordinates = public.coordinates()
        assert type(space_coordinates) is tuple
        assert len(space_coordinates) >= 2
        assert len(space_coordinates) % 2 == 0
        assert type(root_coordinates) is tuple
        assert len(root_coordinates) >= 2
        calls.append((space_coordinates, root_coordinates))
        return left + right

    wrapped = space.wrap(leaf)
    assert inspect.unwrap(wrapped) is leaf
    assert wrapped(2, right=3) == 5
    assert space.coordinates() == ()

    native_wrapped = native.wrap_for_space(space.id, leaf)
    assert inspect.unwrap(native_wrapped) is leaf
    assert native_wrapped(4, right=5) == 9

    class Holder:
        @space.wrap
        def method(self, value):
            return self, value, space.coordinates()

    holder = Holder()
    owner, value, method_coordinates = holder.method(11)
    assert owner is holder
    assert value == 11
    assert type(method_coordinates) is tuple
    assert len(method_coordinates) >= 2
    assert len(calls) == 2

    try:
        native.wrap_for_space(space.id, object())
    except TypeError:
        pass
    else:
        raise AssertionError("wrap_for_space accepted a non-callable")


def main() -> int:
    public, native = require_modules()
    if public is None:
        return 0

    assert public.root_space.id is None
    assert public.disabled_space.id == 1
    assert public.coordinates.__self__ is public.root_space
    assert public.thread_delta.__self__ is public.root_space
    assert public.hash.__self__ is public.root_space
    assert public.exclude.__self__ is public.disabled_space
    assert public.include.__self__ is public.root_space
    assert public.disable.__self__ is public.disabled_space
    assert public.enable.__self__ is public.root_space
    assert public.run_disabled.__self__ is public.disabled_space
    assert public.call_at is not getattr(native, "call_at")
    assert public.ThreadHandoff is native.ThreadHandoff
    assert hasattr(native, "disable")
    assert hasattr(native, "enable")
    assert not hasattr(native, "run_disabled")
    assert not hasattr(native, "run_transparent")
    assert "call_at" in public.__all__
    assert "callbacks" in public.__all__
    assert "CoordinateSpace" in public.__all__
    assert "disable" in public.__all__
    assert "enable" in public.__all__
    assert "run_disabled" in public.__all__
    assert "run_transparent" not in public.__all__
    assert "with_new_coordinates" in public.__all__
    assert not hasattr(public, "run_transparent")
    assert not hasattr(public, "set_replay_checkpoint")
    assert not hasattr(public, "set_thread_start_callback")
    assert not hasattr(public, "get_thread_start_callback")
    assert not hasattr(public, "set_thread_yield_callback")
    assert not hasattr(public, "get_thread_yield_callback")
    assert not hasattr(public, "set_thread_resume_callback")
    assert not hasattr(public, "get_thread_resume_callback")
    assert not hasattr(public.callbacks, "set_thread_start")
    assert not hasattr(public.callbacks, "set_thread_yield")
    assert not hasattr(public.callbacks, "set_thread_resume")
    assert not hasattr(public.callbacks, "thread_start")
    assert not hasattr(public.callbacks, "thread_yield")
    assert not hasattr(public.callbacks, "thread_resume")

    check_callback_registration(public, native)
    check_coordinate_spaces(public)
    check_space_wrappers(public, native)
    check_call_at_include(public)
    check_call_at_overshoot(public)

    print("retrace_module=available")
    print("retrace_callback_wrapping=available")
    print("space_wrapping=available")
    print("call_at_include=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
