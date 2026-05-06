import importlib.util
import sys
import threading


def check_replay_checkpoint(module) -> None:
    hits = []

    def attempt(delta: int) -> None:
        base = tuple(module.instruction_counters())
        target = (base[0] + delta, *base[1:])

        def callback():
            hits.append((tuple(module.instruction_counters()), target))

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


def main() -> int:
    print(f"python={sys.version.split()[0]}")
    print(f"executable={sys.executable}")

    spec = importlib.util.find_spec("retrace")
    if spec is not None:
        assert "retrace" in sys.builtin_module_names
        module = __import__("retrace")
        counters = module.instruction_counters()
        counters_by_id = module.instruction_counters(threading.get_ident())
        view = memoryview(counters)
        assert view.format == "Q"
        assert view.readonly
        assert tuple(counters[0:]) == tuple(counters)
        assert counters == tuple(counters)
        assert len(counters_by_id) == len(counters)
        assert module.common_counters_prefix_length(()) == 0
        prefix = module.common_counters_prefix_length(counters)
        prefix_by_id = module.common_counters_prefix_length(
            counters, threading.get_ident()
        )
        tuple_prefix = module.common_counters_prefix_length(tuple(counters))
        assert 0 <= prefix <= len(counters)
        assert 0 <= prefix_by_id <= len(counters)
        assert 0 <= tuple_prefix <= len(counters)
        assert module.common_counters_prefix_length([2**64 - 1]) == 0
        assert module.get_thread_switch_callback() is None

        def callback(_from_thread_id):
            return None

        module.set_thread_switch_callback(callback)
        assert module.get_thread_switch_callback() is callback
        module.set_thread_switch_callback(None)
        assert module.get_thread_switch_callback() is None
        check_replay_checkpoint(module)

        print("retrace_module=builtin")
        print(f"instruction_counters_type={type(counters).__name__}")
        print(f"instruction_counters_len={len(counters)}")
        print(f"instruction_counters_format={view.format}")
        print(f"instruction_counters_readonly={view.readonly}")
        print("common_counters_prefix_length=available")
        print("thread_switch_callback=available")
        print("replay_checkpoint=available")
    else:
        print("retrace_module=unavailable")

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
