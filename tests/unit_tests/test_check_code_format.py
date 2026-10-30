"""
Unit tests for scripts/check_code_format.sh.

Each test makes a throwaway git repository with the script and a .clang-format, commits a base,
changes it, and runs the script against the base: a misformatted added line fails, a formatted one
passes, a file that is not clang-format clean at the base keeps its own style and is skipped, and a
base that does not resolve is an error. The tests skip where clang-format or git clang-format is
missing.

Copyright 2026 NVIDIA Corporation.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).parent.parent.parent
SCRIPT = REPO_ROOT / "scripts" / "check_code_format.sh"
CLANG_FORMAT = os.environ.get("CLANG_FORMAT", "clang-format")

CLEAN = "int add(int a, int b) {\n    return a + b;\n}\n"
UNCLEAN = "int add(int a, int b)\n{\n    return a + b;\n}\n"


def tools_present():
    """Whether clang-format and git clang-format run."""
    if shutil.which(CLANG_FORMAT) is None:
        return False
    result = subprocess.run(["git", "clang-format", "-h"], capture_output=True, check=False)
    return result.returncode == 0


pytestmark = pytest.mark.skipif(not tools_present(),
                                reason="clang-format or git clang-format missing")


def git(repo, *args):
    """Runs git in the repository."""
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True,
                   env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                        "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"})


def make_repo(tmp_path, files):
    """A repository whose base commit, tagged base, holds the files."""
    repo = tmp_path / "repo"
    (repo / "scripts").mkdir(parents=True)
    shutil.copy(SCRIPT, repo / "scripts" / "check_code_format.sh")
    (repo / ".clang-format").write_text("BasedOnStyle: Google\nIndentWidth: 4\nColumnLimit: 100\n"
                                        "AllowShortFunctionsOnASingleLine: None\n")
    for name, text in files.items():
        (repo / name).write_text(text)
    git(repo, "init", "-q")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "base")
    git(repo, "tag", "base")
    return repo


def check(repo, base="base"):
    """The script's exit status and output against base."""
    result = subprocess.run(["bash", "scripts/check_code_format.sh", base], cwd=repo,
                            capture_output=True, text=True, check=False,
                            env={**os.environ, "CLANG_FORMAT": CLANG_FORMAT})
    return result.returncode, result.stdout + result.stderr


def test_misformatted_line_fails(tmp_path):
    """A badly formatted line added to a clean file fails the check."""
    repo = make_repo(tmp_path, {"a.cpp": CLEAN})
    (repo / "a.cpp").write_text(CLEAN + "int   sub(int a,int b){return a-b;}\n")
    status, output = check(repo)
    assert status == 1, output


def test_formatted_line_passes(tmp_path):
    """A formatted line added to a clean file passes."""
    repo = make_repo(tmp_path, {"a.cpp": CLEAN})
    (repo / "a.cpp").write_text(CLEAN + "int sub(int a, int b) {\n    return a - b;\n}\n")
    status, output = check(repo)
    assert status == 0, output


def test_unclean_file_keeps_its_style(tmp_path):
    """A file that is not clang-format clean at the base is skipped."""
    repo = make_repo(tmp_path, {"b.cpp": UNCLEAN})
    (repo / "b.cpp").write_text(UNCLEAN + "int sub(int a, int b)\n{\n    return a - b;\n}\n")
    status, output = check(repo)
    assert status == 0, output
    assert "skipped" in output and "b.cpp" in output


def test_unresolved_base_is_an_error(tmp_path):
    """A base that does not resolve is an error, not a pass."""
    repo = make_repo(tmp_path, {"a.cpp": CLEAN})
    status, output = check(repo, base="no-such-ref")
    assert status == 2, output
