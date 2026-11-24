#!/usr/bin/env python3
"""Bundle the native Distill executables and their PE DLL dependencies."""

import argparse
from collections import deque
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll", "cfgmgr32.dll",
    "crypt32.dll", "dnsapi.dll", "dwrite.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll",
    "kernel32.dll", "msvcrt.dll", "ncrypt.dll", "normaliz.dll", "ntdll.dll",
    "ole32.dll", "oleaut32.dll", "rpcrt4.dll", "secur32.dll", "shell32.dll",
    "shlwapi.dll", "ucrtbase.dll", "user32.dll", "userenv.dll", "usp10.dll", "version.dll",
    "winhttp.dll", "winmm.dll", "wldap32.dll", "ws2_32.dll",
}


def dll_names(objdump, file):
    output = subprocess.check_output([objdump, "-p", str(file)], text=True)
    return re.findall(r"^\s*DLL Name:\s*(\S+)", output, re.MULTILINE)


def collect_binaries(build, prefixes, objdump, destination):
    candidates = {}
    for prefix in prefixes:
        for file in sorted((prefix / "bin").glob("*")):
            if file.suffix.lower() == ".dll":
                candidates.setdefault(file.name.lower(), file)
    pending = deque(build / name for name in ("distill.exe", "distill-fb.exe"))
    collected = set()
    while pending:
        file = pending.popleft()
        key = file.name.lower()
        if key in collected:
            continue
        collected.add(key)
        shutil.copy2(file, destination / file.name)
        for name in dll_names(objdump, file):
            dll = name.lower()
            if dll in SYSTEM_DLLS or dll.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if dll not in candidates:
                raise ValueError(f"{file.name} needs {name}, missing from all runtime prefixes")
            if dll not in collected:
                pending.append(candidates[dll])
    return len(collected) - 2


def copy_licenses(prefixes, destination):
    for prefix in prefixes:
        licenses = prefix / "share/licenses"
        if licenses.is_dir():
            shutil.copytree(licenses, destination, dirs_exist_ok=True)
    vendored = ROOT / "external/src"
    linkedom = ROOT / "external/js/linkedom/node_modules"
    for folder in (vendored / "quickjs", vendored / "lexbor", *sorted(linkedom.iterdir())):
        if not folder.is_dir() or folder.name.startswith("."):
            continue
        for file in folder.iterdir():
            if file.is_file() and file.name.upper().startswith(("LICENSE", "NOTICE", "COPYING")):
                target = destination / folder.name / file.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(file, target)

    license_text = (vendored / "miniaudio/miniaudio.h").read_text(encoding="utf-8")
    marker = license_text.index("ALTERNATIVE 2 - MIT No Attribution")
    start = license_text.rfind("\n/*\n", 0, marker) + 4
    end = license_text.index("\n*/", marker)
    miniaudio = destination / "miniaudio"
    miniaudio.mkdir(exist_ok=True)
    (miniaudio / "LICENSE").write_text(license_text[start:end].strip() + "\n", encoding="utf-8")

    readability = destination / "readability"
    readability.mkdir(exist_ok=True)
    shutil.copy2(vendored / "lexbor/LICENSE", readability / "LICENSE")
    notices = []
    for name in ("Readability.js", "JSDOMParser.js"):
        source = (vendored / "readability" / name).read_text(encoding="utf-8")
        notices.append(name + "\n" + source.split("*/", 1)[0].removeprefix("/*").strip())
    (readability / "NOTICE").write_text("\n\n".join(notices) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, action="append", required=True,
                        help="dependency prefix with bin/ and share/licenses/; may repeat")
    parser.add_argument("--objdump", default="llvm-objdump")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="distill-win-package-") as work:
        root = Path(work) / "Distill"
        binaries = root / "bin"
        binaries.mkdir(parents=True)
        count = collect_binaries(args.build, args.prefix, args.objdump, binaries)
        data = root / "share/distill"
        shutil.copytree(ROOT / "external/fonts", data / "fonts")
        shutil.copytree(ROOT / "profiles", data / "profiles")
        shutil.copy2(ROOT / "share/welcome.html", data / "welcome.html")
        copy_licenses(args.prefix, root / "licenses")
        shutil.copy2(ROOT / "src/documents/WINDOWS.md", root / "WINDOWS.md")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(root.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(root.parent))
        print(f"{args.output}: two executables and {count} DLLs")


if __name__ == "__main__":
    main()
