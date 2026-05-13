# Runtime Package

The `retracesoftware-cpython` PyPI package contains a minimal patched CPython
runtime overlay for one CPython version and one platform.

The package is intended for higher-level Retrace projects that need to launch a
patched interpreter without knowing the wheel's private directory layout.

## Public Helper

```python
import os
import retracesoftware_cpython

python = retracesoftware_cpython.executable()
os.execv(python, [python, "-c", "import retrace; print(retrace.coordinates())"])
```

`executable()` returns a string path to the packaged patched Python executable.
This is the stable public API for the wheel.

Do not hardcode paths under `retracesoftware_cpython._runtime`. The runtime
layout is private and can differ by platform.

## Console Script

The wheel installs:

```bash
retrace-python
```

This command launches the packaged patched interpreter and forwards arguments:

```bash
retrace-python -c "import retrace; print(retrace.hash())"
```

## Runtime Shape

Release wheels contain the patched Python executable and required runtime
dynamic libraries. They intentionally avoid vendoring a full standard library.
The launcher sets up the packaged executable so it can run with the matching
Python environment.

The wheel runtime payload is intentionally minimal: Python executable files and
required CPython runtime libraries only. It must not include the standard
library, tests, headers, static archives, or `ensurepip` bundles.

The package version is the Retrace runtime package version. It is independent
of the upstream CPython version embedded in a specific wheel. Wheel filenames
carry the CPython build tag and platform tag.

## AI-Readable Docs

Release builds generate `llms.txt` and `llms-full.txt` from the normal MkDocs
documentation. The wheel includes those files inside the
`retracesoftware_cpython` package so coding agents can inspect the installed
artifact without scraping GitHub.
