import importlib.util
import sys
import threading
import time


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


def check_thread_callbacks(module) -> None:
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(1e-6)

    yield_hits = []
    resume_hits = []
    callback_deltas = []
    stop = False

    def yield_callback():
        yield_hits.append(threading.get_ident())
        callback_deltas.append(tuple(module.instruction_counters_delta()))

    def resume_callback():
        resume_hits.append(threading.get_ident())
        callback_deltas.append(tuple(module.instruction_counters_delta()))

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


def apply_counter_delta(stack: list[int], delta) -> None:
    common_count = delta[0]
    drop_count = len(stack) - common_count
    del stack[:drop_count]
    stack[:0] = delta[1:]


def check_instruction_counters_delta(module) -> None:
    stack = []

    def sync() -> None:
        delta = module.instruction_counters_delta()
        assert isinstance(delta, module.U64Buffer)
        assert len(delta) >= 1
        assert 0 <= delta[0] <= len(stack)
        apply_counter_delta(stack, delta)
        live = tuple(module.instruction_counters())
        assert len(stack) == len(live)
        assert tuple(stack[1:]) == live[1:]

    sync()

    def nested() -> None:
        sync()

    nested()
    sync()


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
        dropped = module.instruction_counters(None, 1)
        dropped_by_id = module.instruction_counters(threading.get_ident(), 1)
        dropped_all = module.instruction_counters(None, len(counters) + 1)
        assert len(dropped) == max(0, len(counters) - 1)
        assert len(dropped_by_id) == max(0, len(counters) - 1)
        assert tuple(dropped_all) == ()
        try:
            module.instruction_counters(None, -1)
        except ValueError:
            pass
        else:
            raise AssertionError("negative instruction_counters drop accepted")
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
        check_instruction_counters_delta(module)
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

        print("retrace_module=builtin")
        print(f"instruction_counters_type={type(counters).__name__}")
        print(f"instruction_counters_len={len(counters)}")
        print(f"instruction_counters_format={view.format}")
        print(f"instruction_counters_readonly={view.readonly}")
        print("common_counters_prefix_length=available")
        print("instruction_counters_delta=available")
        print("thread_yield_callback=available")
        print("thread_resume_callback=available")
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
