#!/usr/bin/env python3
"""Build and package Distill for Linux x64, Windows x64, or Raspberry Pi ARM64."""

import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

from package_windows import copy_licenses

ROOT = Path(__file__).resolve().parents[1]
LINUX_BINARIES = (
    "distill", "distill-gtk", "distill-tui", "distill-fb", "distill-fb-x11", "distill-arm-b",
)
RAW = "https://raw.githubusercontent.com"
READABILITY = f"{RAW}/mozilla/readability/ab4027a8b37669745016869a37a504727992b2ba"
ARCHIVES = {
    "quickjs": (
        "https://codeload.github.com/quickjs-ng/quickjs/tar.gz/d7215b0ad3fc2c562e8317fd9bca058fd4863804",
        "a49defe70cbcae9226d9fac025af116e87ebae2440dcff1ca3c80b79e1cdef0b",
    ),
    "lexbor": (
        "https://codeload.github.com/lexbor/lexbor/tar.gz/b2ca101decbc38d0fd2d02383d23ac0cb5f22384",
        "673cf2fafc9c2e5e7e9ae3473434f58c049dc17cfa1e9903ebcd2deffca2f3be",
    ),
}
SOURCES = {
    "miniaudio/miniaudio.h": (
        f"{RAW}/mackron/miniaudio/0.11.26/miniaudio.h",
        "6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e",
    ),
    "termbox2/termbox2.h": (
        f"{RAW}/termbox/termbox2/0e801fd9808a94dbded47ddf86bea4f93049d64d/termbox2.h",
        "0ba7a4bbce31f98b337c0bd6df2758ef5ce4f5907cb0822cd16569ba1b56bc61",
    ),
    "readability/JSDOMParser.js": (
        f"{READABILITY}/JSDOMParser.js",
        "c33bdfb5b3b27672ad8470199fb6852e8b5223eeb1342ae3a9ff393e5bf50a2d",
    ),
    "readability/Readability.js": (
        f"{READABILITY}/Readability.js",
        "e9330028c8a5a4aa7d75147be2605d520f7f213c7b28474947dc0e9c984e9bed",
    ),
    "readability/LICENSE.md": (
        f"{READABILITY}/LICENSE.md",
        "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
    ),
    "readability/NOTICE": (
        f"{READABILITY}/NOTICE",
        "21d526d361caf2b1ca578820ef1113982da5c094878a26bdd7ad555f84946f54",
    ),
}


