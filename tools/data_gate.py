#!/usr/bin/env python3
"""Check the packaged data layout used by Distill's runtime.

The executable never opens a checkout-relative path. A build tree stages the
same layout under build/share/distill; an install uses the corresponding
datadir. This gate checks the contract at either root.
"""

import argparse
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_FILES = (
    "welcome.html",
    "fonts/DejaVuSans-Regular.ttf",
)
REQUIRED_DIRECTORIES = ("fonts", "profiles")


def check(root):
    root = Path(root)
    problems = []
    if not root.is_dir():
        return [f"data root is missing: {root}"]
    for relative in REQUIRED_DIRECTORIES:
        path = root / relative
        if not path.is_dir():
            problems.append(f"missing data directory: {relative}")
    for relative in REQUIRED_FILES:
        path = root / relative
        if not path.is_file():
            problems.append(f"missing data file: {relative}")
    return problems


def self_test():
    with tempfile.TemporaryDirectory(prefix="distill-data-") as directory:
        root = Path(directory)
        for relative in REQUIRED_DIRECTORIES:
            (root / relative).mkdir(parents=True)
        for relative in REQUIRED_FILES:
            (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / relative).write_bytes(b"data")
        if check(root):
            print("data gate self-test FAILED on valid layout")
            return 1
        (root / "welcome.html").unlink()
        if not check(root):
            print("data gate self-test FAILED to catch missing data")
            return 1
    print("data gate self-test: valid and missing-data controls pass")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, default=ROOT / "build" / "share" / "distill")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    problems = check(args.data)
    for problem in problems:
        print(f"data: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"data: {args.data}: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
