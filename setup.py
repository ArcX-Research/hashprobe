"""Build the C executable into the installable MCP package."""
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys

from setuptools import Distribution, setup
from setuptools.command.bdist_wheel import bdist_wheel
from setuptools.command.build_py import build_py


class BuildPackage(build_py):
    def run(self):
        if sys.platform not in ("darwin", "linux"):
            raise RuntimeError("Hashprobe currently supports macOS and Linux")
        super().run()
        root = Path(__file__).resolve().parent
        destination = Path(self.build_lib).resolve() / "hashprobe" / "bin" / "hashprobe"
        destination.parent.mkdir(parents=True, exist_ok=True)
        sources = ["main.c", "suite.c", "target.c", "report.c", "util.c", "reference/sha256.c"]
        command = [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=c11", "-O2", "-D_POSIX_C_SOURCE=200809L", "-D_XOPEN_SOURCE=700",
            "-DCJSON_NESTING_LIMIT=32", "-Isrc", "-Ivendor/cjson",
            # Match the wheel's minimum macOS version and native architecture.
            *(["-mmacosx-version-min=11.0", "-arch", platform.machine()] if sys.platform == "darwin" else []),
            *shlex.split(os.environ.get("CPPFLAGS", "")),
            *shlex.split(os.environ.get("CFLAGS", "")),
            *("src/" + name for name in sources), "vendor/cjson/cJSON.c",
            *shlex.split(os.environ.get("LDFLAGS", "")), "-o", str(destination),
        ]
        subprocess.run(command, cwd=root, check=True)
        destination.chmod(0o755)


class NativeDistribution(Distribution):
    def has_ext_modules(self):
        return True


class NativeWheel(bdist_wheel):
    def get_tag(self):
        _, _, platform_tag = super().get_tag()
        if sys.platform == "darwin":
            platform_tag = f"macosx_11_0_{platform.machine()}"
        # The executable is platform-specific, but does not use Python's ABI.
        return "py3", "none", platform_tag


setup(distclass=NativeDistribution, cmdclass={"build_py": BuildPackage, "bdist_wheel": NativeWheel})