def run(*command, **kwargs):
    command = [os.fspath(part) for part in command]
    print("+ " + shlex.join(command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def executable(name):
    path = shutil.which(name)
    if path is None:
        raise ValueError(f"required executable not found: {name}")
    return os.path.abspath(path)


def sha256(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def download(cache, url, expected):
    path = cache / expected
    if path.is_file() and sha256(path) == expected:
        return path
    with tempfile.TemporaryDirectory(dir=cache) as temporary:
        candidate = Path(temporary) / "download"
        run("curl", "-fLsS", "--retry", "2", "-o", candidate, url)
        if sha256(candidate) != expected:
            raise ValueError(f"SHA-256 mismatch for {url}")
        candidate.replace(path)
    return path


def prepare_vendor(work, cache):
    vendor = work / "vendor"
    sources = vendor / "src"
    sources.mkdir(parents=True)
    cache.mkdir(parents=True, exist_ok=True)
    for name, pin in ARCHIVES.items():
        with tempfile.TemporaryDirectory(dir=work) as unpack:
            unpack = Path(unpack)
            with tarfile.open(download(cache, *pin), "r:gz") as archive:
                archive.extractall(unpack, filter="data")
            [source] = unpack.iterdir()
            source.rename(sources / name)
    for name, pin in SOURCES.items():
        destination = sources / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(download(cache, *pin), destination)
    js = vendor / "js/linkedom"
    shutil.copytree(ROOT / "tools/release-deps", js)
    run("npm", "ci", "--ignore-scripts", "--no-audit", "--no-fund", "--prefer-offline",
        "--cache", cache / "npm", cwd=js)
    return vendor


def configure(source, build, flags, env):
    run("cmake", "-S", source, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", *flags, env=env)


def build_dependencies(vendor, work, flags, env, jobs):
    prefix = work / "prefix"
    quickjs = work / "quickjs"
    quickjs_source = vendor / "src/quickjs"
    configure(quickjs_source, quickjs, [*flags, "-DBUILD_SHARED_LIBS=OFF"], env)
    run("cmake", "--build", quickjs, "--parallel", jobs, "--target", "qjs", env=env)
    (prefix / "include").mkdir(parents=True)
    (prefix / "lib").mkdir()
    shutil.copy2(quickjs_source / "quickjs.h", prefix / "include")
    shutil.copy2(quickjs / "libqjs.a", prefix / "lib")

    lexbor = work / "lexbor"
    configure(vendor / "src/lexbor", lexbor,
              [*flags, "-DLEXBOR_BUILD_STATIC=ON", "-DLEXBOR_BUILD_SHARED=OFF",
               "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_INSTALL_PREFIX={prefix}"], env)
    run("cmake", "--build", lexbor, "--parallel", jobs, env=env)
    run("cmake", "--install", lexbor, env=env)
    return prefix


def configure_distill(vendor, work, flags, env, jobs, extra=()):
    prefix = build_dependencies(vendor, work, flags, env, jobs)
    build = work / "distill"
    configure(ROOT, build, [*flags, f"-DEXTERNAL_PREFIX={prefix}",
                            f"-DDISTILL_VENDOR_DIR={vendor}", "-DDISTILL_STATIC_LEXBOR=ON",
                            "-DCMAKE_SKIP_INSTALL_RPATH=ON", *extra], env)
    return build


def project_version(build):
    header = (build / "generated/distill_version.h").read_text(encoding="utf-8")
    match = re.search(r'^#define DISTILL_VERSION "([0-9]+(?:\.[0-9]+)*)"$', header, re.MULTILINE)
    if match is None:
        raise ValueError("missing version in generated distill_version.h")
    return match[1]


def package_linux(build, vendor, archive, emulator=(), loader=None):
    command = [*emulator, loader, "--list"] if loader else ["ldd"]
    with tempfile.TemporaryDirectory() as staging:
        root = Path(staging) / "Distill"
        run("cmake", "--install", build, "--prefix", root)
        for name in LINUX_BINARIES:
            binary = root / "bin" / name
            if not binary.is_file():
                raise ValueError(f"Linux build did not install {name}. Check the development libraries.")
            libraries = subprocess.check_output([*command, binary], text=True,
                                                env={**os.environ, "LC_ALL": "C"})
            if "not found" in libraries:
                raise ValueError(f"unresolved Linux runtime libraries for {name}\n{libraries}")
        run(*emulator, root / "bin/distill", "--version", cwd=root.parent)
        copy_licenses([], root / "licenses", vendor, termbox=True)
        with tarfile.open(archive, "w:gz") as package:
            package.add(root, arcname=root.name)


def raspberry_pi_settings(sysroot):
    triplet = "aarch64-linux-gnu"
    loader = sysroot / "lib/ld-linux-aarch64.so.1"
    if not loader.is_file():
        raise ValueError(f"ARM64 loader missing from Raspberry Pi sysroot: {loader}")
    emulator = [executable("qemu-aarch64"), "-cpu", "cortex-a53", "-L", str(sysroot)]
    executable("ld.lld")
    flags = [
        "-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
        f"-DCMAKE_C_COMPILER={executable('clang')}",
        f"-DCMAKE_CXX_COMPILER={executable('clang++')}",
        f"-DCMAKE_C_COMPILER_TARGET={triplet}", f"-DCMAKE_CXX_COMPILER_TARGET={triplet}",
        f"-DCMAKE_SYSROOT={sysroot}",
        "-DCMAKE_EXE_LINKER_FLAGS_INIT=-fuse-ld=lld",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
        "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY",
        f"-DCMAKE_CROSSCOMPILING_EMULATOR={';'.join(emulator)}",
        "-DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=OFF",
    ]
    env = dict(os.environ)
    env.pop("PKG_CONFIG_PATH", None)
    env["PKG_CONFIG_SYSROOT_DIR"] = str(sysroot)
    env["PKG_CONFIG_LIBDIR"] = os.pathsep.join(
        str(sysroot / path) for path in (f"usr/lib/{triplet}/pkgconfig", "usr/share/pkgconfig"))
    return flags, env, emulator, loader


def windows_settings(prefix, toolchain, work):
    def tool(name):
        return executable(toolchain / name if toolchain else name)

    compiler = "x86_64-w64-mingw32"
    flags = [
        "-DCMAKE_SYSTEM_NAME=Windows", "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
        f"-DCMAKE_C_COMPILER={tool(compiler + '-clang')}",
        f"-DCMAKE_CXX_COMPILER={tool(compiler + '-clang++')}",
        f"-DCMAKE_FIND_ROOT_PATH={prefix}",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
        "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY",
    ]
    libcurl = prefix / "lib/libcurl.dll.a"
    if (not libcurl.is_file() or not (prefix / "lib/pkgconfig").is_dir()
            or not (prefix / "bin").is_dir()):
        raise ValueError(f"expected a Windows x64 UCRT dependency prefix at {prefix}")
    # MSYS2 .pc files use /ucrt64, including hard-coded paths in libjpeg.pc.
    # Give pkg-config that layout even if the caller named the prefix differently.
    ucrt64 = work / "ucrt64"
    ucrt64.symlink_to(prefix, target_is_directory=True)
    env = dict(os.environ)
    env.pop("PKG_CONFIG_PATH", None)
    env["PKG_CONFIG_SYSROOT_DIR"] = str(work)
    env["PKG_CONFIG_LIBDIR"] = os.pathsep.join(
        str(ucrt64 / path) for path in ("lib/pkgconfig", "share/pkgconfig"))
    extra = [
        f"-DCMAKE_RC_COMPILER={tool(compiler + '-windres')}",
        "-DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=OFF",
        "-DCURL_NO_CURL_CMAKE=ON", f"-DCURL_LIBRARY={libcurl}",
        f"-DCURL_INCLUDE_DIR={prefix / 'include'}",
    ]
    return flags, env, extra, tool("llvm-objdump")


def publish_packages(staged, destination):
    archives = sorted(staged.iterdir())
    checksums = []
    for archive in archives:
        checksum = archive.with_name(archive.name + ".sha256")
        checksum.write_text(f"{sha256(archive)}  {archive.name}\n", encoding="ascii")
        checksums.append(checksum)

    directory = os.open(destination, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(directory, fcntl.LOCK_EX)
        for file in archives + checksums:
            file.replace(destination / file.name)
    finally:
        os.close(directory)
    for archive in archives:
        print(destination / archive.name, flush=True)


def release(args):
    for command in ("cmake", "ninja", "pkg-config", "curl", "npm"):
        executable(command)
    linux = args.target in ("all", "linux")
    windows = args.target in ("all", "windows")
    raspberry_pi = args.target == "raspberry-pi"
    if linux or raspberry_pi:
        executable("ctest")
    if linux:
        executable("ldd")
    if windows:
        executable("makensis")
    jobs = str(args.jobs)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="distill-release-") as work, \
         tempfile.TemporaryDirectory(dir=args.output_dir) as output:
        work = Path(work)
        output = Path(output)
        if windows:
            prefix = args.windows_prefix.resolve()
            flags, env, extra, objdump = windows_settings(prefix, args.windows_toolchain, work)
        if raspberry_pi:
            pi_flags, pi_env, emulator, loader = raspberry_pi_settings(args.raspberry_pi_sysroot.resolve())
        vendor = prepare_vendor(work, ROOT / "build/release-cache")
        if linux or windows:
            native_flags = [f"-DCMAKE_C_COMPILER={executable('clang')}",
                            f"-DCMAKE_CXX_COMPILER={executable('clang++')}"]
            native = configure_distill(vendor, work / "linux", native_flags, None, jobs)
            version = project_version(native)
        if linux:
            run("cmake", "--build", native, "--parallel", jobs)
            run("ctest", "--test-dir", native, "--output-on-failure")
            package_linux(native, vendor, output / f"distill-{version}-linux-x64.tar.gz")
        if raspberry_pi:
            target = configure_distill(vendor, work / "raspberry-pi", pi_flags, pi_env, jobs)
            version = project_version(target)
            run("cmake", "--build", target, "--parallel", jobs, env=pi_env)
            # QEMU user mode cannot report posix_spawn exec errors to the parent.
            run("ctest", "--test-dir", target, "--output-on-failure",
                "--exclude-regex", "^distill-media-transfer$")
            package_linux(target, vendor, output / f"distill-{version}-raspberry-pi-arm64.tar.gz",
                          emulator, loader)
        if windows:
            if not linux:
                run("cmake", "--build", native, "--parallel", jobs, "--target", "distill-jsc")
            target = configure_distill(vendor, work / "windows", flags, env, jobs,
                                       [*extra, f"-DDISTILL_HOST_JSC={native / 'distill-jsc'}"])
            run("cmake", "--build", target, "--parallel", jobs, env=env)
            run(sys.executable, ROOT / "tools/package_windows.py", "--build", target,
                "--prefix", prefix, "--vendor-dir", vendor, "--objdump", objdump,
                "--output", output / f"distill-{version}-windows-x64.zip",
                "--installer", output / f"distill-{version}-windows-x64-setup.exe",
                "--version", version)

        publish_packages(output, args.output_dir)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("all", "linux", "windows", "raspberry-pi"),
                        default="all")
    parser.add_argument("--raspberry-pi-sysroot", type=Path,
                        help="Debian Trixie ARM64 sysroot for Raspberry Pi OS")
    parser.add_argument("--windows-prefix", type=Path, help="MSYS2-style Windows x64 UCRT dependencies")
    parser.add_argument("--windows-toolchain", type=Path, help="LLVM-MinGW bin directory (defaults to PATH)")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/releases")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        parser.error("the release script requires an x86-64 Linux build host")
    if args.target in ("all", "windows") and args.windows_prefix is None:
        parser.error("--windows-prefix is required for the Windows build")
    if args.target == "raspberry-pi" and args.raspberry_pi_sysroot is None:
        parser.error("--raspberry-pi-sysroot is required for the Raspberry Pi build")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        release(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"distill release: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
