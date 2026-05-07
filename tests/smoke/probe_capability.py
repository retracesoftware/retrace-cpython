import importlib.util
import json
import subprocess
import sys
import threading
import time


PROBE_MODULE = "_retrace"


def check_replay_checkpoint(module) -> None:
    hits = []

    def attempt(delta: int) -> None:
        base = tuple(module.coordinates())
        target = (base[0] + delta, *base[1:])

        def callback():
            hits.append((tuple(module.coordinates()), target))

        module.set_replay_checkpoint(threading.get_ident(), target, callback)
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
    assert hits[0][0] == hits[0][1]


def check_thread_callbacks(module) -> None:
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)

    yield_hits = []
    resume_hits = []
    callback_deltas = []
    stop = False

    def yield_callback():
        yield_hits.append(threading.get_ident())
        callback_deltas.append(tuple(module.coordinates_delta()))

    def resume_callback():
        resume_hits.append(threading.get_ident())
        callback_deltas.append(tuple(module.coordinates_delta()))

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
    drop_count = len(stack) - common_count
    del stack[:drop_count]
    stack[:0] = delta[1:]


def check_coordinates_delta(module) -> None:
    stack = []

    def sync() -> None:
        delta = module.coordinates_delta()
        assert isinstance(delta, module.U64Buffer)
        assert len(delta) >= 1
        assert 0 <= delta[0] <= len(stack)
        apply_coordinate_delta(stack, delta)
        live = tuple(module.coordinates())
        assert len(stack) == len(live)
        assert tuple(stack[1:]) == live[1:]

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


def check_disable_for(module) -> None:
    def hidden(value: int) -> int:
        hidden_coordinates.append(tuple(module.coordinates()))

        def child() -> None:
            child_coordinates.append(tuple(module.coordinates()))

        child()
        return value

    wrapped = module.disable_for(hidden)
    assert wrapped is module.disable_for(wrapped)
    assert wrapped.__wrapped__ is hidden
    assert wrapped.__name__ == hidden.__name__

    hidden_coordinates = []
    child_coordinates = []
    list(map(wrapped, range(3)))
    assert len(set(hidden_coordinates)) == 1
    assert len(set(child_coordinates)) == 1
    assert hidden_coordinates == child_coordinates

    class Holder:
        def __init__(self) -> None:
            self.calls = 0

        @module.disable_for
        def method(self) -> int:
            self.calls += 1
            return len(module.coordinates())

    holder = Holder()
    visible_depth = len(module.coordinates())
    assert holder.method() == visible_depth
    assert holder.calls == 1

    generator_coordinates = []

    def generator():
        generator_coordinates.append(tuple(module.coordinates()))
        yield 1
        generator_coordinates.append(tuple(module.coordinates()))

    disabled_generator = module.disable_for(generator)()
    generator_depth = len(module.coordinates())
    assert next(disabled_generator) == 1
    try:
        next(disabled_generator)
    except StopIteration:
        pass
    else:
        raise AssertionError("disabled generator did not stop")
    assert all(len(item) == generator_depth for item in generator_coordinates)


def check_enable_for(module) -> None:
    collapsed_coordinates = []

    def collapsed(value: int) -> int:
        collapsed_coordinates.append(tuple(module.coordinates()))
        return value

    list(map(module.disable_for(collapsed), range(3)))
    assert len(set(collapsed_coordinates)) == 1

    def hidden(value: int) -> int:
        hidden_coordinates.append(tuple(module.coordinates()))

        def visible() -> None:
            enabled_coordinates.append(tuple(module.coordinates()))

            def hidden_again() -> None:
                hidden_again_coordinates.append(tuple(module.coordinates()))

            module.disable_for(hidden_again)()

        module.enable_for(visible)()
        return value

    wrapped_hidden = module.disable_for(hidden)
    assert wrapped_hidden is module.disable_for(wrapped_hidden)

    def plain(value: int) -> int:
        return value

    wrapped_enabled = module.enable_for(plain)
    assert wrapped_enabled is module.enable_for(wrapped_enabled)
    assert wrapped_enabled.__wrapped__ is plain
    assert wrapped_enabled.__name__ == plain.__name__

    hidden_coordinates = []
    enabled_coordinates = []
    hidden_again_coordinates = []
    list(map(wrapped_hidden, range(3)))

    assert len(set(enabled_coordinates)) == len(enabled_coordinates)
    assert all(
        len(enabled) == len(hidden) + 1
        for hidden, enabled in zip(hidden_coordinates, enabled_coordinates)
    )
    assert len(hidden_again_coordinates) == len(enabled_coordinates)
    assert all(
        len(hidden_again) == len(enabled)
        for hidden_again, enabled in zip(hidden_again_coordinates, enabled_coordinates)
    )
    assert all(
        hidden_again[1:] == enabled[1:]
        for hidden_again, enabled in zip(hidden_again_coordinates, enabled_coordinates)
    )


