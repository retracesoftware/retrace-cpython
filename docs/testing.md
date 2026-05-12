# Testing

The repository has two validation layers: focused smoke tests for Retrace probe
behavior and CPython's own regression tests for interpreter compatibility.

## Smoke Tests

Run the main capability smoke test against a patched executable:

```bash
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/probe_capability.py
```

Other focused smoke tests cover thread handoff, deterministic thread ids,
scheduling callbacks, public `retrace` wrappers, and schedule capture/replay:

```bash
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/retrace_public.py
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/thread_handoff.py
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/thread_id_determinism.py
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/thread_probe_concurrency.py
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/thread_schedule_minimal.py
build/src/cpython-3.12.13+retrace/python.exe tests/smoke/thread_schedule_stress.py
```

## Vanilla Compatibility Checks

`scripts/test-against-vanilla` runs smoke tests with a patched executable while
pointing it at a matching vanilla CPython install. This validates the intended
runtime shape: the wheel carries a minimal patched executable overlay, not a
full standard library copy.

## CPython Regression Tests

Run a focused CPython regression test:

```bash
build/src/cpython-3.12.13+retrace/python.exe -m test test_sys -q
```

The full CPython test suite is the stronger validation gate for release builds.
The GitHub workflow runs it unless `skip_tests` is enabled for a fast packaging
smoke run.

## Docs Build Check

The docs build runs MkDocs in strict mode:

```bash
python3 scripts/build-docs
```

Strict mode fails on broken links and turns missing `llms.txt` generation into a
release blocker.
