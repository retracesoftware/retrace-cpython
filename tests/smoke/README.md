# Smoke Tests

These checks should run under both vanilla and patched CPython. Missing probe
support is reported as unavailable rather than treated as an import crash.

Example:

```bash
build/install/3.12.8+retrace/bin/python3 tests/smoke/probe_capability.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/ctypes_thread_ident.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/coordinate_wrappers.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/retrace_public.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_handoff.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_id_determinism.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_probe_concurrency.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_schedule_minimal.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/thread_schedule_stress.py
```

`probe_capability.py` includes callback transparency checks: coordinates,
coordinate hashes, and deltas observed inside Retrace callbacks must describe
the pinned application instruction boundary, not the Python frames entered by
the callback itself. It also checks call_at hit, overshoot, and
past-coordinate rejection behavior, plus the convention where an
`retrace.include` wrapper runs visible application work under the pinned frame.

`coordinate_wrappers.py` checks the public `_retrace.exclude` and
`_retrace.include` decorators: hidden callable frames are omitted from
coordinates, included callables become visible again inside excluded regions,
method binding works, vectorcall-style arguments are preserved, and exception
paths restore the thread's coordinate mode.

`retrace_public.py` checks the Python `retrace` convenience module: public
callback registration through `retrace.callbacks` wraps callbacks with
`_retrace.exclude`, replay callbacks can call application-visible work through
`retrace.include`, and overshoot callbacks use the same wrapping path.

`thread_handoff.py` checks the `_retrace.ThreadHandoff` replay gate: stable
thread-id arguments, durable wake tokens for targets that are not asleep yet,
GIL release while parked, repeated ping-pong handoffs, and close waking
sleepers.

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
plus `_retrace.ThreadHandoff`. The helper verifies that replay consumes the
recorded schedule and that each thread completes its expected work; exact
event-order comparison is available for workloads whose side effects align with
call_at boundaries.
