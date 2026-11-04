"""ctest means the build it just made. An editable install is found before PYTHONPATH."""

import os
from pathlib import Path

import _dfukit
import pytest


def pytest_configure() -> None:
    build = os.environ.get("DFUKIT_BUILD_DIR")
    if build and Path(_dfukit.__file__).parent.resolve() != Path(build).resolve():
        pytest.exit(f"testing {_dfukit.__file__}, not the build in {build}", returncode=1)
