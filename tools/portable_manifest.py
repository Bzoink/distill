#!/usr/bin/env python3
"""Validate Distill's plain portable source manifest.

The manifest is intentionally not JSON or a CMake file. It is a four-column
pipe-separated inventory that a small Muroxi build can read without importing
CMake, while CMake consumes the same file at configure time.

    python3 tools/portable_manifest.py --check
    python3 tools/portable_manifest.py --self-test
"""

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "portability" / "sources.manifest"
TARGET = re.compile(r"^[A-Za-z0-9_-]+$")


def fields(path):
    entries = []
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        values = [value.strip() for value in raw.split("|")]
        if len(values) != 4:
            raise ValueError(f"{path}:{number}: expected four | separated fields")
        target, sources, defines, includes = values
        if not TARGET.fullmatch(target):
            raise ValueError(f"{path}:{number}: invalid target {target!r}")
        entries.append((number, target,
                        [item for item in sources.split(",") if item],
                        [item for item in defines.split(",") if item],
                        [item for item in includes.split(",") if item]))
    return entries


def expand(path, platform, external):
    return path.replace("{platform}", platform).replace("{external}", external)


def validate(manifest, root, platform):
    entries = fields(manifest)
    seen = set()
    problems = []
    external = os.path.relpath(root / "external" / "install", root)
    for number, target, sources, defines, includes in entries:
        if target in seen:
            problems.append(f"{manifest}:{number}: duplicate target {target}")
        seen.add(target)
        for value in defines:
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(=.*)?", value):
                problems.append(f"{manifest}:{number}: invalid define {value!r}")
        for relative in sources + includes:
            expanded = expand(relative, platform, external)
            candidate = root / expanded
            if not candidate.exists():
                kind = "source" if relative in sources else "include directory"
                problems.append(f"{manifest}:{number}: missing {kind} {relative!r} ({candidate})")
        for relative in sources:
            candidate = root / expand(relative, platform, external)
            if candidate.exists() and not candidate.is_file():
                problems.append(f"{manifest}:{number}: source is not a file {relative!r}")
    if not entries:
        problems.append(f"{manifest}: manifest is empty")
    return entries, problems


def self_test():
    with tempfile.TemporaryDirectory(prefix="distill-manifest-") as directory:
        root = Path(directory)
        (root / "src/platform/test").mkdir(parents=True)
        (root / "include").mkdir()
        (root / "src/platform/test/a.c").write_text("", encoding="utf-8")
        good = root / "good.manifest"
        good.write_text("thing|src/platform/{platform}/a.c|TEST=1|include\n", encoding="utf-8")
        entries, problems = validate(good, root, "test")
        if problems or len(entries) != 1:
            print(f"manifest self-test FAILED on valid input: {problems}")
            return 1
        bad = root / "bad.manifest"
        bad.write_text("thing|missing.c||include\nthing|src/platform/test/a.c||include\n",
                       encoding="utf-8")
        _, problems = validate(bad, root, "test")
        if len(problems) != 2:
            print(f"manifest self-test FAILED on invalid input: {problems}")
            return 1
    print("portable manifest self-test: valid, duplicate and missing paths caught")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--platform", default="linux")
    parser.add_argument("--check", action="store_true", help="validate the manifest (the default)")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    try:
        entries, problems = validate(args.manifest, args.root, args.platform)
    except (OSError, ValueError) as error:
        print(f"portable manifest: {error}", file=sys.stderr)
        return 1
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"portable manifest: {len(entries)} targets, platform {args.platform}, OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