def check_retrace_coordinates_xoption() -> None:
    script = r"""
import json
import sys
import _retrace as retrace

visible_depths = []

def visible():
    visible_depths.append(len(retrace.coordinates()))

    def child():
        visible_depths.append(len(retrace.coordinates()))

    child()

baseline = len(retrace.coordinates())
retrace.enable_for(visible)()
after = len(retrace.coordinates())
print(json.dumps({
    "xoption": sys._xoptions.get("retrace_coordinates"),
    "baseline": baseline,
    "visible_depths": visible_depths,
    "after": after,
}))
"""

    def run(*args: str) -> dict:
        result = subprocess.run(
            [sys.executable, *args, "-c", script],
            check=True,
            text=True,
            capture_output=True,
        )
        return json.loads(result.stdout)

    default = run()
    assert default["xoption"] is None
    assert default["baseline"] >= 2
    assert default["after"] >= 2

    enabled = run("-X", "retrace_coordinates=enabled")
    assert enabled["xoption"] == "enabled"
    assert enabled["baseline"] >= 2
    assert enabled["after"] >= 2

    disabled = run("-X", "retrace_coordinates=disabled")
    assert disabled["xoption"] == "disabled"
    assert disabled["baseline"] == 1
    assert disabled["after"] == 1
    assert disabled["visible_depths"] == [2, 3]


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
        coordinates = module.coordinates()
        coordinates_by_id = module.coordinates(threading.get_ident())
        view = memoryview(coordinates)
        assert len(coordinates) >= 1
        assert view.format == "Q"
        assert view.readonly
        assert tuple(coordinates[0:]) == tuple(coordinates)
        assert coordinates == tuple(coordinates)
        assert len(coordinates_by_id) == len(coordinates)
        dropped = module.coordinates(None, 1)
        dropped_by_id = module.coordinates(threading.get_ident(), 1)
        dropped_all = module.coordinates(None, len(coordinates) + 1)
        assert len(dropped) == max(0, len(coordinates) - 1)
        assert len(dropped_by_id) == max(0, len(coordinates) - 1)
        assert tuple(module.coordinates(None, len(coordinates) - 1)) == (
            coordinates[-1],
        )
        assert tuple(dropped_all) == ()
        try:
            module.coordinates(None, -1)
        except ValueError:
            pass
        else:
            raise AssertionError("negative coordinates drop accepted")
        assert module.common_coordinates_prefix_length(()) == 0
        prefix = module.common_coordinates_prefix_length(coordinates)
        prefix_by_id = module.common_coordinates_prefix_length(
            coordinates, threading.get_ident()
        )
        tuple_prefix = module.common_coordinates_prefix_length(tuple(coordinates))
        assert 0 <= prefix <= len(coordinates)
        assert 0 <= prefix_by_id <= len(coordinates)
        assert 0 <= tuple_prefix <= len(coordinates)
        assert module.common_coordinates_prefix_length([2**64 - 1]) == 0
        check_coordinates_delta(module)
        check_coordinate_hash(module)
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
        check_disable_for(module)
        check_enable_for(module)
        check_retrace_coordinates_xoption()
        check_deterministic_identity_hashes()

        print("_retrace_module=builtin")
        print(f"coordinates_type={type(coordinates).__name__}")
        print(f"coordinates_len={len(coordinates)}")
        print(f"coordinates_format={view.format}")
        print(f"coordinates_readonly={view.readonly}")
        print("common_coordinates_prefix_length=available")
        print("coordinates_delta=available")
        print("hash=available")
        print("disable_for=available")
        print("enable_for=available")
        print("retrace_coordinates_xoption=available")
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
