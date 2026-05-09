# Smoke Tests

These checks should run under both vanilla and patched CPython. Missing probe
support is reported as unavailable rather than treated as an import crash.

Example:

```bash
build/install/3.12.8+retrace/bin/python3 tests/smoke/probe_capability.py
build/install/3.12.8+retrace/bin/python3 tests/smoke/ctypes_thread_ident.py
```
