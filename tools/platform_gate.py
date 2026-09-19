#!/usr/bin/env python3
"""The platform gate (muroxi_compat.md, M1).

Nothing outside src/platform/<os>/ may reach the operating system directly.
This checks the two ways it could:

  includes     every #include <...> outside src/platform/<os>/ must be a
               standard C header, a C++ standard header in a .cpp file, or a
               library on the allowlist below; every #include "..." must be a
               project header or a vendored single-header library
  feature      no file outside src/platform/<os>/ may define a feature-test
               macro (_POSIX_C_SOURCE, _GNU_SOURCE, ...). The core is built
               as strict ISO C (-std=c23 -pedantic-errors), so without one no
               POSIX declaration is visible there and the compiler refuses
               any OS call that slipped past the include check

A new library belongs on the allowlist deliberately, with its portability
understood: that is the point of listing them.

    python3 tools/platform_gate.py              check src/
    python3 tools/platform_gate.py --self-test  show the gate catches what it should
"""

import argparse
import os
import re
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

C_STANDARD = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h",
    "iso646.h", "limits.h", "locale.h", "math.h", "setjmp.h", "signal.h", "stdalign.h",
    "stdarg.h", "stdatomic.h", "stdbit.h", "stdbool.h", "stdckdint.h", "stddef.h",
    "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h", "string.h", "tgmath.h",
    "threads.h", "time.h", "uchar.h", "wchar.h", "wctype.h",
}

CXX_STANDARD = {
    "algorithm", "array", "cctype", "cmath", "cstddef", "cstdint", "cstdio", "cstdlib",
    "cstring", "functional", "map", "memory", "set", "string", "unordered_map", "utility",
    "vector",
}

# Libraries the core may use. Each is portable C (or HarfBuzz-style C++ with
# no runtime) that Muroxi already builds or can build -- or is on its way out
# (litehtml, Cairo: muroxi_compat.md M3, M4).
LIBRARIES = (
    "quickjs.h",        # QuickJS-ng
    "curl/",            # libcurl, on every platform
    "lexbor/",          # Lexbor
    "linebreak.h",      # libunibreak
    "graphemebreak.h",  # libunibreak
    "png.h", "jpeglib.h", "webp/",      # image decoders
    "ft2build.h", "harfbuzz/",          # FreeType, HarfBuzz
    "cairo/", "litehtml/",              # to be removed (M3, M4)
)

# Single-header libraries vendored under external/src, included by name.
VENDORED = {"miniaudio.h", "termbox2.h"}

FEATURE_MACROS = re.compile(
    r"^\s*#\s*define\s+(_POSIX_C_SOURCE|_XOPEN_SOURCE|_GNU_SOURCE|_DEFAULT_SOURCE|"
    r"_BSD_SOURCE|_SVID_SOURCE|_DARWIN_C_SOURCE|_NETBSD_SOURCE)\b")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
SOURCES = (".c", ".h", ".inc", ".cpp")


def platform_backend(path, src):
    """True for files under src/platform/<os>/, which may use the OS."""
    relative = os.path.relpath(path, src).split(os.sep)
    return len(relative) >= 3 and relative[0] == "platform"


def project_headers(src):
    headers = set()
    for directory, _, files in os.walk(src):
        for name in files:
            if name.endswith((".h", ".inc")):
                headers.add(name)
            elif name.endswith(".h.in"):
                headers.add(name[:-3])
    return headers


def check(src):
    """[(path, line, message)] for every violation under src."""
    problems = []
    known = project_headers(src)
    for directory, subdirectories, files in os.walk(src):
        subdirectories[:] = [d for d in subdirectories if d != "documents"]
        for name in sorted(files):
            if not name.endswith(SOURCES):
                continue
            path = os.path.join(directory, name)
            if platform_backend(path, src):
                continue
            cxx = name.endswith(".cpp")
            with open(path, encoding="utf-8", errors="replace") as handle:
                for number, line in enumerate(handle, 1):
                    if FEATURE_MACROS.match(line):
                        problems.append((path, number, "feature-test macro outside src/platform/<os>/: "
                                         + line.strip()))
                        continue
                    match = INCLUDE.match(line)
                    if not match:
                        continue
                    kind, header = match.groups()
                    if kind == '"':
                        if os.path.basename(header) not in known and header not in VENDORED:
                            problems.append((path, number, f'unknown header "{header}"'))
                        continue
                    if header in C_STANDARD or (cxx and header in CXX_STANDARD):
                        continue
                    if header.startswith(LIBRARIES):
                        continue
                    problems.append((path, number, f"OS or unlisted header <{header}>"))
    return problems


def self_test():
    """The gate must flag each planted violation and nothing else."""
    with tempfile.TemporaryDirectory(prefix="distill-gate-") as src:
        os.makedirs(os.path.join(src, "platform", "linux"))
        files = {
            "core_ok.c": '#include "core_ok.h"\n#include <stdio.h>\n#include <curl/curl.h>\n#include "miniaudio.h"\n',
            "core_ok.h": "#include <stdint.h>\n",
            "os_header.c": "#include <unistd.h>\n",
            "feature.c": "#define _GNU_SOURCE\n#include <string.h>\n",
            "unlisted.c": "#include <zlib.h>\n",
            "stray.c": '#include "nowhere.h"\n',
            "cxx_in_c.c": "#include <vector>\n",
            os.path.join("platform", "dthing.h"): "#include <sys/types.h>\n",
            os.path.join("platform", "linux", "thing.c"): "#define _GNU_SOURCE\n#include <unistd.h>\n",
        }
        for relative, text in files.items():
            with open(os.path.join(src, relative), "w", encoding="utf-8") as handle:
                handle.write(text)
        flagged = {os.path.relpath(path, src) for path, _, _ in check(src)}
    expected = {"os_header.c", "feature.c", "unlisted.c", "stray.c", "cxx_in_c.c",
                os.path.join("platform", "dthing.h")}
    if flagged != expected:
        print(f"platform gate self-test FAILED\n  flagged:  {sorted(flagged)}\n  expected: {sorted(expected)}")
        return 1
    print(f"platform gate self-test: {len(expected)} planted violations caught, "
          "the clean core file and the platform backend passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="check the gate itself")
    ap.add_argument("--src", default=os.path.join(ROOT, "src"), help="tree to check")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    problems = check(args.src)
    for path, number, message in problems:
        print(f"{os.path.relpath(path, ROOT)}:{number}: {message}")
    if problems:
        print(f"platform gate: {len(problems)} violation(s)")
        return 1
    print("platform gate: no OS header or feature-test macro outside src/platform/<os>/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
