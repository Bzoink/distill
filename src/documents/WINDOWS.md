# Distill on Windows

On Windows 10 or later (x64), extract the portable ZIP and open `Distill/bin/distill-fb.exe`. The
`distill.exe` in the same directory is the command-line engine. Keep `bin`
and `share` together so the reader can find its fonts, site profiles and
welcome page. The reader opens PNG, JPEG and WebP images and plays MP3, WAV
and FLAC through WASAPI. For a list of engine options run
`distill.exe --help` from PowerShell.

## Build

Use a Windows x64 UCRT toolchain with C23 support, such as MSYS2 UCRT64 or
LLVM-MinGW. Use libraries built for that same Windows target. The build needs
libcurl, libunibreak, libpng, libjpeg, libwebp, FreeType and HarfBuzz. Build
the checked-out QuickJS-ng and Lexbor with CMake for the same target and put
their headers and libraries in the dependency prefix. A static QuickJS library
and a shared Lexbor build work together.

Once the dependency prefix is visible to CMake and pkg-config, configure and
build with:

```sh
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DEXTERNAL_PREFIX="$prefix"
cmake --build build/windows --target distill distill-fb
```

On a Linux cross-build, pass a Windows CMake toolchain and set
`DISTILL_HOST_JSC` to an executable built on the host from the same QuickJS
sources. Point pkg-config at the Windows dependency directory. MSYS2 package
metadata uses the `/ucrt64` prefix, so set `PKG_CONFIG_SYSROOT_DIR` to the
directory containing the extracted `ucrt64` tree.

Build the portable ZIP with:

```sh
python3 tools/package_windows.py --build build/windows --prefix "$prefix" \
  --output build/distill-windows-x64.zip
```

Pass `--prefix` more than once if the DLLs are installed into several prefixes.
The packager uses `llvm-objdump` to follow PE imports. Use `--objdump` to
select a different compatible executable.
