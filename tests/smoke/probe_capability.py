import importlib.util
import sys


def main() -> int:
    print(f"python={sys.version.split()[0]}")
    print(f"executable={sys.executable}")

    spec = importlib.util.find_spec("retrace")
    if spec is not None:
        module = __import__("retrace")
        counters = module.instruction_counters()
        view = memoryview(counters)
        assert view.format == "Q"
        assert view.readonly
        assert tuple(counters[0:]) == tuple(counters)
        assert counters == tuple(counters)
        print("retrace_module=available")
        print(f"instruction_counters_type={type(counters).__name__}")
        print(f"instruction_counters_len={len(counters)}")
        print(f"instruction_counters_format={view.format}")
        print(f"instruction_counters_readonly={view.readonly}")
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
