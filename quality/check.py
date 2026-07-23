# SPDX-License-Identifier: Apache-2.0
"""Repository-wide source, build-file, and license sanity checks."""

from __future__ import annotations

import ast
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


SPDX = "SPDX-License-Identifier: Apache-2.0"
SKIP_DIRECTORIES = {
    ".git",
    ".idea",
    ".vscode",
    "__pycache__",
    "build",
    "bazel-bin",
    "bazel-out",
    "bazel-testlogs",
}
LICENSED_SUFFIXES = {
    ".bzl",
    ".c",
    ".cc",
    ".cpp",
    ".h",
    ".hpp",
    ".py",
    ".rs",
    ".smithy",
}
LICENSED_NAMES = {
    ".bazelrc",
    ".clang-format",
    ".dockerignore",
    ".gitignore",
    "BUILD",
    "BUILD.bazel",
    "CMakeLists.txt",
    "Dockerfile",
    "MODULE.bazel",
    "docker-bake.hcl",
}
TEXT_SUFFIXES = LICENSED_SUFFIXES | {
    ".bazelrc",
    ".hcl",
    ".j2",
    ".json",
    ".md",
    ".schema",
    ".toml",
    ".txt",
}
FORBIDDEN = (
    re.compile(r"\b" + "ara" + r"::com\b", re.IGNORECASE),
    re.compile(r"\b" + "auto" + r"sar\b", re.IGNORECASE),
    re.compile("openvehicle" + r"framework\.org", re.IGNORECASE),
)


def workspace() -> Path:
    value = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if not value:
        raise RuntimeError("run with: bazel run //quality:check")
    return Path(value).resolve()


def executable(value: Path) -> Path:
    if value.is_absolute():
        return value
    candidates = [
        Path.cwd() / value,
        Path(sys.argv[0]).resolve().parent / value,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(f"formatter executable not found: {value}")


def ignored(path: Path, root: Path) -> bool:
    relative = path.relative_to(root)
    if relative.parts[:2] == ("docs", "design"):
        return True
    return any(
        part in SKIP_DIRECTORIES
        or part.startswith("bazel-")
        or part.startswith("build-")
        for part in relative.parts
    )


def candidates(root: Path) -> list[Path]:
    result = []
    for path in root.rglob("*"):
        if path.is_file() and not ignored(path, root):
            if path.name in LICENSED_NAMES or path.suffix in TEXT_SUFFIXES:
                result.append(path)
    return sorted(result)


def check_file(path: Path, root: Path) -> list[str]:
    relative = path.relative_to(root)
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return [f"{relative}: not valid UTF-8"]
    errors = []
    if content and not content.endswith("\n"):
        errors.append(f"{relative}: missing final newline")
    for line_number, line in enumerate(content.splitlines(), 1):
        if line.rstrip() != line:
            errors.append(f"{relative}:{line_number}: trailing whitespace")
        if "\t" in line:
            errors.append(f"{relative}:{line_number}: tab character")
    if path.name in LICENSED_NAMES or path.suffix in LICENSED_SUFFIXES:
        if SPDX not in "\n".join(content.splitlines()[:5]):
            errors.append(f"{relative}: missing Apache-2.0 SPDX header")
    for pattern in FORBIDDEN:
        if pattern.search(content):
            errors.append(f"{relative}: contains prohibited project terminology")
    if path.suffix == ".json":
        try:
            json.loads(content)
        except json.JSONDecodeError as error:
            errors.append(f"{relative}:{error.lineno}: invalid JSON: {error.msg}")
    if path.suffix == ".py":
        try:
            ast.parse(content, filename=str(relative))
        except SyntaxError as error:
            errors.append(f"{relative}:{error.lineno}: invalid Python: {error.msg}")
    if path.suffix in {".h", ".hpp"}:
        has_pragma_once = "#pragma once" in content
        has_include_guard = bool(
            re.search(r"^#ifndef\s+\w+\s*$", content, re.MULTILINE)
            and re.search(r"^#define\s+\w+\s*$", content, re.MULTILINE)
        )
        if not has_pragma_once and not has_include_guard:
            errors.append(f"{relative}: header needs #pragma once or an include guard")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--buildifier", required=True, type=Path)
    parser.add_argument("--clang-format", required=True, type=Path)
    args = parser.parse_args()
    args.buildifier = executable(args.buildifier)
    args.clang_format = executable(args.clang_format)
    root = workspace()
    files = candidates(root)
    errors = [error for path in files for error in check_file(path, root)]
    starlark = [
        path
        for path in files
        if path.name in {"BUILD", "BUILD.bazel", "MODULE.bazel"} or path.suffix == ".bzl"
    ]
    cpp = [
        path
        for path in files
        if path.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}
        and "codegen/tests/golden" not in path.relative_to(root).as_posix()
    ]
    for command, label in (
        ([str(args.buildifier), "-mode=check", "-lint=warn", *map(str, starlark)], "buildifier"),
        ([str(args.clang_format), "--dry-run", "--Werror", *map(str, cpp)], "clang-format"),
    ):
        completed = subprocess.run(command, cwd=root, text=True, capture_output=True, check=False)
        if completed.returncode:
            output = (completed.stdout + completed.stderr).strip()
            errors.append(f"{label} check failed:\n{output}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"quality gate passed for {len(files)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
