#!/usr/bin/env python3
"""Build and run HTOS host tests consistently on Windows and Linux."""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
from pathlib import Path

TEST_DIRECTORY = Path(__file__).resolve().parent
TEST_SPECS = {
    "test_example.out": {"sources": ["test_example.c", "unity.c"]},
    "test_list.out": {"sources": ["test_list.c", "unity.c", "../kernel/htlist.c"]},
    "test_queue.out": {
        "sources": [
            "test_queue.c",
            "unity.c",
            "../kernel/htqueue.c",
            "../kernel/htsemaphore.c",
            "../kernel/htlist.c",
        ]
    },
    "test_mem.out": {"sources": ["test_mem.c", "unity.c", "../kernel/htmem.c"]},
    "test_task.out": {
        "sources": ["test_task.c", "unity.c", "../kernel/httask.c", "../kernel/htlist.c"],
        "extra_flags": [
            "-DHTOS_HOST_TEST",
            "-Wno-pointer-to-int-cast",
            "-Wno-int-to-pointer-cast",
            "-Istubs",
        ],
    },
    "test_utils.out": {
        "sources": ["test_utils.c", "unity.c", "../kernel/htutils.c"],
    },
    "test_watchdog.out": {
        "sources": ["test_watchdog.c", "unity.c", "../kernel/htwatchdog.c"],
    },
}


def clean() -> int:
    for value in [*TEST_SPECS, "test_results.txt"]:
        path = TEST_DIRECTORY / value
        if path.exists():
            path.unlink()
    for object_file in TEST_DIRECTORY.glob("*.o"):
        object_file.unlink()
    return 0


def build_tests(compiler: str) -> int:
    default_flags = "-Wall -Wextra -Wpedantic -Werror -std=c11"
    flags = shlex.split(os.environ.get("CFLAGS", default_flags), posix=os.name != "nt")
    linker_flags = shlex.split(os.environ.get("LDFLAGS", ""), posix=os.name != "nt")

    for target, spec in TEST_SPECS.items():
        command = [
            compiler,
            *flags,
            *spec.get("extra_flags", []),
            "-I.",
            "-I../include",
            "-o",
            target,
            *spec["sources"],
            *linker_flags,
        ]
        print("Building:", " ".join(command))
        completed = subprocess.run(command, cwd=TEST_DIRECTORY, check=False)
        if completed.returncode != 0:
            return completed.returncode
    return 0


def run_tests() -> int:
    failures = 0
    sections: list[str] = []

    for target in TEST_SPECS:
        executable = (TEST_DIRECTORY / target).resolve()
        heading = f"=== Running {target} ==="
        print(heading)
        completed = subprocess.run(
            [str(executable)],
            check=False,
            capture_output=True,
            text=True,
        )
        output = completed.stdout + completed.stderr
        print(output, end="" if output.endswith("\n") else "\n")
        sections.append(f"{heading}\n{output}")
        if completed.returncode != 0:
            failure_line = f"{target} exited with code {completed.returncode}"
            print(failure_line)
            sections.append(failure_line)
            failures += 1

    (TEST_DIRECTORY / "test_results.txt").write_text("\n".join(sections), encoding="utf-8")
    if failures:
        print(f"{failures} test executable(s) failed")
        return 1

    print("All tests passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--build", action="store_true", help="build and run all host tests")
    action.add_argument("--build-only", action="store_true", help="build host tests without running them")
    action.add_argument("--clean", action="store_true", help="remove generated host-test files")
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"), help="C compiler executable")
    arguments = parser.parse_args()

    if arguments.clean:
        return clean()

    build_result = build_tests(arguments.cc)
    if build_result != 0 or arguments.build_only:
        return build_result
    return run_tests()


if __name__ == "__main__":
    raise SystemExit(main())
