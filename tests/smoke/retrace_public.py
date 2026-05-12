import importlib.util
import inspect
import _thread


PUBLIC_MODULE = "retrace"
NATIVE_MODULE = "_retrace"


def require_modules():
    if importlib.util.find_spec(PUBLIC_MODULE) is None:
        print("retrace_module=unavailable")
        return None, None
    if importlib.util.find_spec(NATIVE_MODULE) is None:
        print("_retrace_module=unavailable")
        return None, None
    return __import__(PUBLIC_MODULE), __import__(NATIVE_MODULE)


def check_callback_registration(public, native):
    def start_callback():
        return None

    def yield_callback():
        return None

    def resume_callback():
        return None

    try:
        public.callbacks.thread_start = start_callback
        stored_start = native.get_thread_start_callback()
        assert inspect.unwrap(stored_start) is start_callback
        assert public.callbacks.thread_start is stored_start
        public.callbacks.thread_start = None
        assert public.callbacks.thread_start is None

        public.callbacks.thread_yield = yield_callback
        stored_yield = native.get_thread_yield_callback()
        assert inspect.unwrap(stored_yield) is yield_callback
        assert public.callbacks.thread_yield is stored_yield
        public.callbacks.thread_yield = None
        assert public.callbacks.thread_yield is None

        public.callbacks.thread_resume = resume_callback
        stored_resume = native.get_thread_resume_callback()
        assert inspect.unwrap(stored_resume) is resume_callback
        assert public.callbacks.thread_resume is stored_resume
        public.callbacks.thread_resume = None
        assert public.callbacks.thread_resume is None
    finally:
        public.callbacks.thread_start = None
        public.callbacks.thread_yield = None
        public.callbacks.thread_resume = None


def check_call_at_include(public):
    order = []
    control_coordinates = []
    visible_coordinates = []

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
        base = tuple(public.coordinates())
        target = (*base[:-1], base[-1] + delta)
        try:
            public.call_at(_thread.get_ident(), target, callback)
        except ValueError as exc:
            assert str(exc) == "call_at coordinates are in the past"
            return
        value = 0
        for index in range(1000):
            value += index
            value ^= index
            value += 1
        public.call_at(None)

    try:
        for delta in range(1, 1000):
            attempt(delta)
            if visible_coordinates:
                break
    finally:
        public.call_at(None)

    assert order == ["control-before", "visible", "control-after"]
    assert control_coordinates[0] == control_coordinates[1]
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
        target = (*base[:-1], base[-1] + delta)
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


def main() -> int:
    public, native = require_modules()
    if public is None:
        return 0

    assert public.coordinates is native.coordinates
    assert public.thread_delta is native.thread_delta
    assert public.hash is native.hash
    assert public.exclude is native.exclude
    assert public.include is native.include
    assert public.call_at is not getattr(native, "call_at")
    assert public.ThreadHandoff is native.ThreadHandoff
    assert "call_at" in public.__all__
    assert "callbacks" in public.__all__
    assert not hasattr(public, "set_replay_checkpoint")
    assert not hasattr(public, "set_thread_start_callback")
    assert not hasattr(public, "get_thread_start_callback")
    assert not hasattr(public, "set_thread_yield_callback")
    assert not hasattr(public, "get_thread_yield_callback")
    assert not hasattr(public, "set_thread_resume_callback")
    assert not hasattr(public, "get_thread_resume_callback")

    check_callback_registration(public, native)
    check_call_at_include(public)
    check_call_at_overshoot(public)

    print("retrace_module=available")
    print("retrace_callback_wrapping=available")
    print("call_at_include=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
