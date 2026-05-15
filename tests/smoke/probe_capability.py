import importlib.util
import inspect
import json
import os
import subprocess
import sys
import threading
import time
import _thread


PROBE_MODULE = "_retrace"
THREAD_ID_SPACE_SHIFT = 48
THREAD_ID_HASH_MASK = (1 << THREAD_ID_SPACE_SHIFT) - 1
THREAD_ID_SPACE_MASK = (1 << 16) - 1


def is_u64(value) -> bool:
    return type(value) is int and 0 < value < 2**64


def thread_id_space(thread_id: int) -> int:
    return thread_id >> THREAD_ID_SPACE_SHIFT


def thread_id_hash(thread_id: int) -> int:
    return thread_id & THREAD_ID_HASH_MASK


def child_thread_id() -> int:
    ids = []
    ready = threading.Event()

    def worker() -> None:
        ids.append(_thread.get_ident())
        ready.set()

    started_id = _thread.start_new_thread(worker, ())
    assert ready.wait(5)
    assert ids == [started_id]
    return ids[0]


def advance_leaf_instruction(coordinates, delta: int):
    return (*coordinates[:-2], coordinates[-2] + delta, 0)


def check_call_at(module) -> None:
    hits = []
    errors = []

    def attempt(delta: int) -> bool:
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

        base = tuple(module.coordinates(ident))
        target = advance_leaf_instruction(base, delta)

        def callback():
            hits.append(target)

        try:
            module.call_at(ident, target, callback)
        except ValueError as exc:
            assert str(exc) == "call_at coordinates are in the past"
            go.set()
            thread.join(5)
            assert not thread.is_alive()
            return False
        try:
            go.set()
            thread.join(5)
            assert not thread.is_alive()
        finally:
            module.call_at(None)
        return True

    for delta in range(1, 1000):
        attempt(delta)
        if hits:
            break
    assert not errors, errors
    assert hits


def check_call_at_past_rejected(module) -> None:
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


def check_call_at_overshoot(module) -> None:
    hits = []
    overshoots = []

    def run_body() -> None:
        value = 0
        for index in range(1000):
            value += index
            value ^= index
            value += 1

    def attempt(delta: int) -> bool:
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
        run_body()
        module.call_at(None)
        assert not (hits and overshoots)
        return bool(overshoots)

    for delta in range(1, 1000):
        if attempt(delta):
            break
    assert overshoots


def check_callback_coordinate_transparency(module) -> None:
    hits = []
    generated = []
    errors = []

    def callback() -> None:
        pinned_coordinates = tuple(module.coordinates())
        pinned_hash = module.hash()
        hits.append(pinned_coordinates)

        def helper(depth: int) -> None:
            assert tuple(module.coordinates()) == pinned_coordinates
            assert module.hash() == pinned_hash
            if depth:
                helper(depth - 1)

        helper(4)

        delta = tuple(module.thread_delta())
        common_count = delta[0]
        assert 0 <= common_count <= len(pinned_coordinates)
        assert delta[1:] == pinned_coordinates[common_count:]

        def transparent_generator():
            assert tuple(module.coordinates()) == pinned_coordinates
            assert module.hash() == pinned_hash
            yield tuple(module.coordinates()), module.hash()
            yield tuple(module.coordinates()), module.hash()

        generator = transparent_generator()
        assert next(generator) == (pinned_coordinates, pinned_hash)
        generated.append(generator)

    def attempt(delta: int) -> None:
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

        base = tuple(module.coordinates(ident))
        target = advance_leaf_instruction(base, delta)
        try:
            module.call_at(ident, target, callback)
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
            module.call_at(None)

    for delta in range(1, 1000):
        attempt(delta)
        if hits:
            break

    assert not errors, errors
    assert hits
    assert generated

    outside_coordinates = tuple(module.coordinates())
    resumed_coordinates, resumed_hash = next(generated[0])
    assert type(resumed_hash) is int
    assert type(resumed_coordinates) is tuple
    assert len(resumed_coordinates) % 2 == 0
    assert resumed_coordinates != hits[0]


