# Muroxi compatibility plan

Status: planned. Written 2026-09-18 against Distill `95a87cc` and Muroxi
`b5c796ae0`. The stack facts were updated for Muroxi `eb15e557d`, which
landed demand-grown stacks. The libc and curl facts were measured on
2026-09-18 by comparing symbols; see [What Distill uses, measured](#what-distill-uses-measured).
Updated 2026-09-19: M0 to M4 are done, M5's host side is built, and the
framebuffer shell (FRAMEBUFFER_PLAN.md) exists on Linux, so M7 is now only the
`libmxpresent` sink. Each phase below has its result.
It sits beside [PLAN.md](PLAN.md) and
[FRAMEBUFFER_PLAN.md](FRAMEBUFFER_PLAN.md). Where it changes something either
of them decided, it says so and why; see
[Changes to other plans](#changes-to-other-plans).

## Direction

Make Distill a program that is easy to port *in general*, and let Muroxi be
the second platform that proves it. Nothing in the core should know it is
running on Muroxi. Everything that does know lives in a handful of backend
files behind narrow interfaces, and the Muroxi build compiles those files
instead of the Linux ones.

Most of the work is removal. Most of what makes Distill hard to port today
is dependencies it does not need: litehtml (C++), Gumbo, Cairo, pixman,
fontconfig, GTK and GLib. What stays is plain C that Muroxi either already
builds or can build. *(2026-09-19: litehtml and Gumbo are gone (M4). Cairo is
linked only by the `--paint` reference and the GTK host's window (M3). GTK and
GLib remain only in `distill-gtk`. Distributions' HarfBuzz still brings GLib
in, and a static dependency profile removes that (M5).)*

**Muroxi can grow to meet Distill.** Muroxi is developed alongside Distill,
and its side has offered to add what Distill needs, curl features included.
That sets the split:
- **Muroxi adds it** when a gap is Muroxi being narrower than an ordinary
  POSIX system or an ordinary libcurl: a missing libc function, a curl feature
  compiled out, a limit lower than a small browser needs. Distill does not
  work around such gaps; it names them in
  [What Muroxi adds for Distill](#what-muroxi-adds-for-distill).
- **Distill changes** when the problem is Distill depending on something no
  small platform should have to provide: a C++ runtime, a dynamic loader, GTK,
  Cairo, or OS calls spread through the core. Those changes make Distill
  portable in general, not just to Muroxi.

## What Muroxi is, for this project

Muroxi is a native x86_64 operating system derived from MINIX 3. These are the
facts that constrain a port. Each was read from the Muroxi tree on the date
above. "Measured" means read from source or a frozen contract; nothing here
has been built for the target yet.

### Target and toolchain

- **Static ELF64 only.** The target triple is `x86_64-unknown-none-elf`,
  built with Muroxi's pinned Clang/LLD 22.1.8 inside Muroxi's container. The
  link must be static with no undefined symbols. There is no dynamic loader and
  no `dlopen`/`dlsym`.
- **The C library is `libmxc`,** not glibc or musl. It is a named subset
  (the POSIX tier contract, `config/contracts/posix-tier.toml`), and a header
  existing does not prove the function behind it exists.
- **There is no C++ runtime:** no C++ ABI, no exception or unwind support, no
  `libstdc++`/`libc++`. A C++ library either builds without the standard
  library (HarfBuzz can) or cannot be used (litehtml cannot).
- **Muroxi builds third-party code from pinned sources inside its own build
  script**, the way it builds NetSurf, curl and FreeType. It does not run
  CMake for ports. A plain list of sources and flags is what it can consume.

### Stack: 64 KiB guaranteed, grows on demand to its reservation

Muroxi stacks grow on demand (Muroxi `doc/stack-growth-plan.md`, frozen in
`posix-tier.toml [stack]`). The sizes come from `user_abi.h`:

| Stack | Reserved | Guaranteed | Grows |
|---|---|---|---|
| Main thread | 8 MiB (`MX_USER_STACK_RESERVE`) | 64 KiB (`MX_USER_STACK_COMMIT`) | on demand, to the reservation |
| pthread, default | 1 MiB (`MX_PTHREAD_STACK_RESERVE`); the lowest page is the guard | none | on demand, every page |
| pthread, maximum | 8 MiB − 4 KiB (`MX_PTHREAD_STACK_MAX`), set with `pthread_attr_setstacksize` | — | — |

What that means for Distill:

- **Only the first 64 KiB of the main stack is guaranteed.** Its memory is set
  aside when the program starts, so using it cannot fail.
- **Growth past 64 KiB can fail.** If the machine has no free page, the
  thread ends as a crash named `stack-growth-refused`. Nothing returns an
  error the program could handle.
- **Overflow is named but not catchable.** Going past the reservation ends
  the thread with `cause=stack-overflow` on the serial log and in a counter.
  The parent's wait status is still an ordinary crash (page fault, vector 14),
  so a shell cannot tell an overflow from any other crash.
- **Pointers far below the stack pointer are not grown.** Growth serves only
  faults from 128 bytes below the stack pointer upward, and never an
  instruction fetch. A stray pointer into the unused part of the reservation
  still crashes, as it would on Linux.
- **Kernel copies grow the stack too:** a `read` into a local buffer on a
  not-yet-grown page works. One case is not proved yet: a signal delivered
  while the stack pointer is in a not-yet-grown page. Distill's shells
  install signal handlers; their stacks are shallow when they do.
- **QuickJS's own check must fire first.** Distill called
  `JS_SetMaxStackSize(runtime, 8 MiB)`, the whole main-stack reservation, so
  the guard page was hit before QuickJS's check and the process died instead
  of throwing. M0 found this crashing on Linux too, and it is fixed: the limit
  is now the stack limit less 1 MiB. See
  [The engine as a library](#the-engine-as-a-library).
- QuickJS's parser, regexp engine and JSON code, and Readability's tree
  walks are all recursive. The deepest recursion depends on the page. That
  is engine code, which may go deeper than 64 KiB. The shells' layout,
  `reader_layout`, recurses only to 48 levels of nesting
  (`LAYOUT_DEPTH_MAX`). Deeper content is refused, not laid out.

### Memory, processes and files

| Limit | Value | Source |
|---|---|---|
| Processes, system-wide | 64 | `posix-tier.toml [limits]` |
| Descriptors per process | 32; the kernel `poll` path covers descriptors 0–15 (Muroxi extends it to all 32; see [What Muroxi adds](#what-muroxi-adds-for-distill)) | `posix-tier.toml`, `sysroot/src/poll.c` |
| Path length | 256 bytes | `posix-tier.toml` |
| Total `argv` | 4096 bytes | `posix-tier.toml` |
| `mmap` | anonymous private read/write only; no file-backed, shared or executable mappings | `posix-tier.toml [mmap]` |
| Anonymous maps per process | 128, each at most 8 MiB. `malloc` never returns memory to the kernel, so this caps a process's lifetime heap growth, not its working set. Each pthread stack uses one slot | `posix_runtime.h` (`MX_POSIX_MAX_MAPS`) |
| Threads per process | 4, counting the main thread | `thread.c` (`mx_thread_pthread_create`) |
| Memory per process | 16 MiB declared; not enforced on heap or stack today | `resource-pressure.toml [quotas.process]` |
| Swap | none | `posix-tier.toml` |

`/tmp` is volatile and `/home` persists across reboots.

### Processes and signals

- **Available:** `posix_spawn`, `fork`/`exec`, `waitpid`, pipes, `poll`,
  `termios`, `TIOCGWINSZ`, pthreads with futex-backed mutexes and condition
  variables.
- **Missing (measured):** `posix_spawnp`,
  `posix_spawn_file_actions_addopen` and `pipe2`. Since M1 they are called
  only in the Linux `dproc` backend
  ([proc.c](../platform/linux/proc.c)). Both shells and media transfers use
  them through it, and Muroxi's `dproc` backend needs them the same way.
  Muroxi adds them.
- **Signals:** only HUP, INT, QUIT, KILL, TERM, CHLD, TSTP, CONT, TTIN, TTOU
  and WINCH. There is no SIGPIPE, SIGSEGV, SIGBUS, SIGFPE, SIGILL or SIGABRT
  handling.
  - Both shells install their crash restore through `d_proc_on_fatal_signal`,
    which reports how many handlers took, and they run either way. On Muroxi
    that fails harmlessly for the missing signals.
  - The Linux console sink also needs SIGUSR1 and SIGUSR2 for VT switching,
    and refuses to start without them. That is Linux only: on Muroxi the
    console owns switching, and there is nothing to install.

### libc gaps that affect Distill's dependencies (measured)

- **Math.** `libmxc`'s math is a small set: `fabs floor ceil trunc modf fmod
  frexp ldexp sqrt log log2 log10 exp pow cbrt sin cos tan asin acos atan
  atan2`, plus a few `float` variants.
  - QuickJS-ng additionally calls `sinh cosh tanh asinh acosh atanh expm1 log1p
    hypot round lrint scalbn`. It will not link without them.
  - JavaScript's `Math` results must also be accurate. Whether the existing
    functions are accurate enough is unmeasured.
- **Other.** `tzset`, `strtold`, `<fenv.h>`, `sched_yield`, `usleep` and
  `open_memstream` are absent. QuickJS-ng's `quickjs.c` and `dtoa.c` do not
  need them. `quickjs-libc.c` (the `qjs` command's `std`/`os` modules) does
  need `dlopen` and must not be linked.
- The full measured list, and who needs each symbol, is in
  [What Distill uses, measured](#what-distill-uses-measured).

### Networking

- **curl is present** (8.22.0) with Mbed TLS, a pinned CA bundle, gzip,
  deflate and brotli decoding, and IPv4.
- **The frozen command profile is narrow.** `mx-curl-https-v1` describes
  `/bin/curl` as GET-only, HTTPS-only, HTTP/1.1, no cookies, no multi-transfer
  and no uploads.
- **The library is wider than that profile (measured).** The archive NetSurf
  links (`libcurl_https.a`) defines `curl_multi_*` (including
  `curl_multi_poll`), `curl_share_*` and the whole cookie engine
  (`Curl_cookie_*`). Its `curl_config.h` does not define
  `CURL_DISABLE_COOKIES`, `CURL_DISABLE_HTTP` or `HTTP_ONLY`, so plain `http`,
  `POST` and custom methods are compiled in.
- **One compiled-out feature matters: `CURL_DISABLE_PARSEDATE`.** Without
  date parsing, a cookie's `Expires=` attribute cannot be read, so persistent
  cookies either become session cookies or are mishandled. Distill's session
  file relies on expiry, so Muroxi turns date parsing back on.
- **HTTP/2 is not built** (no nghttp2). Distill asks for
  `CURL_HTTP_VERSION_2TLS`. curl refuses that option when it has no HTTP/2,
  and Distill ignores the refusal, so it runs on HTTP/1.1, which is correct.
  Distill should ask only when `curl_version_info` reports HTTP/2, so that
  the fallback is a decision rather than an accident.
- **Distill uses more curl than the command profile describes:** `curl_multi`,
  `CURLOPT_SHARE`, the cookie engine (`COOKIEFILE`, `COOKIEJAR`,
  `COOKIELIST`), `POST`/`POSTFIELDS`/`CUSTOMREQUEST`, and plain `http://` for
  the local corpus. The library has all of it; what is missing is a profile
  that allows a package to use it. Muroxi adds that profile.
- **Parallel transfers are limited.** Muroxi's network path "is not yet safe
  for larger parallel transfers" (Muroxi `doc/browser-on-console.md`). NetSurf
  ships capped at 2 fetches in total.
- **TLS refuses without synchronized time.** It returns
  `MX_CURL_ERR_UNTRUSTED_TIME` rather than connecting.

### Pixels: `libmxpresent`

The only way to get pixels on screen is Muroxi's presentation library.
Programs never map the framebuffer.

```c
int mx_present_client_open_for_provider(struct mx_present_client *client,
                                        uint32_t provider); /* CONSOLE or COMPOSITOR */
uint32_t *mx_present_client_buffer_pixels(const struct mx_present_client *client,
                                          uint32_t index);
int mx_present_client_submit(struct mx_present_client *client, uint32_t index,
                             const struct mx_present_rect *damage,
                             uint32_t damage_count);   /* at most 32 rects */
int mx_present_client_close(struct mx_present_client *client);
```

- **Two providers.** The *console* provider runs the program full-screen from
  the text console. The *compositor* provider gives it a desktop window. The
  same binary serves both.
- **Geometry** is reported as width, height, `stride_bytes` and the format
  (`XRGB8888` or `ARGB8888`). The stride must be honoured.
- **A session has a generation.** A changed generation means the
  presentation was taken away and handed back, and everything must be redrawn.
  That is exactly the plan's `D_EVENT_SUSPEND`/`D_EVENT_RESUME`.
- **There is no VT switching, `KD_GRAPHICS` or console restore to write.**
  Muroxi's console owns that.
- **How the framebuffer shell maps onto it** (2026-09-19):
  - `d_display_present` passes pixels, their stride and damage rectangles.
    The shell sends the whole screen as one rectangle each frame, within the
    32-rectangle limit; smaller damage is a later optimisation.
  - Every sink copies through `d_display_copy`, which honours both strides.
    The gate's memory sink pads its rows so a copy that does not is caught.
  - The shell already handles `D_EVENT_SUSPEND` and `D_EVENT_RESUME`, and the
    gate checks them.
- **Template:** NetSurf's Muroxi adapter is the working example, in the Muroxi
  tree:
  - `sysroot/netsurf/muroxi_surface.c` (551 lines): presentation, raw
    `termios`, mouse modes;
  - `sysroot/netsurf/muroxi_input.c` (335 lines): the input decoder.

### Input: one byte stream

- **Keyboard.** Keys arrive on standard input as the same ANSI byte stream a
  terminal delivers. The keymap is applied before Distill sees anything. This
  matches FRAMEBUFFER_PLAN.md's decision to read keys from the tty.
- **Mouse.** The mouse arrives in the same stream as SGR (1006) reports,
  after the program writes `ESC[?1003h ESC[?1006h`. The contract is
  `mx-console-mouse-model-v1`.
- **Pointer precision is one character cell** (coordinates are 1-based
  cells). NetSurf maps a cell to pixels using `TIOCGWINSZ`. The shell's hit
  targets therefore need to be at least a cell in size. Sub-cell precision
  would need a Muroxi change (see
  [What Muroxi adds for Distill](#what-muroxi-adds-for-distill)).
- **There is no evdev.** FRAMEBUFFER_PLAN.md's raw-evdev pointer code does
  not port. It lives only in the Linux console sink
  ([display_console.c](../platform/linux/display_console.c)).
- **The decoder exists** (2026-09-19): [input_decode.c](../input_decode.c)
  in `distill-shell`, shared by both shells. It decodes keys, the Linux
  console's function keys and SGR reports. It puts a cell click at the
  centre of the cell in pixels, from the cell size it is given. `check.py
  --fb` clicks and scrolls through it in cell coordinates, as Muroxi will.

### Audio

- `system.audio` is reached through `mx_audio_client.h`:
  `mx_sio_open`, `mx_sio_setpar(rate, channels, bits)`, `mx_sio_start`,
  `mx_sio_write`, `mx_sio_stop`, plus mixer volume.
- The service takes PCM at 44.1 or 48 kHz.
- Muroxi has its own WAV, MP3, FLAC and Opus decoders. Distill can keep
  miniaudio's decoders and send the PCM here.

### Terminal

- The console and the graphical terminal set `TERM` and speak ANSI with the
  mouse protocol above.
- There is no sixel or kitty graphics. The TUI's image fallback, alt text,
  is what will show.

### Packaging and licences

- Optional applications ship as their own package, outside the base image, as
  NetSurf does (`netsurf_muroxi.package.toml`: an entry path, providers,
  network and JS flags, and explicit budgets).
- Muroxi's licence rule excludes GPL code from the base image only. All of
  Distill's runtime dependencies are permissive, so none of that applies:
  - QuickJS-ng: MIT;
  - Lexbor: Apache-2.0;
  - libcurl: curl licence;
  - Mbed TLS: Apache-2.0;
  - FreeType: FTL;
  - HarfBuzz: MIT;
  - libunibreak: zlib;
  - libpng, libjpeg-turbo, libwebp: permissive;
  - miniaudio: MIT-0 or public domain;
  - termbox2: MIT;
  - linkedom: ISC;
  - Readability: Apache-2.0;
  - fonts: OFL or Bitstream Vera.

  Record each licence in the package anyway.

## The target shape

### Dependencies

| Dependency | Decision | Reason |
|---|---|---|
| QuickJS-ng | Keep, vendored; link `quickjs.c`, `libregexp.c`, `libunicode.c`, `dtoa.c` and `cutils.c` only | Portable C. Never link `quickjs-libc.c` |
| linkedom, domino, Readability | Keep; **compile to QuickJS bytecode at build time and embed** | No runtime JS paths; less work per navigation |
| Lexbor | Keep; build only the modules used | Portable C |
| libunibreak | Keep | Small, portable C |
| FreeType | Keep; **load faces from memory** (`FT_New_Memory_Face`) | Avoids relying on FreeType's file stream; NetSurf on Muroxi does the same |
| HarfBuzz | Keep, optional; build the single-file amalgamation with `-fno-exceptions -fno-rtti`, no standard library | Builds without a C++ runtime. Without it, text falls back to unshaped advances |
| libpng, libjpeg-turbo, libwebp, zlib | Keep behind [image.c](../image.c) | Muroxi has all four pinned |
| libcurl + Mbed TLS | Keep in the engine (not a `dfetch` seam; see the M1 result) | Muroxi has it. Mbed TLS on every platform gives one TLS behaviour everywhere; optional |
| miniaudio | Keep for **decoding only** (`MA_NO_DEVICE_IO`); output through the audio interface | Its device backends need `dlopen`. *Done in M1:* the core decodes only, and devices are in the Linux `daudio` backend |
| termbox2 | Keep for the TUI | Needs only `termios`, `poll` and ANSI. The framebuffer shell does not use it |
| litehtml, Gumbo | **Removed** (M4) | Replaced by `reader_layout`; see below |
| Cairo, pixman, fontconfig | **Removed** (M3) | Replaced by the software painter. Cairo survives only as the `--paint` reference and the GTK host's window surface |
| GTK, GLib | **Remove from everything but a desktop host window**, if one is kept | See [The desktop](#the-desktop) |

The result is all C, apart from HarfBuzz compiled as C++ with no C++ runtime.

### Our own layout in place of litehtml

litehtml only ever lays out Distill's own reader template, never a source
site's page. That template is a closed vocabulary:
- the reader walk's 7 block kinds ([reader_walk.h](../reader_walk.h));
- 3 inline styles, plus links and images;
- tables, cards and form controls;
- about 60 fixed CSS rules, generated in `reader_template`, now in
  [engine.c](../engine.c).

*(Done in M4, 2026-09-19; see the [M4 result](#m4-result-2026-09-19).
`reader_layout` still reads the template's CSS itself. The C style table
proposed below was not built. That parser has to read numbers without the
locale: see the locale rule in
[Portability rules for all code](#portability-rules-for-all-code).)*

Several of those rules exist only to work around litehtml:
- `<main>` lays out as an inline box, so the measure moved to `<body>`;
- controls are hidden and their padding stripped so GTK widgets can sit on
  litehtml's content box;
- RTL is written as mirrored rules because litehtml only reliably handles
  physical sides.

Most of the replacement already exists:
- [reader_cells.c](../reader_cells.c) is a pure function from the reader page
  and a width to a grid, with links, controls, anchors, images and tables
  recorded as it goes;
- [shaper.h](../shaper.h) already breaks lines through a `DTextMeasure`
  callback, so line breaking does not care about the unit.

The plan:

- **Make one layout for both units.** Turn `reader_cells` into
  `reader_layout`, which takes a measurer:
  - cells, using `cell_width.h`, as today;
  - pixels, using FreeType advances, shaped by HarfBuzz when present.
- **Output a box list:** text runs, fills, borders, images and controls.
  Link, control, anchor and focus geometry is recorded during layout, not
  queried back afterwards. The cell grid becomes the unit-1 case.
- **Replace the CSS with a C style table.** The template's `:root` variables
  and rules become a table of metrics and colours per theme and zoom. The HTML
  export generates its `<style>` from the same table, so the export and the
  renderer cannot disagree.
- **Handle direction logically.** RTL becomes inline direction in the layout
  rather than mirrored rules.
- **Keep it inside the guaranteed 64 KiB.** Layout runs in the long-lived
  shell, which holds history, forms and playback. On Muroxi, growth past
  64 KiB can be refused and would kill the shell with the user's state in it.
  Walk the tree with an explicit stack. The only recursion allowed is bounded
  by nesting depth with a stated limit, and hitting the limit is a refusal. No
  large local buffers.

What this changes for the shells:
- The terminal, the framebuffer and any desktop host get the same focus order
  and the same form geometry, because they come from one layout.
- Controls are drawn by us everywhere. This is the drawn-controls work
  FRAMEBUFFER_PLAN.md phase F already requires, now shared.

### The platform layer

The core is strict C23 with no OS calls: extraction, the reader walk, layout,
the painter, the shell model, form submission, settings and history. It
reaches the platform through these interfaces, one header each under
`src/platform/`:

| Interface | Covers | Linux backend | Muroxi backend |
|---|---|---|---|
| `dfetch` | requests, redirects, cookies, progress | libcurl | libcurl (Muroxi archive) |
| `dfs` | existence and permissions, directories, private temporary files, safe replacement, appends, listing | POSIX | POSIX subset |
| `dclock` | monotonic time, sleeping | `clock_gettime` | `clock_gettime` |
| `dproc` | children and their output, waiting on several handles, fatal signals, stack limit, peak RSS, hand-off to the system's URI handler | `posix_spawn` + pipes + `poll` | `posix_spawn` + pipes + `poll` |
| `dterm` | the controlling terminal: open, size in pixels, direct queries, restore from a signal handler | `/dev/tty` + termios | console tty + termios |
| `ddisplay` + input | FRAMEBUFFER_PLAN.md's `DDisplay` | memory buffer (core code), X11 window (`distill-fb-x11` only), fbdev + tty + evdev | memory buffer, `libmxpresent` console or compositor + stdin |
| `daudio` | PCM output | miniaudio device | `mx_sio_*` |
| `dthread` | a thread with a set stack size, for the in-process engine (added in M2) | `pthread_attr_setstacksize` | the same |

*(M1, M2 and the framebuffer shell, 2026-09-19.)* All but `dfetch` exist, in
`src/platform/*.h` with backends in `src/platform/linux/`:
- **`dterm` was added.** The terminal shell's own terminal handling (the
  crash restore, the pixel size, capability queries) was OS code the table
  had no row for.
- **`ddisplay` came with the framebuffer shell** (2026-09-19,
  FRAMEBUFFER_PLAN.md phase B). `src/platform/ddisplay.h` is chosen at run
  time; the memory sink and the blitter are core code, and Linux has the
  console (fbdev, tty, evdev) and, in `distill-fb-x11` only, a window. The
  ANSI and SGR decoder is `src/input_decode.c`, in `distill-shell`. Muroxi's
  backend is a `libmxpresent` sink beside them (M7).
- **`dfetch` is not a seam, and stays libcurl in the engine.** libcurl is
  what both platforms use, so a per-OS interface around it would have one
  implementation, twice. What M1 needed from it was the concurrency limit,
  which is engine code (`--max-fetches`). An interface over requests belongs
  with M2's engine library, where the in-process mode needs one anyway.

Rules for the platform layer:

- **Most backends are chosen at link time,** one `.c` file per platform and
  no function-pointer tables. Only the display needs a runtime choice, because
  tests use the memory buffer and real runs use the screen.
- **Nothing outside `src/platform/<os>/` includes an OS header** beyond
  standard C. `tools/platform_gate.py` checks includes and feature-test
  macros; the core is also built `-std=c23 -pedantic-errors` with no
  feature-test macro, so the compiler refuses any POSIX call there.
- **The input decoder is shared.** Decoding the ANSI and SGR byte stream into
  `DEvent` is portable core code. Linux on a VT and Muroxi feed it the same
  kind of bytes, so it is host-tested once and both use it.
- **Every limit is explicit and passed in:** fetch concurrency, JS heap,
  image pixels, decoded bytes and response size. The Muroxi build sets them
  from its package manifest. When a limit is exceeded, refuse and name the
  limit; never truncate or clamp.

### The engine as a library

- **Expose one entry point,** `d_engine_run(const DEngineRequest *,
  const DEngineCallbacks *)`.
- **The one-shot `distill` command becomes a thin wrapper** that parses
  options into a request and prints results.
- **Shells run the engine as a subprocess by default,** which keeps crash
  isolation and returns all memory on exit. An in-process mode, contained by
  QuickJS's memory limit and interrupt handler, serves platforms without
  process creation and makes the engine testable without spawning.
- **Pass the request in a file or on stdin, not on `argv`.** Muroxi's whole
  `argv` is 4096 bytes, and a long GET form query can exceed that on its own.
  `--request-data-file` already does this for bodies; extend it to the whole
  request.
- **Keep cookies in the session file, not `/tmp`.** The in-memory jar is
  loaded from and saved to the `--session-file` through `dfs`, rather than
  `mkstemp("/tmp/distill-cookies-XXXXXX")`.
- **Size JavaScript's stack from the platform.** *(Decided by M0, and
  already done on Linux.)* QuickJS's maximum stack is the real stack limit
  less a 1 MiB margin, capped at 8 MiB:
  - Linux: 7 MiB under the default 8 MiB limit;
  - Muroxi: 7 MiB of the 8 MiB main-stack reservation.

  The margin covers C frames QuickJS does not count: those above the point it
  measures from, and our own host functions. M0 measured the engine's deepest
  native stack on a real page at 345 KiB, so 1 MiB is about three times that.
  This replaces the 4 MiB first proposed here, which would have halved the
  recursion depth pages get (one JS call costs about 1.8 KiB) for no measured
  benefit. A page that recurses deeper gets a catchable RangeError instead of
  a page fault (`43-deep-recursion`). If the engine ever runs
  JavaScript on a pthread, it sets that thread's stack with
  `pthread_attr_setstacksize` (at most 8 MiB − 4 KiB) and sizes the JS limit
  from it.
- **Accept that deep pages can cost the engine, not the shell.** Past the
  guaranteed 64 KiB, a refused growth kills the engine process. Because the
  engine is a separate process, the shell survives and reports a failed
  navigation; it cannot tell an overflow from any other crash.
- **Report time failures separately.** "The system time is not trusted"
  (Muroxi's `MX_CURL_ERR_UNTRUSTED_TIME`) gets its own exit status and
  message, not the general failure status. The user can fix it; a network
  failure they cannot.

### Data files and paths

- **No shipped binary refers to `DISTILL_SOURCE_DIR`.** Data comes from a
  data directory: set at build time, overridable by an environment variable,
  and resolved through `dfs`.
- **JavaScript is embedded as bytecode.** Fonts, profiles and the welcome page
  are installed under `<datadir>/distill/`.
- **Every path fits in 256 bytes,** and a longer one is refused with a clear
  error.

### The desktop

Once the painter and layout are ours, the framebuffer shell runs in a desktop
window as easily as on a console. *(2026-09-19: it does. `distill-fb-x11` is
the same shell in an X11 window, with input methods through XIM. It is
built for development, not shipped, and has no clipboard, file chooser or
accessibility.)* The GTK shell then becomes either:

- **retired:** the framebuffer shell in an X11 or Wayland window; or
- **a thin host:** a GTK window whose drawing area is one more display
  backend. It keeps what GTK really provides: the file chooser, clipboard,
  input methods and accessibility.

Accessibility is the real loss of going fully custom, since a raw pixel buffer
gives screen readers nothing. Choose deliberately; this plan does not assume
either way.

## Portability rules for all code

- **Language:** `-std=c23 -pedantic-errors`. No GNU extensions, no
  variable-length arrays, no `alloca`.
- **Types:** fixed-width types wherever a size crosses an interface; `size_t`
  for sizes, with checked multiplication.
- **No locale-dependent calls.** `wcwidth` is already avoided; keep it that
  way. This includes `strtod`/`strtof` and `<ctype.h>`: use `d_strtod`,
  `d_isspace` and the other helpers in `cutil.h`.
  - Linux can hide this, because the engine never calls `setlocale`. GTK
    does, and so does the X11 sink (for `LC_CTYPE`). Under a comma-decimal
    locale such as sv_SE, `strtof` dropped every fractional CSS length in
    the GTK window (fixed 2026-09-19, with a test run under that locale).
  - Muroxi has no locales, so it hides the defect entirely.
- **Stack:** no local buffer over 4 KiB. Large or long-lived buffers go on
  the heap or are `static`. No recursion without a stated depth bound. The
  shells stay within 64 KiB of stack; only the engine may go deeper.
- **Signals:** installing a signal handler may fail, and the program must
  carry on without it.
- **Static-safe:** no `dlopen`, no dependence on NSS, `iconv` modules or
  runtime plugins.
- **Descriptors:** at most 32 open at once, Muroxi's per-process table.
  Network sockets are bounded by `dfetch`'s concurrency limit, not by chance.

## Gates that model Muroxi on Linux

Each gate below runs on this machine and catches a whole class of Muroxi
failure before anything is built for Muroxi. Each must be shown to fail at
least once, against a deliberately broken build, before it is trusted.

- **`--stack`.** Two runs over the corpus, modelling Muroxi's guarantee and
  its reservation:
  - the shells under `ulimit -s 64`, the stack Muroxi guarantees;
  - the engine under `ulimit -s 8192`, Muroxi's main-stack reservation, with
    the Muroxi JS stack constant.

  The negative control is a fixture with deliberately deep script recursion,
  run with the JS limit raised to the whole 8 MiB: it must crash instead of
  throwing.

  *Built in M2.* Since 2026-09-19 it runs both shells' suites, the terminal's
  and the framebuffer's, in 64 KiB, and both must fail in 16 KiB. The limit
  is set with `setrlimit` on the soft limit only. `ulimit -s` in a shell also
  lowers the hard limit, so the engines the shell starts could not get their
  8 MiB back.
- **`--static-link`.** Link a fully static build against musl with
  `-Wl,--no-undefined` and no `dlopen`. This models "static, not glibc, no
  loader". *Built in M5 (host side), with `--symbols`. Neither can pass yet:
  there is no static dependency profile to link against.*
- **`--symbols`.** Compare `nm -u` of the static link's Distill objects
  against a checked-in list of `libmxc.a`'s exported symbols, regenerated from
  Muroxi with `llvm-nm`. Any symbol outside the list is a named porting task,
  found on Linux.
- **`--budget`.** Record peak memory, JS heap, fetch count and startup time for
  each corpus page. Fail when a page exceeds the budget the Muroxi package will
  declare. *Not built as a separate gate.* Pages declare `max_rss_mib`,
  `max_run_ms` and `max_fetches_at_once` in the manifest, and `--extract`
  checks them. `tools/measure_budgets.py` is the measurement, and it is a
  diagnostic.
- **`--fb`** (FRAMEBUFFER_PLAN.md phase B), driven by a script of ANSI and SGR
  bytes: the byte stream the Muroxi console produces, cell coordinates
  included. *Built 2026-09-19* (`corpus/fb_suite.py`); its stride check is
  shown to fail `distill-fb-defect`, and `--stack` runs it in 64 KiB.

## Delivery phases

Each phase keeps the Linux build working and gated. Phases M1 to M5 need no
Muroxi machine.

### M0 — measure

Run the corpus and record, per page:
- peak memory and JS heap;
- peak native stack of the engine and of each shell, to compare with
  Muroxi's guaranteed 64 KiB and its 8 MiB reservation;
- fetch count and the most fetches at once;
- engine startup time with JS parsed from source.

**Done when** those numbers are in a table in this document and the budgets
for the Muroxi package are chosen from them.

#### M0 result (2026-09-19)

Done. Measured on Linux against Distill with the fixes below applied.

**How it was measured.**
- The engine and the TUI write one line of statistics when `DISTILL_STATS`
  names a file ([measure.c](../measure.c)). Off by default, so ordinary runs
  are unchanged.
- [tools/measure_budgets.py](../../tools/measure_budgets.py) runs each page
  the way the TUI does (`--template`, 5 s settle cap), twice:
  - once for memory, time and fetches;
  - once for stack, with `DISTILL_STACK_PROBE` painting 7 MiB of the stack,
    since painting inflates RSS.

  It also runs the whole TUI suite with the probe on.
- **Peak RSS** is the address space's own high-water mark (`VmHWM`), not
  `getrusage`: Linux carries `ru_maxrss` across `execve`, so every engine at
  first reported the Python harness's 30 MiB.
- **JS heap** is counted by a sized allocator handed to QuickJS. That
  allocator is also a Muroxi fix: QuickJS-ng sizes its large allocations with
  `malloc_usable_size`, which `libmxc` lacks, so on Muroxi they would count as
  zero against the memory limit.
- **The stack probe** was shown correct before use. Against recursion of a
  known depth it reports 264,160 bytes for 262,144 bytes of frames.
  `distill-measure-test` fails against two deliberately broken probes.

**Sources.**
- The 47 local fixtures. They are small by design and set no budget.
- The 44 live sites of `corpus/site-comparison.json` and the manifest's live
  rungs. These are a diagnostic, never a gate. 33 were usable content runs;
  challenges, and four sites throttled in the final run, are excluded.
- Raw rows are kept under `reports/m0/`, which is not tracked.

| Measure | Local fixtures, max | Live, median | Live, 90th percentile | Live, max |
|---|---|---|---|---|
| Engine peak RSS | 46 MiB | 44 MiB | 69 MiB | 151 MiB (Chart.js gallery) |
| JS heap peak | 24 MiB | 20 MiB | 42 MiB | 129 MiB (Chart.js), 86 MiB (YouTube) |
| Engine native stack | 61 KiB* | 104 KiB | 171 KiB | 274 KiB (React); 345 KiB in an earlier run (YouTube) |
| Fetches per navigation | 5 | 7 | 54 | 125 (X, stopped by the cap) |
| Fetches at once | 1 | 1 | 6 | 28 (React); 46 in an earlier run (Product Hunt) |
| Engine run time | — | 1.4 s | — | 5.0 s (settle cap) |

\* Excluding `43-deep-recursion`, which exists to reach the JS stack limit.

**The terminal shell** peaked at 20 KiB of stack and 7.4 MiB RSS across the
TUI suite's 10 shell runs.

**The framebuffer shell** was measured on 2026-09-19, once it existed. It
used the same `DISTILL_STATS` line, and the memory display ran a script
over `09-article` and `15-gallery`.

| Screen | Reading | Image viewer open |
|---|---|---|
| no page, 64×64 (the process floor) | 8.6 MiB | — |
| 1280×800 | 23 MiB | 28 MiB |
| 1920×1080 | 35 MiB | 43 MiB |

- Its stack peaked at 40 KiB at both sizes. `--stack` runs the whole
  framebuffer suite in 64 KiB.
- Memory grows with the screen, because the shell holds full-screen pixel
  buffers:
  - the composited screen;
  - the page layer, cached so scrolling does not relayout;
  - the viewer layer, while the viewer is open;
  - the memory display's own device buffer, which on Muroxi is
    `libmxpresent`'s.
- The floor includes Linux's shared libraries, among them GLib, which
  arrives through the distribution's HarfBuzz.

**Startup with JavaScript parsed from source:**
- compiling linkedom takes 6–7 ms per navigation;
- compiling Readability takes 2–5 ms, and it happens twice on 6 of the 44
  sites;
- the whole runtime phase (runtime, linkedom and the page's parse) has a
  median of 87 ms.

So embedding bytecode saves about 10 ms per navigation on this machine. The
case for it is packaging (no runtime JS paths), not speed.

**What the measurements found in Distill, now fixed.** Each fix has a fixture
that fails against the previous engine:

| Defect | Evidence | Fixture |
|---|---|---|
| Extraction copied and re-parsed every `<style>` element for every element, and never returned Lexbor's text copies | BBC 170 → 51 MiB, DN 137 → 32 MiB, W3Schools 144 → 49 MiB; extraction about twice as fast; output byte-identical | `42-stylesheet-scale`: 339 → 45 MiB (`max_rss_mib`) |
| The JS stack limit was the whole 8 MiB stack, so recursion past about 4,600 calls killed the engine with SIGSEGV | one JS call level costs about 1.8 KiB of native stack | `43-deep-recursion`: SIGSEGV → a RangeError the page catches |
| Blocking scripts fetched before the settle loop each got the whole cap as their timeout, even after the cap had passed | X: 14 s → 5.0 s | `44-blocking-script-chain`: 15.0 → 5.0 s (`max_run_ms`) |
| HTTP/2 was requested from any libcurl | Muroxi's has none | none possible on Linux; the option is now set only when curl reports HTTP/2 |

**Budgets for the Muroxi package.** A later decision can raise these; each
is a named refusal when exceeded, never a truncation.

| Budget | Value | Reasoning |
|---|---|---|
| Engine JS heap (`JS_SetMemoryLimit`) | 64 MiB | Covers 31 of the 33 usable live pages. The two above it are YouTube (86 MiB) and the Chart.js gallery (129 MiB, already at today's 128 MiB limit), and both are refused with the limit named |
| Engine process memory (package quota) | 96 MiB | The largest RSS of a page within the heap budget is 73 MiB (Target). The Linux process floor is 17 MiB of mostly shared libraries, which a static Muroxi binary does not map the same way, so the margin is deliberate |
| Terminal shell memory | 16 MiB | Measured 7.4 MiB. Fits Muroxi's declared 16 MiB default quota as it is |
| Framebuffer shell memory (package quota) | 48 MiB, for screens up to 1920×1080 | Measured 43 MiB at 1920×1080 with the viewer open, and 28 MiB at 1280×800. Muroxi's 16 MiB default does not fit even the process floor plus one 1080p frame. Proposed 2026-09-19; M7 re-measures on the target |
| Shell stack | within the guaranteed 64 KiB | Measured 20 KiB (terminal) and 40 KiB (framebuffer) |
| Engine stack | Muroxi's 8 MiB main-stack reservation, unchanged | The deepest real page used 345 KiB, so the per-package reservation item is not needed |
| JS stack limit | the stack limit less 1 MiB: 7 MiB on Muroxi | See [The engine as a library](#the-engine-as-a-library) |
| Fetches at once | Muroxi's stated cap (at least 4 asked), passed as `--max-fetches` | Median 1, 90th percentile 6, max 46 (figures above 1 may be one low; see the M1 result). The engine queues above the cap since M1 |

**Left open by M0:**
- **Real-page stack depth varies between runs.** YouTube read 345 KiB once
  and 66 KiB the next time. Budgets use the larger reading.
- **Every number here is from Linux.** Time on Muroxi under QEMU will be
  longer. Memory will differ by the libraries a static link leaves out.
  M6 re-measures on the target.

### M1 — the platform seams

Add the `src/platform/` interfaces and Linux backends. Move every OS call
behind them. Install signal handlers failure-tolerantly. Add the
include-grep gate.

**Done when** `check.py`, `--extract`, `--render`, `--tui` and `ctest` pass
unchanged, and the grep finds no OS header outside `src/platform/`.

#### M1 result (2026-09-19)

Done. Every gate passes:
- `check.py`: DOM, `--extract`, `--render`, `--viewer`, `--tui` (42 checks)
  and `--app`;
- `ctest`: 12 tests, the gate and its self-test among them;
- `distill-gtk --self-test`.

**The layer.** Five interfaces in `src/platform/`: `dclock`, `dfs`, `dproc`,
`dterm` and `daudio`. Their Linux backends are in `src/platform/linux/`. See
[The platform layer](#the-platform-layer) for what each covers and why
`ddisplay` and `dfetch` are not among them yet.

**What moved behind it:**
- **The engine:** its clock and sleep, the session file's privacy check,
  the cookie jar's temporary file, the stack limit, path resolution and the
  profile directory scan.
- **The terminal shell:**
  - spawning engines, reading their output and the poll loop;
  - hand-off to `xdg-open`, now `d_proc_open_with_system`, which returns
    nothing where there is no desktop session;
  - its private run directory and temporary files;
  - its terminal, including the crash restore. That is installed
    failure-tolerantly: `d_proc_on_fatal_signal` reports how many handlers
    took, and the shell runs either way.
- **Everything else:** media transfers, settings (saved with
  `d_fs_replace`), the renderer's font scan and the measurement code.
- **Audio.** miniaudio's implementation and its devices moved into the Linux
  audio backend. The audio core now only decodes, and hands PCM to `daudio`.

**The GTK shell** moved to `src/platform/linux/app.c`. GLib is its
platform: it spawns, watches and handles files through GLib. So it is Linux
code rather than portable code converted to our interfaces, which fits the
open decision about whether it survives. It is the one target built without
`-pedantic-errors`, because GLib's own idioms are not ISO C.

**The gate.** It is shown to work in two ways:
- `tools/platform_gate.py` finds no OS header or feature-test macro outside
  `src/platform/<os>/`, where it finds 52 in the committed pre-M1 tree;
- its `--self-test` catches six planted violations and passes a clean file.

Strict ISO C found what the grep could not:
- a `getcwd` hidden in URL resolution;
- `strcasecmp`, which is POSIX; it is now an ASCII `d_casecmp` in `cutil.h`;
- a 22 KB string literal. ISO C promises 4,095 characters, so the host shim
  is now one literal per line, and was shown byte-identical after the split.

**The fetch limit.** The engine attaches at most `--max-fetches` transfers
at once (8 by default; a Muroxi package passes its stated cap) and queues
the rest in order. A blocking script or module fetch holds a slot too: it
waits for one, keeping the network moving, rather than opening one more
connection than the limit. Product Hunt measured 9 at once until it did.

Fixture `45-fetch-concurrency` covers both: twelve requests at once, then a
blocking script, all of which must be answered, with at most 8 attached. The
old engine attached 12; an engine without the blocking-fetch wait attached
9.

**Correction to M0.** The at-once counter did not count the request being
attached when it was new, so M0's figures above 1 may be one low: the
maximum is 46 or 47, not 46. The conclusion stands.

### M2 — the engine as a library

`d_engine_run`, the thin command wrapper, requests by file or stdin, the
cookie jar in the session file, embedded bytecode JavaScript, the JS stack
from the platform, and a distinct untrusted-time status.

**Done when** the golden gates pass through both the subprocess and the
in-process modes; the tree has no runtime JS path; and `--stack` passes the
corpus or refuses named pages with QuickJS's stack error. The in-process mode
runs the engine on its own pthread with a stack set by
`pthread_attr_setstacksize`, never on the shell's main stack.

#### M2 result (2026-09-19)

Done. Every gate passes in both modes:
- `check.py` and `check.py --extract`, through the command;
- `check.py --in-process` and `check.py --extract --in-process`, with the
  whole corpus in one `distill-host` process, each navigation on its own
  thread. Only the three pages with budgets differ: they report "pending",
  since a budget measures a process;
- `--render`, `--viewer`, `--tui` (42 checks), `--app`, `ctest` and the
  platform gate.

**The library.**
- `src/engine.h`: `DEngineRequest` (every option, with every limit
  explicit), callbacks for progress, location, output and diagnosis, and
  `d_engine_run`. The engine's 51 writes to stderr and stdout go through the
  callbacks.
- `src/distill.c` is the command: parse, run, print. Its stdout protocol is
  unchanged, so the shells needed no change for it.
- `src/engine_args.c` parses options once, from argv and request files alike.

**Requests by file.** `--request FILE|-` holds one option per line.
- The TUI's navigations and the shared media transfers write a private
  request file and pass only its path, so nothing of a request reaches argv.
- A value containing a newline is refused, not mangled.
- The GTK host, which is Linux only, still uses argv.

**In-process.** `src/engine_host.h` runs one request on a new thread. That
thread's stack is 8 MiB − 4 KiB, Muroxi's thread maximum, set through the new
`dthread` platform interface (`pthread_attr_setstacksize` on Linux; C11 threads
cannot set a stack size). The JS stack limit is derived from the thread's
stack. The deep-recursion page gets its RangeError inside the thread, and
repeated runs in one process give identical output. No shell uses in-process
mode yet: Muroxi can start processes, so the shells keep their isolation.

**Cookies.**
- Every run uses libcurl's in-memory cookie engine.
- A session file is checked private, loaded at the start and replaced whole
  with `d_fs_replace` at the end.
- No run creates a temporary jar. `check.py` now checks that an engine
  writes no files of its own during a run; the old engine fails that check
  with its `distill-cookies-*` file in `/tmp`.

**Embedded JavaScript and the data directory.**
- linkedom, JSDOMParser and Readability are compiled to QuickJS bytecode at
  build time by `tools/distill_jsc.c`. That tool links the engine's own
  libqjs and records logical file names, not paths on the build machine.
- Tracing a run confirms no `.js` file is opened.
- A local article's run fell from 14 ms to 6–7 ms: linkedom loads in 1 ms
  instead of compiling in 6.
- Fonts, profiles and the welcome page come from the data directory
  (`src/datadir.h`), which refuses paths longer than the platform allows.
  Fonts are now installed.
- No binary contains a source-tree path except the runtime library search
  path, which M5's static link removes.
- Neither shell compiles in the engine's path any more: both look beside
  their own executable.

**The clock.**
- `d_clock_wall_trusted` is new in `dclock`. On Linux it only knows that a
  clock set before 2026 is wrong; Muroxi's backend reports its own time
  quality.
- A TLS refusal while the clock is untrusted exits 5 and says to set the
  time. That includes a guessed `https://`, which is no longer stepped down
  to plaintext to get round it; the old engine did step down and reported
  success.
- The transport checks cover both cases through the
  `DISTILL_TEST_UNTRUSTED_TIME` test seam.

**`--stack`.** It models Muroxi's stacks on Linux:
- all 47 gated pages extract with the engine in an 8 MiB stack;
- the terminal suite's 42 checks pass with the shell in 64 KiB, each engine
  given 8 MiB of its own.

Both negative controls fail as they must:
- the deep-recursion page crashes with the JS limit set to the whole stack;
- the shell does not start in 16 KiB.

The pre-M0 engine fails the gate on `43-deep-recursion`.

### M3 — the software painter

FRAMEBUFFER_PLAN.md phase A, unchanged, including its `--paint` parity gate.

**Done when** that phase's condition holds and Cairo is no longer linked by
any target but a GTK host.

#### M3 result (2026-09-19)

Done.

**The painter.**
- `src/paint.h` is the interface render.c draws through. render.c keeps
  fonts, shaping and measurement and no longer mentions Cairo.
- `src/paint_soft.c` implements it in portable C, over a buffer of
  premultiplied 32-bit pixels:
  - "over" compositing with pixman's rounding;
  - glyph masks placed at fractional positions by bilinear sampling, as
    Cairo samples a translated mask;
  - exact area coverage for text decoration;
  - images box-filtered when shrunk and bilinear otherwise, transparent
    outside the source (Cairo's GOOD filter);
  - a 4×4-sampled disc, a clip stack, and PNG through libpng.
- It keeps within the portability rules: no local buffer over 4 KiB and no
  recursion.
- `d_render_surface` became `d_render_pixels`, which gives the pixels with
  their stride, so nothing outside render.c sees a Cairo type.

**Who links Cairo.**
- The engine, the TUI and `distill-host` link no Cairo (checked with `ldd`).
- The GTK host paints with the software painter too, and wraps its pixels in
  a Cairo surface only to put them in its window. A screenshot on Xvfb shows
  the article drawn correctly.
- Cairo's painter survives only as `tests/paint_cairo.c`, the reference.

**The gate, `check.py --paint`.**
- The painter is chosen at link time. One tool source
  (`tests/paint_tool.c`) builds three render tools: the software painter,
  the Cairo reference, and a software painter with a deliberate defect
  (glyphs a row low).
- The gate paints the reader page of all 47 gated corpus pages, and
  `corpus/paint/primitives.html`, with the first two, and compares the
  pixels. The primitives page has every painter operation, including images
  shrunk 4× and enlarged 2×, translucent fills and borders, clipping, both
  list markers and both decorations.
- The limits are a mean difference of 0.5 per channel and a largest
  connected region of 16 pixels differing by more than 32 in any channel.
- Measured: a worst mean of 0.051 and no differing pixel on any page.
- The defect painter fails on both control pages (mean 6.70 and 1.75;
  regions of 115 and 96 pixels).

### M4 — our own layout

`reader_layout` with cell and pixel measurers, the style table, the box list
and logical direction. While it is built, compare its pixel output with the
reference layout for every page in the render gate.

**Done when** the layout comparison stays within a stated threshold for the
render corpus, has been shown to fail against a deliberately broken layout,
and `--tui` passes on the cell measurer. Then delete litehtml, Gumbo and the
C++ adapter.

#### M4 result (2026-09-19)

Done. `reader_layout` is now the production pixel measurer and the TUI keeps
using the same page model through its cell measurer. The engine, GTK host and
headless render tools all link the C layout and software painter; the old
litehtml/Gumbo/C++ adapter path is no longer built or shipped.

The temporary layout parity gate compared all 48 render-corpus pages against
the reference layout at the stated limit of 0.5 mean difference per channel
and 16 pixels in the largest differing region. It passed every page: the
worst mean was 0.363 per channel and the worst differing region was 0 pixels.
Both deliberately broken-layout controls failed as required: `09-article`
had mean 6.707 and region 122, while `paint/primitives.html` had mean 3.330
and region 1470.

The permanent software-painter gate still passes all 48 pages, with worst
mean 0.041 per channel and no differing region; both painter negative controls
fail. The 42-check `--tui` gate passes, as do the full default, `--render` and
`--extract` corpus gates. CTest passes all 12 tests, including the layout
geometry contract.

### M5 — portable build

A plain per-library source manifest (sources, defines, include directories)
that CMake reads and Muroxi's build can read too, the `--static-link` and
`--symbols` gates, and the data-directory rules.

**Done when** the static musl build passes the corpus and `--symbols` reports
only the gaps listed in
[What Muroxi adds for Distill](#what-muroxi-adds-for-distill). The measured
list in [What Distill uses, measured](#what-distill-uses-measured) is the
gate's first expected output; the Distill-side rows in it are gone by then.

#### M5 slice result (2026-09-19)

The host-side portability slice is implemented:

- `portability/sources.manifest` contains the 19 production targets and is
  consumed by CMake and `tools/portable_manifest.py`; Muroxi can consume the
  same plain format.
- `DISTILL_STATIC_LINK=ON` selects static archives and adds a no-undefined
  static link, with no shared-library fallback.
- `tools/portable_gate.py` and `corpus/check.py` provide the `--static-link`
  and `--symbols` gates, using the measured Muroxi symbol allowlist.
- `tools/data_gate.py` checks the staged or installed data directory required
  by the portable package.
- The normal build and all 16 CTest tests pass, including the manifest and
  data self-tests.

The final static-musl corpus run is still pending: this Linux checkout does
not have static archives for libcurl and the remaining font, image and text
dependencies. The ordinary `build/distill` artifact is intentionally rejected
by the static gate as dynamically linked. Completing M5 requires the static
dependency profile supplied by the Muroxi build.

*(2026-09-19.)* The framebuffer shell brought:
- a 20th manifest target, `distill-fb`. Its Linux display backends come in
  through `src/platform/{platform}/`;
- a 17th CTest test, `distill-shell-input`.

Its objects were measured against `libmxc`. The only new need is four
math functions, listed in
[What Muroxi adds for Distill](#what-muroxi-adds-for-distill).

The same static profile also closes FRAMEBUFFER_PLAN.md phase H. Today GLib
reaches `distill-fb` only through the distribution's HarfBuzz, which Arch's
FreeType links too. A static HarfBuzz from the amalgamation, with FreeType,
libpng, zlib, libjpeg, libwebp and libunibreak, removes it.

### M6 — the terminal on Muroxi

The Muroxi backends for `dfs`, `dclock`, `dproc` and `dfetch`. Ports of
QuickJS-ng (core only), Lexbor and libunibreak into Muroxi's build. The
`distill` engine and `distill-tui`, packaged.

**Done when** on Muroxi under QEMU a corpus article loads over the network
from the local corpus server and reads by keyboard in the TUI, and a form
submits. This is gated by a Muroxi scenario, not by hand.

### M7 — the framebuffer on Muroxi

*The shell exists (FRAMEBUFFER_PLAN.md, 2026-09-19); what is left here is the
sink.* `src/platform/muroxi/display.c` and a `libmxpresent` sink implementing
`display_sink.h`'s table, with the keyboard and SGR pointer on standard input
through `src/input_decode.c`, as NetSurf's adapter does. The memory sink and
`check.py --fb` need nothing from Muroxi.

The `libmxpresent` display backend for both providers, and generation changes
mapped to suspend and resume. The shell already handles those events, and the
decoder already converts cells to pixels. The sink passes the cell size from
`TIOCGWINSZ`, as the memory sink's `cell=` option does in the gate. The
package declares the framebuffer shell's memory budget (see the M0 result).

**Done when** on Muroxi a corpus article reads by keyboard and by mouse
through the console provider and the compositor provider. The screen is
checked by pixels in the Muroxi scenario, not by the log. Taking the
presentation away and back redraws the whole page.

### M8 — media

The Muroxi `daudio` backend over `mx_sio_*`, with miniaudio decoding only.

**Done when** a corpus page's audio plays on Muroxi.

## What Muroxi adds for Distill

These are Muroxi changes. They belong in the Muroxi repository and its plans,
not in Distill patches. Each was found by measurement (see the next section),
not guessed. "Required" means the Distill package does not link or does not
work without it.

### Required for M6 (the terminal on Muroxi)

| Item | Needed by | Note |
|---|---|---|
| `sinh cosh tanh asinh acosh atanh expm1 log1p hypot round lrint scalbn` in `libmxc`, with accuracy tests | QuickJS-ng (`Math`, number formatting) | The msun sources in the imported NetBSD tree are candidates. Check the accuracy of the existing functions too: JavaScript's `Math` results are visible to pages |
| `pthread_cond_timedwait` and `pthread_condattr_setclock` (at least `CLOCK_MONOTONIC`) | QuickJS-ng's `cutils.h` (`Atomics.wait`) | Condition variables are already futex-backed, so this is the timed wait on top. The alternative is building QuickJS with `JS_HAVE_THREADS` 0, which is a Distill-side change for a POSIX gap |
| `posix_spawnp`, `posix_spawn_file_actions_addopen` | the `dproc` backend ([proc.c](../platform/linux/proc.c) on Linux), used by both shells and media transfers | Launching helpers and redirecting their output to a file |
| `pipe2` with `O_CLOEXEC` and `O_NONBLOCK` | the `dproc` backend, as above | The engine's progress pipe must not leak into other children |
| `realpath` | the `dfs` backend ([fs.c](../platform/linux/fs.c) on Linux) | Resolving a local file argument to a `file:` URL |
| `strerror_r` (XSI form) | termbox2 | Terminal error messages |
| `pthread_attr_setstacksize` in the shipped `libmxc.a` | M2's in-process engine | In source at Muroxi `eb15e557d`; the `artifacts/iso/libmxc.a` measured here predates it |
| `poll` over all 32 descriptors, not only 0–15 | every Distill process | The engine holds stdio, the session file and a socket per transfer; the shell holds stdio, the engine pipes, transfers and audio. 16 is too tight to hold as a rule. Extending `poll` removes a limit that is lower than the descriptor table itself |
| **A Distill curl profile** for `libcurl_https.a` consumers: plain `http` as well as `https`; `GET`, `POST` and custom methods; request bodies; the cookie engine with **date parsing enabled** (drop `CURL_DISABLE_PARSEDATE`); `curl_multi` and share handles; redirects up to 10; `CURLOPT_XFERINFOFUNCTION` progress | the engine, every navigation | The archive already has everything except date parsing. The work is the profile contract and a gate that the Distill package may use it, beside the frozen `mx-curl-https-v1` command profile |
| **Parallel transfers made safe, to a stated number** — at least 4 at once, 6 preferred | the engine | M0: the median page has 1 transfer at a time and the 90th percentile 6; the most seen was 46 (Product Hunt). NetSurf's 2 is enough for NetSurf because it runs less JavaScript. Distill honours whatever number is stated through `dfetch`. If 4 is not reachable yet, state the number and Distill caps to it |
| A package manifest and a QEMU scenario for Distill; every Distill source in `product_source_paths()` | M6, M7 | Otherwise a green Muroxi run can score an image that does not contain the change |

### Required later

| Item | Needed by | Note |
|---|---|---|
| `fmaxf fminf lround nearbyintf` in `libmxc` | `reader_layout.c`, `paint_soft.c` (M3, M4) | Found measuring `distill-fb` (2026-09-19); every other symbol its objects need is in `libmxc` or already listed here |
| A HarfBuzz port, from the amalgamation with `-fno-exceptions -fno-rtti` and no C++ standard library | the framebuffer shell's shaping | Muroxi has FreeType (for NetSurf) but no HarfBuzz; without it text falls back to unshaped advances (see Dependencies) |
| A per-process memory budget for the Distill engine: 96 MiB, with a 64 MiB JS heap inside it | M6 | M0 measured a median of 44 MiB and a 90th percentile of 69 MiB on live pages. The terminal shell fits the 16 MiB default as it is (7.4 MiB) |
| A per-process memory budget for the framebuffer shell: 48 MiB | M7 | Measured 43 MiB at 1920×1080 with the image viewer open (2026-09-19; see the M0 result). The shell holds several full-screen buffers, so the need scales with the screen |

### Optional

| Item | Note |
|---|---|
| A proof that a signal delivered while the stack pointer is in a not-yet-grown page reaches its handler | The one growth case Muroxi's stack arc has not proved yet |
| A larger main-stack reservation per package (a manifest field) | Not needed: M0's deepest real page used 345 KiB of the 8 MiB reservation |
| Sub-cell pointer positions (SGR-pixels, 1016) | Better pointer precision on the console; the compositor already has pixel input internally |
| HTTP/2 in `libcurl_https.a` (nghttp2) | Fewer connections for the same parallelism; HTTP/1.1 works |
| IPv6 in the curl build | Reaching IPv6-only hosts |

### Not asked for

- **A C++ runtime.** It would be needed only if litehtml were kept, and M4
  removes it.
- **`dlopen`.** Only miniaudio's device backends and `quickjs-libc.c` want it,
  and Distill links neither on Muroxi.
- **A longer `argv`, file-backed `mmap`, or `SIGPIPE`/`SIGSEGV`.** Distill
  passes requests in a file, loads fonts from memory, and treats a failed
  signal install as normal. Without `SIGPIPE`, a write to a closed pipe returns
  `EPIPE`, which is what Distill handles anyway.
- **More than 4 threads per process.** The shell needs at most three: the main
  thread, the audio writer, and the in-process engine where that mode is used.

**Done on the Muroxi side:** stacks that grow on demand, with
`pthread_attr_setstacksize` (Muroxi `eb15e557d`, `doc/stack-growth-plan.md`).
This replaces the earlier request for a per-process stack size above 64 KiB.

## What Distill uses, measured

Measured 2026-09-18. The method was the `--symbols` gate done by hand:
- **Distill's side:** `nm -u` over the objects of `distill` and `distill-tui`
  and their libraries, the QuickJS-ng shared library, and Lexbor's static
  archive, minus every symbol they define themselves.
- **Muroxi's side:** the symbols defined by `libmxc.a` and `libmxcompat.a` from
  Muroxi's `artifacts/iso`.

### What the gap means

| Undefined on Muroxi | Who needs it | Resolution |
|---|---|---|
| `sinh cosh tanh asinh acosh atanh expm1 log1p hypot round lrint scalbn` | QuickJS-ng | Muroxi adds |
| `pthread_cond_timedwait`, `pthread_condattr_setclock` | QuickJS-ng `cutils.h` | Muroxi adds |
| `posix_spawnp`, `posix_spawn_file_actions_addopen`, `pipe2` | `media_transfer.c`, `tui.c` (since M1, the `dproc` backend) | Muroxi adds |
| `realpath` | `main.c` (since M1, the `dfs` backend) | Muroxi adds |
| `strerror_r` | termbox2 | Muroxi adds |
| `pthread_attr_setstacksize` | `media_audio.c` (miniaudio) | Muroxi has it in source; the archive is older |
| `dlopen dlsym dlclose eventfd wcsrtombs sched_getscheduler sched_get_priority_min/max pthread_attr_setsched* pthread_attr_setinheritsched` | `media_audio.c`: miniaudio's device backends | **Distill, done in M1:** the audio core builds miniaudio with `MA_NO_DEVICE_IO`; devices live only in the Linux `daudio` backend, and PCM goes out through `daudio` |
| `malloc_usable_size` | QuickJS-ng | None: `cutils.h` returns 0 on platforms it does not know. M0's sized allocator counts the heap without it |
| libstdc++ and C++ ABI symbols (`__cxa_*`, `std::` containers and streams) | litehtml and its adapter | **Distill, done:** removed with litehtml in M4 |
| `__isoc23_strtol` and similar, `__errno_location`, `_setjmp`, `__libc_single_threaded` | glibc's headers, not the source | None: compiled against `libmxc`'s headers these are `strtol`, `errno` and `setjmp`, all present |

The curl side was measured the same way against `libcurl_https.a`: every
curl function Distill calls is present, and so is the cookie engine. The only
curl change is the profile and date parsing, above.

### The framebuffer shell, measured 2026-09-19

The same method was applied to `distill-fb`'s objects: the shell, the
display sinks, `distill-shell`, the layout, render core, painter, image and
audio libraries. Its Linux display backends were left out, since Muroxi
replaces them. Beyond the rows above, `libmxc` lacks only four functions:

| Undefined on Muroxi | Who needs it | Resolution |
|---|---|---|
| `fmaxf fminf lround nearbyintf` | `reader_layout.c`, `paint_soft.c` | Muroxi adds (Required later, above) |

The measurement also found two needs of the shell's own that were removed in
Distill rather than asked of Muroxi:
- an `fmin`, now a comparison;
- `__ctype_b_loc`, which is glibc's `isspace` in render.c, now `d_isspace`.

## Changes to other plans

Applied 2026-09-18: PLAN.md (the [Portability](PLAN.md#portability)
requirements, the stack table, Phase 7, licensing, risks and ledger),
FRAMEBUFFER_PLAN.md and MEDIA_PLAYBACK_PLAN.md now carry these changes, each
marked where it was made.

- **FRAMEBUFFER_PLAN.md, the page model.** That plan builds the framebuffer
  shell on the GTK shell's page model: `DLinkBox`, `DFormBox` and litehtml's
  render bands. This plan replaces that model with `reader_layout`'s box list
  (M4), because litehtml cannot be built on Muroxi. *Done:* `distill-fb`
  draws `DPage`, `reader_layout`'s page. It was fixed on the way to report
  links inside paragraphs, which `d_page_link_boxes` had left out.
- **FRAMEBUFFER_PLAN.md, input.** The tty and ANSI key decoder becomes shared
  core code with SGR mouse decoding added, because Muroxi delivers the pointer
  on the same stream. The evdev pointer stays a Linux-only input source.
  *Done:* [input_decode.c](../input_decode.c), shared with the TUI.
- **FRAMEBUFFER_PLAN.md, phase H.** "`ldd` shows no GLib" becomes "the static
  link passes `--symbols`", which is stronger and also covers Muroxi.
  *Open:* that link waits on M5's static dependency profile.
- **PLAN.md, language boundary.** The deliberate C++17 adapter at the
  rendering boundary goes away with litehtml. The only C++ left is HarfBuzz,
  built without a C++ runtime.

## Open decisions

- **The desktop host.** Retire the GTK shell, or keep it as a host window for
  the file chooser, clipboard, input methods and accessibility.
  `distill-fb-x11` now shows what retiring it would look like.
- **TLS everywhere.** Use Mbed TLS on Linux too, for identical TLS behaviour,
  or keep the distribution's curl.
- **Shaping without C++.** Stay with HarfBuzz, or evaluate a C shaper such as
  `kb_text_shape.h` against corpus pages 24, 25 and 26 (the RTL, CJK and
  complex-script pages) first. Its coverage is unmeasured here.
- ~~**The JS stack on Muroxi.**~~ Decided by M0: the stack limit less 1 MiB,
  7 MiB on Muroxi. The deepest real page used 345 KiB of the 8 MiB
  reservation, so the per-package reservation item is not needed.
- ~~**Budgets.**~~ Decided by M0: a 64 MiB JS heap and a 96 MiB engine quota,
  against NetSurf's 16 MiB. See the [M0 result](#m0-result-2026-09-19).

## Non-goals

- A Muroxi-specific code path anywhere outside `src/platform/muroxi/`.
- Raw framebuffer or GOP access on Muroxi; `libmxpresent` is the only way to
  get pixels.
- Running any of Distill inside the Muroxi kernel, a server or the base
  image; it is an optional package.
- A C++ runtime for Muroxi on Distill's behalf.
