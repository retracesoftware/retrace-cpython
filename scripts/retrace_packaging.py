import fnmatch
import os
import re
import shutil
from pathlib import Path


LIBRARY_PATTERNS = (
    "python*.dll",
    "vcruntime*.dll",
    "libpython*.so",
    "libpython*.so.*",
    "libpython*.dylib",
)

PYTHON_EXECUTABLE_RE = re.compile(
    r"(?:python(?:\.exe)?|python3|python\d+(?:\.\d+)?[a-z]*)$"
)


def is_python_executable_name(name):
    if name.endswith("-config"):
        return False
    return PYTHON_EXECUTABLE_RE.fullmatch(name) is not None


def is_python_executable(path):
    return is_python_executable_name(path.name) and (
        os.name == "nt" or os.access(path, os.X_OK)
    )


def matches(name, patterns):
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)


def executable_preference(path):
    name = path.name
    if name == "python.exe":
        return (0, len(path.parts), str(path))
    if name == "python":
        return (1, len(path.parts), str(path))
    if name == "python3":
        return (2, len(path.parts), str(path))
    return (3, len(path.parts), str(path))


def selected_runtime_files(root):
    root = Path(root)
    executables_by_identity = {}
    libraries = []

    for path in root.rglob("*"):
        if not path.is_file():
            continue

        name = path.name
        if is_python_executable(path):
            stat = path.stat()
            identity = (stat.st_dev, stat.st_ino)
            current = executables_by_identity.get(identity)
            if current is None or executable_preference(path) < executable_preference(current):
                executables_by_identity[identity] = path
        elif matches(name, LIBRARY_PATTERNS):
            libraries.append(path)

    executables = sorted(executables_by_identity.values(), key=executable_preference)
    if not executables:
        raise SystemExit(f"no Python executable found under {root}")

    return sorted(set(executables + libraries))


def copy_runtime_files(runtime_root, runtime_dest):
    runtime_root = Path(runtime_root)
    runtime_dest = Path(runtime_dest)
    copied = []

    for src in selected_runtime_files(runtime_root):
        rel = src.relative_to(runtime_root)
        dest = runtime_dest / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        copied.append(rel)

    return copied