def check_call_at_include(module) -> None:
    order = []
    control_coordinates = []
    visible_coordinates = []
    nested_coordinates = []
    errors = []

    def visible() -> None:
        order.append("visible")
        current = tuple(module.coordinates())
        visible_coordinates.append(current)
        pinned = control_coordinates[0]
        assert current[: len(pinned)] == pinned
        assert len(current) > len(pinned)

        def nested() -> None:
            nested_coordinates.append(tuple(module.coordinates()))

        nested()
        assert nested_coordinates[-1][: len(pinned)] == pinned
        assert len(nested_coordinates[-1]) > len(pinned)
        assert nested_coordinates[-1] != current

    included_visible = module.include(visible)

    def callback():
        order.append("control-before")
        current = tuple(module.coordinates())
        control_coordinates.append(current)
        included_visible()
        order.append("control-after")
        control_coordinates.append(tuple(module.coordinates()))
        return None

    def attempt(delta: int) -> None:
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

        base = tuple(module.coordinates(ident))
        target = advance_leaf_instruction(base, delta)
        try:
            module.call_at(ident, target, callback)
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
            module.call_at(None)

    for delta in range(1, 1000):
        attempt(delta)
        if visible_coordinates:
            break

    assert not errors, errors
    assert order == ["control-before", "visible", "control-after"]
    assert control_coordinates[0][:-1] == control_coordinates[1][:-1]
    assert control_coordinates[1][-1] >= control_coordinates[0][-1]
    assert visible_coordinates
    assert nested_coordinates


def check_thread_callbacks(module) -> None:
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)

    yield_hits = []
    resume_hits = []
    callback_deltas = []
    callback_failures = []
    stop = False

    def check_callback_frame_pinned(kind: str) -> None:
        try:
            coordinates = tuple(module.coordinates())
            coordinate_hash = module.hash()

            def helper():
                return tuple(module.coordinates()), module.hash()

            helper_coordinates, helper_hash = helper()
            assert helper_coordinates == coordinates, (
                kind,
                coordinates,
                helper_coordinates,
            )
            assert helper_hash == coordinate_hash, (
                kind,
                coordinate_hash,
                helper_hash,
            )
        except BaseException as exc:
            callback_failures.append((kind, repr(exc)))

    def yield_callback():
        yield_hits.append(_thread.get_ident())
        check_callback_frame_pinned("yield")
        callback_deltas.append(tuple(module.thread_delta()))

    def resume_callback():
        resume_hits.append(_thread.get_ident())
        check_callback_frame_pinned("resume")
        callback_deltas.append(tuple(module.thread_delta()))

    def contender():
        value = 0
        while not stop:
            value += 1

    workers = [threading.Thread(target=contender) for _ in range(2)]
    started = []

    try:
        module.set_thread_yield_callback(yield_callback)
        module.set_thread_resume_callback(resume_callback)
        for worker in workers:
            worker.start()
            started.append(worker)

        deadline = time.monotonic() + 2.0
        while (not yield_hits or not resume_hits) and time.monotonic() < deadline:
            for index in range(10_000):
                _ = index ^ len(yield_hits) ^ len(resume_hits)
            time.sleep(0)
    finally:
        stop = True
        for worker in started:
            worker.join()
        module.set_thread_yield_callback(None)
        module.set_thread_resume_callback(None)
        sys.setswitchinterval(old_interval)

    assert yield_hits
    assert resume_hits
    assert not callback_failures, callback_failures
    assert callback_deltas
    assert all(delta and delta[0] >= 0 for delta in callback_deltas)


def apply_coordinate_delta(stack: list[int], delta) -> None:
    common_count = delta[0]
    del stack[common_count:]
    stack.extend(delta[1:])


def check_thread_delta(module) -> None:
    stack = []

    def sync() -> None:
        delta = module.thread_delta()
        assert type(delta) is tuple
        assert len(delta) >= 1
        assert 0 <= delta[0] <= len(stack)
        apply_coordinate_delta(stack, delta)
        live = tuple(module.coordinates())
        assert len(stack) == len(live)
        assert len(stack) % 2 == 0
        assert all(type(item) is int and 0 <= item < 2**64
                   for item in stack)

    sync()

    def nested() -> None:
        sync()

    nested()
    sync()


def check_coordinate_hash(module) -> None:
    value = module.hash()
    assert type(value) is int
    assert 0 <= value < 2**64

    def nested() -> int:
        return module.hash()

    nested_hashes = [nested() for _ in range(3)]
    assert all(type(item) is int for item in nested_hashes)
    assert all(0 <= item < 2**64 for item in nested_hashes)
    assert len(set(nested_hashes)) == len(nested_hashes)


