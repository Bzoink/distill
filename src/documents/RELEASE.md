# Building Distill releases

Run the release script from an x86-64 Linux checkout to build and package
Linux x64 and Windows x64.

```sh
python3 tools/release.py \
  --windows-toolchain /path/to/llvm-mingw/bin \
  --windows-prefix /path/to/ucrt64
```

Omit `--windows-toolchain` if the LLVM-MinGW tools are on `PATH`. Use
`--target linux` for Linux only or `--target windows` for Windows only.
`--output-dir` changes the destination and `--jobs` sets the build parallelism.

The first run downloads and verifies pinned QuickJS-ng, Lexbor, miniaudio,
termbox2 and Readability sources, and installs linkedom from an npm lockfile.
Downloads are cached under `build/release-cache`. The script builds QuickJS-ng
and Lexbor for each selected target and links them statically. A Windows-only
build also builds a native bytecode compiler.

## Build host

Use Python 3.12 or newer, CMake 3.25 or newer, Ninja, Clang with C23 support,
pkg-config, curl, Node.js 20.19 or newer, and npm. The releases are built on
Debian 13. Install the Linux development libraries with:

```sh
sudo apt install clang cmake ninja-build pkgconf curl nodejs npm \
  libcurl4-openssl-dev libunibreak-dev libpng-dev libjpeg-dev libwebp-dev \
  libfreetype-dev libharfbuzz-dev libgtk-3-dev libcairo2-dev libx11-dev libxext-dev
```

For Windows, install NSIS 3 (`sudo apt install nsis`) and use the x64 UCRT
LLVM-MinGW toolchain. The prefix can be a copy
of the `ucrt64` directory from MSYS2 after installing these packages and their
dependencies in the MSYS2 UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{curl,libunibreak,libpng,libjpeg-turbo,libwebp,freetype,harfbuzz}
```

Keep the prefix's headers, import libraries, pkg-config files, DLLs and
licenses. The Windows packager follows imports recursively and fails if
an application DLL is missing.

## Raspberry Pi

Build for 64-bit Raspberry Pi OS (Trixie) from the same x86-64 Linux host.
Install `gcc-aarch64-linux-gnu`, `lld`, and `qemu-user` on the host.
Use a Debian 13 ARM64 sysroot with the Linux development libraries listed
above, plus `libc6-dev` and `libstdc++-14-dev`. Preserve the sysroot's
`/lib` and `/usr` layout and its symlinks.

```sh
python3 tools/release.py --target raspberry-pi \
  --raspberry-pi-sysroot /path/to/arm64-sysroot
```

The script runs CTest and checks the packaged binaries' shared libraries
under QEMU with a Cortex-A53 CPU. The media-transfer test requires native
ARM64 or full-system emulation because QEMU user mode cannot pass
`posix_spawn` errors back to the parent process.

## Packages

Each package has a matching `.sha256` file in `build/releases`. The portable
archives contain a `Distill/` directory. Extract it and keep its `bin/` and
`share/` directories together.

- `distill-<version>-linux-x64.tar.gz` contains the command-line tool, GTK
  and terminal readers, framebuffer and X11 readers, and the experimental
  engine. The script runs CTest before packaging. The binaries use the
  build distribution's runtime libraries, including GTK 3 and X11.
  Run `Distill/bin/distill-gtk` from a graphical session.
  Install the `bin/` and `share/` contents under a standard prefix such as
  `~/.local` to register the Distill launcher and icon for your desktop.
- `distill-<version>-raspberry-pi-arm64.tar.gz` contains the same Linux
  readers for 64-bit Raspberry Pi OS (Trixie). Run `Distill/bin/distill-gtk`
  from the desktop or `Distill/bin/distill-tui` from a terminal. On
  Raspberry Pi OS Lite, install the runtime libraries first.

  ```sh
  sudo apt install libcurl4t64 libunibreak6 libfreetype6 libharfbuzz0b \
    libpng16-16t64 libjpeg62-turbo libwebp7
  ```

  The GTK reader also needs `libgtk-3-0t64` and a graphical session.
- `distill-<version>-windows-x64.zip` contains the command-line tool and
  native GDI reader with their DLLs. On Windows 10 or later, open
  `Distill/bin/distill-fb.exe`. See [the Windows guide](WINDOWS.md).
- `distill-<version>-windows-x64-setup.exe` installs the same Windows build
  for the current user in `%LOCALAPPDATA%\Programs\Distill` by default.
  It adds a Start menu shortcut and an entry in Installed apps.
