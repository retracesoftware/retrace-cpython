import ctypes
import importlib.util
import sys
import threading
import time
import _thread


def is_u64(value) -> bool:
    return type(value) is int and 0 < value < 2**64


class StopThread(Exception):
    pass


def main() -> int:
    print(f"python={sys.version.split()[0]}")
    print(f"executable={sys.executable}")

    retrace = None
    if importlib.util.find_spec("_retrace") is not None:
        retrace = __import__("_retrace")
        print("_retrace_module=builtin")
    else:
        print("_retrace_module=unavailable")

    ready = threading.Event()
    done = threading.Event()
    release = threading.Event()
    child = {}

    def worker() -> None:
        child["ident"] = _thread.get_ident()
        ready.set()
        try:
            while not release.is_set():
                time.sleep(0.01)
        except StopThread:
            child["stopped"] = True
        finally:
            done.set()

    start_ident = _thread.start_new_thread(worker, ())
    try:
        assert is_u64(start_ident)
        assert ready.wait(5)
        assert child["ident"] == start_ident

        if retrace is not None:
            assert retrace.coordinates(start_ident)[0] == start_ident
            assert not hasattr(retrace, "thread_id")
            assert not hasattr(retrace, "thread_id_from_ident")

        set_async_exc = ctypes.pythonapi.PyThreadState_SetAsyncExc
        set_async_exc.argtypes = (ctypes.c_ulong, ctypes.py_object)
        set_async_exc.restype = ctypes.c_int

        assert set_async_exc(ctypes.c_ulong(0), ctypes.py_object(StopThread)) == 0
        result = set_async_exc(
            ctypes.c_ulong(start_ident),
            ctypes.py_object(StopThread),
        )
        assert result == 1
        assert done.wait(5)
        assert child.get("stopped") is True
    finally:
        release.set()
        done.wait(5)

    print("ctypes_async_exc_ident=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
