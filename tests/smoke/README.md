# Smoke Tests

These checks should run under both vanilla and patched CPython. Missing probe
support is reported as unavailable rather than treated as an import crash.

Example:

```bash
build/install/3.12.8+retrace/bin/python3 tests/smoke/probe_capability.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/coordinate_contracts.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/ctypes_thread_ident.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/coordinate_wrappers.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/retrace_public.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_handoff.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_id_determinism.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_probe_concurrency.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_schedule_fresh.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_schedule_minimal.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_schedule_primitives.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_schedule_stress.py
```

`probe_capability.py` includes callback transparency checks: coordinates,
coordinate hashes, and deltas observed inside Retrace callbacks must describe
the pinned application instruction boundary, not the Python frames entered by
the callback itself. It also checks call_at hit, overshoot, and
past-coordinate rejection behavior, plus the convention where an
`retrace.include` wrapper runs visible application work under the pinned frame.

`coordinate_contracts.py` keeps the public `retrace` coordinate semantics close
to retrace-python: visible frame-pair shape, nested/repeated call coordinates,
per-thread coordinate and delta isolation, `exclude`/`include` transparency,
and `call_at` past, overshoot, and transparent callback behavior.

`coordinate_wrappers.py` checks the public `_retrace.exclude` and
`_retrace.include` decorators: hidden callable frames are omitted from
coordinates, included callables become visible again inside excluded regions,
method binding works, vectorcall-style arguments are preserved, and exception
paths restore the thread's coordinate mode. It also checks
`_retrace.with_new_coordinates`, including fresh root coordinates, exception
restoration, and deterministic same-process child thread ids.

`retrace_public.py` checks the Python `retrace` convenience module: public
callback registration through `retrace.callbacks` wraps callbacks with
`_retrace.exclude`, replay callbacks can call application-visible work through
`retrace.include`, and overshoot callbacks use the same wrapping path.

`thread_handoff.py` checks the `_retrace.ThreadHandoff` replay gate: stable
thread-id arguments, durable transfer tokens for targets that are not asleep
yet, timeout failures, GIL release while parked, repeated ping-pong handoffs,
and close waking sleepers.

`thread_id_determinism.py` starts a small `_thread.start_new_thread` loop in
fresh subprocesses and asserts the returned public thread-id sequence is stable
across interpreter runs with the same Retrace root seed.

`thread_probe_concurrency.py` checks native probe behavior without trace files:
call_at callbacks fire only on the requested thread, overshoot callbacks are
delivered for both post-arm and already-past coordinates, `thread_delta()`
state is isolated per thread, start callbacks delivered before the first thread
frame see an empty cursor, and scheduler callback thread ids are visible through
`threading._active` or `threading._limbo`.

`thread_schedule.py` contains reusable capture/replay support for the schedule
tests. The scenario files own thread creation and workload shape, while the
helper records yield/resume callbacks and replays them with call_at callbacks
plus `_retrace.ThreadHandoff`. Replay thread-start callbacks register the
started thread, look ahead to arm any available yield point, then park in
`ThreadHandoff.start()` until the controller transfers execution with
`ThreadHandoff.to()`. The helper verifies that replay consumes the recorded
schedule and that each thread completes its expected work; exact event-order
comparison is available for workloads whose side effects align with call_at
boundaries.

`thread_schedule_primitives.py` runs a shared multi-thread workload through the
schedule controller while exercising an intentionally unlocked counter, a locked
counter, a semaphore gate, a semaphore-backed bounded buffer, and a condition
handoff. Critical sections are hidden from scheduler-control callbacks so replay
does not park a thread while it owns the primitive under test.

`thread_schedule_fresh.py` records and replays a small schedule sequentially in
one process under separate fresh `retrace.CoordinateSpace` instances. The
helper-owned launcher thread stays outside the schedule; the schedule covers
threads created by the target. Replay treats recorded thread ids as logical
schedule ids and binds them to the public thread ids observed during replay,
while still arming recorded coordinates in the replay space. It also exercises
`test_thread_schedule(retrace, function, args, kwargs)`, which raises if record
and replay return different values.
