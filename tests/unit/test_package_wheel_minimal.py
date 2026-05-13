#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


class PackageWheelMinimalTests(unittest.TestCase):
    def test_full_install_input_is_filtered_to_runtime_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            runtime = tmpdir / "runtime"
            docs = tmpdir / "docs"
            wheel_dir = tmpdir / "wheelhouse"

            (runtime / "bin").mkdir(parents=True)
            (runtime / "include/python3.12").mkdir(parents=True)
            (runtime / "lib/python3.12/test").mkdir(parents=True)
            (runtime / "lib/python3.12/lib-dynload").mkdir(parents=True)
            (runtime / "lib").mkdir(exist_ok=True)
            (runtime / "share/man/man1").mkdir(parents=True)
            docs.mkdir()

            python = runtime / "bin/python3.12"
            python.write_bytes(b"fake-python-binary")
            python.chmod(0o755)
            try:
                (runtime / "bin/python3").symlink_to("python3.12")
            except OSError:
                shutil.copy2(python, runtime / "bin/python3")

            (runtime / "bin/python3.12-config").write_text("config\n", encoding="utf-8")
            (runtime / "share/man/man1/python3.1").write_text("manpage\n", encoding="utf-8")
            (runtime / "include/python3.12/Python.h").write_text("header\n", encoding="utf-8")
            (runtime / "lib/libpython3.12.a").write_bytes(b"static archive")
            (runtime / "lib/libpython3.12.dylib").write_bytes(b"dynamic library")
            (runtime / "lib/python3.12/test/test_noise.py").write_text(
                "noise\n",
                encoding="utf-8",
            )
            stdlib_extension = (
                runtime
                / "lib/python3.12/lib-dynload/unicodedata.cpython-312-darwin.so"
            )
            stdlib_extension.write_bytes(b"stdlib extension")
            (docs / "llms.txt").write_text("short docs\n", encoding="utf-8")
            (docs / "llms-full.txt").write_text("full docs\n", encoding="utf-8")

            subprocess.run(
                [
                    sys.executable,
                    "scripts/package-wheel",
                    "3.12.4",
                    "0.0.0",
                    "macos-arm64",
                    str(runtime),
                    "--wheel-dir",
                    str(wheel_dir),
                    "--docs-dir",
                    str(docs),
                ],
                cwd=REPO_ROOT,
                check=True,
            )

            wheels = list(wheel_dir.glob("*.whl"))
            self.assertEqual(len(wheels), 1)
            with zipfile.ZipFile(wheels[0]) as zf:
                names = set(zf.namelist())

            runtime_names = {
                name
                for name in names
                if name.startswith("retracesoftware_cpython/_runtime/")
            }
            self.assertEqual(
                runtime_names,
                {
                    "retracesoftware_cpython/_runtime/bin/python3",
                    "retracesoftware_cpython/_runtime/lib/libpython3.12.dylib",
                },
            )
            self.assertIn("retracesoftware_cpython/llms.txt", names)
            self.assertIn("retracesoftware_cpython/llms-full.txt", names)
            self.assertFalse(any("/include/" in name for name in names))
            self.assertFalse(any("/test/" in name for name in names))
            self.assertFalse(any("/share/" in name for name in names))
            self.assertFalse(any(name.endswith(".a") for name in names))
            self.assertFalse(any("lib-dynload" in name for name in names))


if __name__ == "__main__":
    unittest.main()