def check_thread_ids(module) -> None:
    main_id = _thread.get_ident()
    assert is_u64(main_id)
    assert main_id == _thread.get_ident()
    assert thread_id_space(main_id) == 0
    assert thread_id_hash(main_id) != 0

    worker_ids = []

    def worker() -> None:
        worker_ids.append(_thread.get_ident())

    workers = [threading.Thread(target=worker) for _ in range(3)]
    for worker_thread in workers:
        worker_thread.start()
    for worker_thread in workers:
        worker_thread.join()

    assert len(worker_ids) == len(workers)
    assert all(is_u64(item) for item in worker_ids)
    assert len(set(worker_ids)) == len(workers)
    assert main_id not in set(worker_ids)
    assert all(thread_id_space(item) == 0 for item in worker_ids)
    assert all(thread_id_hash(item) != 0 for item in worker_ids)

    disabled_child_id = module.run_in_space(1, child_thread_id)
    assert is_u64(disabled_child_id)
    assert thread_id_space(disabled_child_id) == 1
    assert thread_id_hash(disabled_child_id) != 0

    space_id = module.allocate_space_id()
    space_child_id = module.run_in_space(space_id, child_thread_id)
    assert is_u64(space_child_id)
    assert thread_id_space(space_child_id) == (
        space_id & THREAD_ID_SPACE_MASK)
    assert thread_id_hash(space_child_id) != 0
    assert len({main_id, disabled_child_id, space_child_id}) == 3


def check_ctypes_async_exc_bridge(module) -> None:
    import ctypes

    class RetraceStop(Exception):
        pass

    ready = threading.Event()
    done = threading.Event()
    child = {}
    stop = False

    def worker() -> None:
        nonlocal stop
        child["ident"] = _thread.get_ident()
        ready.set()
        try:
            while not stop:
                time.sleep(0.01)
        except RetraceStop:
            child["stopped"] = True
        finally:
            done.set()

    _thread.start_new_thread(worker, ())
    try:
        assert ready.wait(5)
        set_async_exc = ctypes.pythonapi.PyThreadState_SetAsyncExc
        set_async_exc.argtypes = (ctypes.c_ulong, ctypes.py_object)
        set_async_exc.restype = ctypes.c_int
        result = set_async_exc(
            ctypes.c_ulong(child["ident"]),
            ctypes.py_object(RetraceStop),
        )
        assert result == 1
        assert done.wait(5)
        assert child.get("stopped") is True
    finally:
        stop = True
        done.wait(5)


def thread_id_with_root_seed(seed: str | None) -> int:
    env = os.environ.copy()
    if seed is None:
        env.pop("RETRACE_ROOT_SEED", None)
    else:
        env["RETRACE_ROOT_SEED"] = seed

    script = "import json, _thread; print(json.dumps(_thread.get_ident()))"
    output = subprocess.run(
        [sys.executable, "-c", script],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    ).stdout
    return json.loads(output)


def check_root_seed_environment(module) -> None:
    default_id = thread_id_with_root_seed(None)
    explicit_default_id = thread_id_with_root_seed("retrace")
    seed_a_id = thread_id_with_root_seed("retrace-smoke-a")
    seed_a_repeat_id = thread_id_with_root_seed("retrace-smoke-a")
    seed_b_id = thread_id_with_root_seed("retrace-smoke-b")

    assert default_id == explicit_default_id
    assert seed_a_id == seed_a_repeat_id
    assert all(is_u64(item) for item in (
        default_id, seed_a_id, seed_b_id
    ))
    assert seed_a_id != seed_b_id
    assert seed_a_id != default_id


def check_c_driven_callback_coordinates(module) -> None:
    def mapped(value: int) -> int:
        map_coordinates.append(tuple(module.coordinates()))
        return value

    map_coordinates = []
    list(map(mapped, range(3)))
    assert len(set(map_coordinates)) == len(map_coordinates)

    def key(value: int) -> int:
        key_coordinates.append(tuple(module.coordinates()))
        return value

    key_coordinates = []
    sorted([2, 1, 3], key=key)
    assert len(set(key_coordinates)) == len(key_coordinates)


def check_no_disable_api(module) -> None:
    assert not hasattr(module, "disable_for")
    assert not hasattr(module, "run_disabled")
    assert not hasattr(module, "run_transparent")
    assert not hasattr(module, "enable_for")
    assert not hasattr(module, "thread_id")
    assert not hasattr(module, "thread_id_from_ident")
    assert not hasattr(module, "common_coordinates_prefix_length")
    assert not hasattr(module, "U64Buffer")

    try:
        module.coordinates((0,))
    except TypeError:
        pass
    else:
        raise AssertionError("sequence thread id was accepted")
    try:
        module.coordinates(0)
    except LookupError:
        pass
    else:
        raise AssertionError("integer thread id was accepted")


