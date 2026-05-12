import importlib.util
import json
import os
import subprocess
import sys


PROBE_MODULE = "_retrace"
RUNS = int(os.environ.get("RETRACE_THREAD_ID_DETERMINISM_RUNS", "8"))
THREADS = int(os.environ.get("RETRACE_THREAD_ID_DETERMINISM_THREADS", "16"))
ROOT_SEED = "retrace-thread-id-determinism-smoke"


CHILD = r"""
import json
import sys
import _thread

thread_count = int(sys.argv[1])
starter = getattr(_thread, "start_new", _thread.start_new_thread)
ids = []
locks = []


def worker(lock):
    lock.release()


for _ in range(thread_count):
    lock = _thread.allocate_lock()
    lock.acquire()
    ids.append(starter(worker, (lock,)))
    locks.append(lock)

for lock in locks:
    lock.acquire()

print(json.dumps(ids))
"""


def is_u64(value):
    return type(value) is int and 0 < value < 2**64


def run_child():
    env = os.environ.copy()
    env["RETRACE_ROOT_SEED"] = ROOT_SEED
    completed = subprocess.run(
        [sys.executable, "-I", "-c", CHILD, str(THREADS)],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    ids = json.loads(completed.stdout)
    assert type(ids) is list, ids
    assert len(ids) == THREADS, ids
    assert all(is_u64(item) for item in ids), ids
    return ids


def main() -> int:
    if importlib.util.find_spec(PROBE_MODULE) is None:
        print("_retrace_module=unavailable")
        print("thread_id_determinism=unavailable")
        return 0

    runs = [run_child() for _ in range(RUNS)]
    baseline = runs[0]
    mismatches = [
        (index, ids)
        for index, ids in enumerate(runs[1:], start=1)
        if ids != baseline
    ]
    assert not mismatches, {
        "baseline": baseline,
        "mismatches": mismatches,
    }

    print("_retrace_module=builtin")
    print(f"thread_id_determinism_runs={RUNS}")
    print(f"thread_id_determinism_threads={THREADS}")
    print("thread_id_determinism=available")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
