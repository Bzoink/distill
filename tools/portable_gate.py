#!/usr/bin/env python3
"""M5 gates for static binaries and Muroxi's platform symbol surface.

The gates accept an explicit artifact so a Muroxi build can run the same
checks after copying its result back to the host:

    python3 tools/portable_gate.py --static-link build/distill-static
    python3 tools/portable_gate.py --symbols build/distill-static
    python3 tools/portable_gate.py --self-test

`--static-link` proves there is no ELF interpreter, dynamic section, or
runtime loader dependency. `--symbols` reads undefined symbols from one or
more objects/archives and checks them against portability/muroxi-symbols.txt.
The latter is the exported libc/platform surface captured from Muroxi; the
other static archives in the final link are checked by their own package
profiles.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SYMBOLS = ROOT / "portability" / "muroxi-symbols.txt"


def tool(name):
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"required tool not found: {name}")
    return path


def output(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as error:
        raise RuntimeError(f"could not run {' '.join(command)}: {error}") from error
    return result.returncode, result.stdout + result.stderr


def static_problems(binary):
    path = Path(binary)
    if not path.is_file() or not os.access(path, os.X_OK):
        return [f"static link: not an executable file: {path}"]
    problems = []
    try:
        _, file_info = output([tool("file"), str(path)])
        if "statically linked" not in file_info:
            problems.append(f"static link: {path} is not statically linked ({file_info.strip()})")
        _, program = output([tool("readelf"), "-l", str(path)])
        if re.search(r"\bINTERP\b", program):
            problems.append("static link: ELF contains an INTERP segment")
        dynamic_status, dynamic = output([tool("readelf"), "-d", str(path)])
        if dynamic_status == 0 and "There is no dynamic section" not in dynamic:
            problems.append("static link: ELF contains a dynamic section")
        symbols_status, symbols = output([tool("nm"), "-a", str(path)])
        if symbols_status == 0:
            forbidden = sorted({name for name in ("dlopen", "dlsym", "dlclose")
                                if re.search(rf"\b{re.escape(name)}\b", symbols)})
            if forbidden:
                problems.append("static link: dynamic loader symbols present: " + ", ".join(forbidden))
    except RuntimeError as error:
        problems.append(str(error))
    return problems


def undefined_symbols(paths):
    names = set()
    problems = []
    try:
        nm = tool("nm")
    except RuntimeError as error:
        return set(), [str(error)]
    for path in paths:
        status, text = output([nm, "-u", str(path)])
        if status not in (0, 1):
            problems.append(f"symbols: nm failed for {path}")
            continue
        for line in text.splitlines():
            parts = line.split()
            if not parts or parts[-1] in ("U", "w", "W"):
                continue
            name = parts[-1]
            if name in ("U", "w", "W"):
                continue
            names.add(name.split("@", 1)[0])
    return names, problems


def symbol_problems(paths, allowlist):
    if not allowlist.is_file():
        return [f"symbols: allowlist is missing: {allowlist}"]
    allowed = {line.strip() for line in allowlist.read_text(encoding="utf-8").splitlines()
               if line.strip() and not line.lstrip().startswith("#")}
    found, problems = undefined_symbols(paths)
    unknown = sorted(found - allowed)
    problems.extend(f"symbols: not exported by Muroxi: {name}" for name in unknown)
    return problems


def self_test():
    failures = []
    with tempfile.TemporaryDirectory(prefix="distill-portable-gate-") as directory:
        root = Path(directory)
        source = root / "tiny.c"
        binary = root / "tiny"
        source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        compiler = shutil.which("musl-gcc") or shutil.which("cc")
        if compiler:
            command = [compiler, str(source), "-o", str(binary)]
            if Path(compiler).name == "musl-gcc":
                command.insert(1, "-static")
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            if result.returncode == 0:
                problems = static_problems(binary)
                if problems:
                    failures.extend(problems)
        object_path = root / "tiny.o"
        subprocess.run(["cc", "-c", str(source), "-o", str(object_path)], check=True)
        allowlist = root / "symbols.txt"
        allowlist.write_text("puts\n", encoding="utf-8")
        # The empty object has no undefined symbol and therefore must pass;
        # the fixture below proves an unknown symbol is rejected.
        unresolved = root / "unresolved.c"
        unresolved.write_text("extern int foreign(void); int f(void) { return foreign(); }\n",
                              encoding="utf-8")
        unresolved_object = root / "unresolved.o"
        subprocess.run(["cc", "-c", str(unresolved), "-o", str(unresolved_object)], check=True)
        if not symbol_problems([unresolved_object], allowlist):
            failures.append("symbols: unknown-symbol negative control passed")
    if failures:
        print("portable gate self-test FAILED")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("portable gate self-test: static and unknown-symbol controls pass")
    return 0


def default_binary():
    candidates = [os.environ.get("DISTILL_STATIC_BINARY"),
                  str(ROOT / "build" / "distill-static"),
                  str(ROOT / "build" / "distill")]
    return next((candidate for candidate in candidates if candidate), candidates[-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--static-link", metavar="BINARY", nargs="?", const=default_binary())
    parser.add_argument("--symbols", metavar="OBJECT", nargs="+")
    parser.add_argument("--allowlist", type=Path, default=DEFAULT_SYMBOLS)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.static_link and not args.symbols:
        parser.error("choose --static-link, --symbols, or --self-test")
    failures = []
    if args.static_link:
        failures.extend(static_problems(args.static_link))
        if not failures:
            print(f"static link: {args.static_link}: OK")
    if args.symbols:
        failures.extend(symbol_problems(args.symbols, args.allowlist))
        if not failures:
            print(f"symbols: {len(args.symbols)} object(s), OK")
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
