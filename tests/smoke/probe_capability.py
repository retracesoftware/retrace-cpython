import importlib.util
import json
import os
import subprocess
import sys
import threading
import time
import _thread


PROBE_MODULE = "_retrace"


def is_u64(value) -> bool:
    return type(value) is int and 0 < value < 2**64


def check_replay_checkpoint(module) -> None:
    hits = []

    def attempt(delta: int) -> None:
        base = tuple(module.coordinates())
        target = (*base[:-1], base[-1] + delta)

        def callback():
            hits.append(target)

        module.set_replay_checkpoint(_thread.get_ident(), target, callback)
        value = 0
        for index in range(1000):
            value += index
            value ^= index
            value += 1
        module.set_replay_checkpoint(None)

    for delta in range(1, 400):
        attempt(delta)
        if hits:
            break
    assert hits


def check_thread_callbacks(module) -> None:
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)

    yield_hits = []
    resume_hits = []
    callback_deltas = []
    stop = False

    def yield_callback():
        yield_hits.append(_thread.get_ident())
        callback_deltas.append(tuple(module.thread_delta()))

    def resume_callback():
        resume_hits.append(_thread.get_ident())
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
        assert tuple(stack[:-1]) == live[:-1]

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
        thread_id = _thread.get_ident()
        assert is_u64(thread_id)
        assert thread_id == _thread.get_ident()
        coordinates = module.coordinates()
        coordinates_by_id = module.coordinates(thread_id)
        assert type(coordinates) is tuple
        assert len(coordinates) >= 1
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
        assert module.get_thread_yield_callback() is None
        assert module.get_thread_resume_callback() is None

        def yield_callback():
            return None

        def resume_callback():
            return None

        module.set_thread_yield_callback(yield_callback)
        assert module.get_thread_yield_callback() is yield_callback
        module.set_thread_yield_callback(None)
        assert module.get_thread_yield_callback() is None
        module.set_thread_resume_callback(resume_callback)
        assert module.get_thread_resume_callback() is resume_callback
        module.set_thread_resume_callback(None)
        assert module.get_thread_resume_callback() is None
        check_thread_callbacks(module)
        check_replay_checkpoint(module)
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
        print("thread_yield_callback=available")
        print("thread_resume_callback=available")
        print("replay_checkpoint=available")
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