def check_deterministic_identity_hashes() -> None:
    script = r"""
import json

def snapshot():
    functions = []
    for index in range(12):
        def item(index=index):
            return index
        functions.append(item)
    objects = [object() for _ in range(12)]
    nans = [float("nan") for _ in range(12)]

    return {
        "function_hashes": [hash(item) for item in functions],
        "object_hashes": [hash(item) for item in objects],
        "nan_hashes": [hash(item) for item in nans],
        "function_set": [item() for item in set(functions)],
        "object_set_len": len(set(objects)),
        "nan_set_len": len(set(nans)),
    }

print(json.dumps(snapshot(), sort_keys=True))
"""

    first = subprocess.run(
        [sys.executable, "-c", script],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    second = subprocess.run(
        [sys.executable, "-c", script],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    first_snapshot = json.loads(first)
    second_snapshot = json.loads(second)
    assert first_snapshot == second_snapshot
    assert len(set(first_snapshot["function_hashes"])) == 12
    assert len(set(first_snapshot["object_hashes"])) == 12
    assert len(set(first_snapshot["nan_hashes"])) == 12
    assert first_snapshot["object_set_len"] == 12
    assert first_snapshot["nan_set_len"] == 12


def main() -> int:
    print(f"python={sys.version.split()[0]}")
    print(f"executable={sys.executable}")

    spec = importlib.util.find_spec(PROBE_MODULE)
    if spec is not None:
        assert PROBE_MODULE in sys.builtin_module_names
        assert "retrace" not in sys.builtin_module_names
        module = __import__(PROBE_MODULE)
        assert hasattr(module, "call_at")
        assert not hasattr(module, "set_replay_checkpoint")
        thread_id = _thread.get_ident()
        assert is_u64(thread_id)
        assert thread_id == _thread.get_ident()
        coordinates = module.coordinates()
        coordinates_by_id = module.coordinates(thread_id)
        assert type(coordinates) is tuple
        assert len(coordinates) >= 1
        assert len(coordinates) % 2 == 0
        assert tuple(coordinates[0:]) == tuple(coordinates)
        assert len(coordinates_by_id) == len(coordinates)
        dropped = module.coordinates(None, 1)
        dropped_by_id = module.coordinates(thread_id, 1)
        dropped_all = module.coordinates(None, len(coordinates) + 1)
        assert len(dropped) == max(0, len(coordinates) - 1)
        assert len(dropped_by_id) == max(0, len(coordinates) - 1)
        assert len(module.coordinates(None, len(coordinates) - 1)) == 1
        assert tuple(dropped_all) == ()
        try:
            module.coordinates(None, -1)
        except ValueError:
            pass
        else:
            raise AssertionError("negative coordinates drop accepted")
        check_thread_delta(module)
        check_coordinate_hash(module)
        check_thread_ids(module)
        check_ctypes_async_exc_bridge(module)
        check_root_seed_environment(module)
        assert module.get_thread_start_callback() is None
        assert module.get_thread_yield_callback() is None
        assert module.get_thread_resume_callback() is None

        def start_callback():
            return None

        def yield_callback():
            return None

        def resume_callback():
            return None

        module.set_thread_start_callback(start_callback)
        assert inspect.unwrap(module.get_thread_start_callback()) is start_callback
        module.set_thread_start_callback(None)
        assert module.get_thread_start_callback() is None
        module.set_thread_yield_callback(yield_callback)
        assert inspect.unwrap(module.get_thread_yield_callback()) is yield_callback
        module.set_thread_yield_callback(None)
        assert module.get_thread_yield_callback() is None
        module.set_thread_resume_callback(resume_callback)
        assert inspect.unwrap(module.get_thread_resume_callback()) is resume_callback
        module.set_thread_resume_callback(None)
        assert module.get_thread_resume_callback() is None
        check_thread_callbacks(module)
        check_call_at(module)
        check_call_at_past_rejected(module)
        check_call_at_overshoot(module)
        check_callback_coordinate_transparency(module)
        check_call_at_include(module)
        check_c_driven_callback_coordinates(module)
        check_no_disable_api(module)
        check_deterministic_identity_hashes()

        print("_retrace_module=builtin")
        print(f"coordinates_type={type(coordinates).__name__}")
        print(f"coordinates_len={len(coordinates)}")
        print("thread_delta=available")
        print("hash=available")
        print("ctypes_async_exc_bridge=available")
        print("root_seed_environment=available")
        print("disable_api=unavailable")
        print("deterministic_identity_hashes=available")
        print("thread_start_callback=available")
        print("thread_yield_callback=available")
        print("thread_resume_callback=available")
        print("call_at=available")
        print("call_at_overshoot=available")
        print("callback_coordinate_transparency=available")
        print("call_at_include=available")
    else:
        print("_retrace_module=unavailable")

    spec = importlib.util.find_spec("_retrace_probe")
    if spec is None:
        print("retrace_probe=unavailable")
        return 0

    module = __import__("_retrace_probe")
    abi_version = getattr(module, "abi_version", None)
    features = getattr(module, "features", None)
    print("retrace_probe=available")
    print(f"abi_version={abi_version}")
    print(f"features={features}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
