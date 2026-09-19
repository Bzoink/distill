# Framebuffer plan

Status: planned. Phase 5 of [PLAN.md](PLAN.md). Where this plan changes
something PLAN.md decided, it says so and says why; see
[Changes to PLAN.md](#changes-to-planmd).

Revised 2026-09-18 by [muroxi_compat.md](muroxi_compat.md), which makes
Distill portable to Muroxi. Where a section below was changed by it, the
section says so. In short: the page model is `reader_layout`'s box list, not
litehtml's geometry; the key decoder is shared core code that also decodes SGR
mouse reports; and phase H's check is the static link, not `ldd`.

## Direction

Same core, a different place to put the pixels. The reader page, litehtml's
layout, the shaper, the fonts and the themes are the desktop's. What is new is
everything GTK was doing for us: painting, getting pixels onto a screen,
reading the keyboard and mouse, and drawing the controls a page's forms need.

The goal from PLAN.md stands: the same URL, read and interacted with, is
navigable by keyboard alone and by mouse alone on the framebuffer, matching
the desktop — within budget.

## What this machine has

Measured, because "this computer has no framebuffer" turned out to be half
true.

- **`/dev/fb0` exists.** It is `nvidia-drmdrmfb`, the DRM driver's fbdev
  emulation (`CONFIG_DRM_FBDEV_EMULATION=y`), 2560×1440, stride 10240. It is
  what the text consoles draw on; X runs on VT7 and owns the screen while it
  is showing.
- **Permissions are already in place.** The user is in `video` (`/dev/fb0`,
  `/dev/dri/card*`) and `input` (`/dev/input/event*`). No root is needed to
  run on a spare VT.
- **The stride is exactly width × 4.** So the real device will *not* catch a
  blit that ignores `line_length` — the one bug PLAN.md singles out. The test
  surfaces below pad their stride on purpose for that reason.
- **Also installed:** Xvfb, Xephyr, `qemu-system-x86_64`, and the `vkms`
  kernel module (virtual KMS, not loaded).

So the real target is Ctrl+Alt+F3 away. What is missing is a *convenient*
place to develop: switching VTs for every change is slow, and nothing on a
console can be screenshotted by a gate.

## What already exists

**The paint seam is already a C interface.** `src/render.h` is everything
litehtml draws with — fill, stroke, text, image, list marker, clip — and
`src/litehtml_probe.cpp` contains no Cairo call at all. Cairo lives entirely
inside `src/render.c`, behind that interface.

**Cairo is doing very little.** `render.c` uses it for seven things: solid
fills, rectangle strokes, compositing FreeType's 8-bit glyph masks
(`FT_RENDER_MODE_NORMAL`) in a colour, drawing a decoded image scaled with
`CAIRO_FILTER_GOOD`, one filled circle for list bullets, a rectangular clip
stack, and writing a PNG. Glyph rasterisation is already FreeType's, shaping
is already HarfBuzz's, and PNG writing can go through the libpng we already
link for decoding. This is the ~300 lines PLAN.md predicted, and it is now
confirmed by reading the code rather than by estimation.

**The TUI built most of the shell plumbing without GLib.** `src/tui.c` spawns
the engine with `posix_spawn`, reads its progress, services transfers and
runs a poll loop over plain descriptors. It also has a UTF-8 line editor
(`prompt_*`), a save-as prompt in place of a file chooser, and a signal-safe
restore path. None of it touches termbox except the drawing. It is all
`static` inside `tui.c` today.

**Shared already:** `app_state.c` (history), `address.c`, `form_submit.c`,
`settings.c`, `media_transfer.c` and the audio core. The framebuffer shell
links them as the TUI does.

**What the framebuffer shell is, then:** a page model built on
`reader_layout`'s box list — text runs, fills, borders, images and controls,
with link, control, anchor and focus geometry recorded during layout — with
the TUI's *plumbing*. It is not a port of either file.

*(Revised 2026-09-18. This plan first built on the GTK shell's page model:
`DLinkBox`, `DFormBox` and document bands from
`distill_litehtml_document_render_region`. litehtml cannot be built on
Muroxi, which has no C++ runtime, so
[muroxi_compat.md](muroxi_compat.md#our-own-layout-in-place-of-litehtml)
replaces it with `reader_layout`, the TUI's `reader_cells` generalised to a
pixel measurer. Until M4 lands, the shell may read litehtml's geometry through
the same box-list shape, so nothing above it changes when the source does.)*

## The findings that shape this plan

### The render gate cannot see a wrong pixel

`check.py --render` verifies that a PNG was written and that its dimensions
are positive. That is all. A painter that fills every page with black passes
it. A replacement painter therefore needs a gate that compares pixels before
it can be trusted, and that gate is part of phase A, not an afterthought.

### GLib arrives through the libraries this target needs

PLAN.md's hard rule is that the framebuffer target links no GLib. On this
distribution:

- **`libharfbuzz.so` links `libglib-2.0`** (and graphite2). The TUI never
  noticed because it does not shape. The framebuffer target does, so the
  distribution's HarfBuzz breaks the rule on the first link. HarfBuzz has to
  be built into `external/install` with `-Dglib=disabled -Dgobject=disabled`,
  the way litehtml and Lexbor already are.
- **`libinput.so` links GLib, GObject, gudev, libwacom and Lua.** PLAN.md
  named libinput as the input path with raw evdev as the fallback. As packaged
  here, libinput cannot be linked at all without breaking the rule, so the
  order has to flip; see [Input on the console](#input-on-the-console).

This is PLAN.md's step 7, "strip whatever quietly pulled GLib back in",
found before the code exists rather than after.

## Architecture

### Process model

Unchanged. `build/distill` stays one-shot and is spawned once per navigation
with `--template`; the framebuffer shell is long-lived and holds the history,
the laid-out document and playback, as the other two shells do.

### Three layers

```
  distill-fb shell   navigation, history, focus, forms, chrome, viewer
        │  paints into
  back buffer        XRGB8888, own width/height/stride, owned by the shell
        │  presented by
  display sink       memory | X11 window | fbdev | libmxpresent   (+ KMS later)
        ▲  events from
  input source       script | X11 events | tty + evdev | stdin (ANSI + SGR)
```

The shell never knows which sink it is on. A sink and its input source come
as a pair, since the X11 window delivers both and the console delivers both.

```c
typedef struct DDisplay DDisplay;

typedef struct {
    int width, height;          /* pixels */
    int stride;                 /* bytes per row; never assumed width * 4 */
} DDisplayMode;

typedef enum {
    D_EVENT_KEY,                /* key + modifiers + UTF-8 text, if any */
    D_EVENT_POINTER_MOVE,       /* absolute, in display pixels */
    D_EVENT_POINTER_BUTTON,
    D_EVENT_SCROLL,             /* wheel, in lines or pixels */
    D_EVENT_RESIZE,             /* a window was resized */
    D_EVENT_SUSPEND,            /* VT switched away, or presentation taken: stop drawing, stop reading */
    D_EVENT_RESUME,             /* VT switched back, or new generation: redraw everything */
    D_EVENT_QUIT,
} DEventKind;

DDisplay *d_display_open(const char *spec);   /* "memory:WxH", "x11:WxH", "fbdev:/dev/fb0",
                                                 "mxpresent:console", "mxpresent:compositor" */
DDisplayMode d_display_mode(const DDisplay *display);
size_t d_display_fds(DDisplay *display, struct pollfd *fds, size_t capacity);
bool d_display_next_event(DDisplay *display, DEvent *event);
void d_display_present(DDisplay *display, const uint32_t *pixels, int stride,
                       const DRenderRect *damage, size_t damage_count);
void d_display_close(DDisplay *display);
```

Handing back descriptors instead of owning the loop is the property that made
termbox2 the right choice for the TUI, for the same reason: the shell also
waits on engine pipes, transfers and the audio core.

The display is the one platform interface chosen at run time, because gates
use the memory sink and real runs use the screen. It lives under
`src/platform/` with the others, and each sink is compiled only on the
platform that has it: fbdev and X11 on Linux, `libmxpresent` on Muroxi.

`present` copies damaged rectangles **row by row** from the back buffer into
the device, honouring both strides. That is the whole of "the blitter" in
PLAN.md's sense. The painter is a separate thing — see phase A.

### The pointer

fbdev has no hardware cursor. The sink draws the pointer on the *presented*
copy only: moving it restores the rectangle underneath from the back buffer
and draws it again. The back buffer never contains a cursor, so screenshots
and the shell's own reads of it stay clean.

### The console

What PLAN.md calls "surface lifecycle", made concrete:

- **Refuse to start in the wrong place.** On a VT only: if stdin is not a
  `/dev/ttyN` (for example, run from a terminal inside X), exit with a message
  naming `--display x11`, rather than scribbling on a console that X is
  covering.
- **`KDSETMODE KD_GRAPHICS`**, so the kernel stops drawing text and the
  blinking cursor over us.
- **`VT_SETMODE VT_PROCESS`**, with signals for release and acquire. On
  release: stop presenting, stop reading the mouse, acknowledge with
  `VT_RELDISP`. On acquire: redraw everything, since the console may have
  drawn over the screen in between.
- **Restore on every exit we can see.** `KD_TEXT`, `VT_AUTO`, the termios
  state, in the signal handler for `SIGTERM`, `SIGINT`, `SIGSEGV` and
  `SIGABRT`, as the TUI's emergency restore already does for its terminal.
  Installing each handler may fail, and the shell carries on without it
  (PLAN.md's portability rules; Muroxi has no `SIGSEGV` or `SIGABRT`).
  `SIGKILL` cannot be caught; the recovery (switch VT and back, or
  `kbd_mode -u; chvt`) goes in the help text, not in the code.

### Input on the console

**Keyboard: the tty, not evdev.** The recommendation, and a change from
PLAN.md. In `KD_GRAPHICS` the VT still translates keys through the console
keymap and delivers them on the tty. Reading the tty in raw mode gives us:

- the user's layout, dead keys and compose as `loadkeys` configured them, so
  å, ä and ö arrive as text with no xkbcommon and no keymap of our own;
- Ctrl+Alt+Fn VT switching, still done by the kernel. With evdev and the
  keyboard in `K_OFF` we would have to implement it, and a bug there strands
  the user on our screen;
- no reading of every keyboard on the machine, which evdev access is
  (membership of `input` is equivalent to a key logger).

The cost is that the Linux console does not encode every modifier combination
distinctly. Ctrl+arrows and Ctrl+Tab, for example, arrive as the plain key
under the default keymap. The key map should be chosen with that in mind. The
TUI lives with the same limits and its key map already fits them.
termbox2's decoder already knows the `linux` terminal's sequences and is the
first thing to try reusing, input side only.

*(Revised 2026-09-18.)* The decoder is **shared core code**, not part of the
fbdev sink: it turns an ANSI byte stream into `DEvent`s, and it also decodes
SGR (1006) mouse reports. Muroxi delivers its keyboard and its pointer on that
one stream, in cell coordinates, so the same decoder serves the Linux console
and Muroxi and is host-tested once. Cell coordinates are converted to pixels
by the sink, from `TIOCGWINSZ`, which is why the shell's hit targets must be
at least a character cell in size.

**Pointer: raw evdev**, on Linux only; Muroxi has no evdev and uses the
SGR stream above. Relative mice are small: `REL_X`, `REL_Y`,
`REL_WHEEL`, `BTN_LEFT/RIGHT/MIDDLE`, accumulated and clamped to the screen,
opened from `/dev/input/by-id/*-event-mouse` or chosen by capability. That is
perhaps 150 lines. Touchpads are where this breaks down (absolute multitouch,
tap to click, palm rejection) and are where libinput earns its keep. They are
[an open decision](#open-decisions), not a first-version requirement.

### Painting chrome and forms

The shell draws its own chrome with the same `d_render_*` calls litehtml
uses: address bar, status line, progress, the audio transport and menus for
theme and zoom. Nothing here is a widget toolkit. It is a handful of
fixed-shape controls drawn at known places.

Forms are the real new work, as PLAN.md said. The GTK shell places native
widgets over each `DFormBox`. Here the shell draws them itself, in the control
boxes `reader_layout` records, and so does every other shell once that layout
lands (muroxi_compat.md M4) — the drawing is written once and shared: text and password fields, a multi-line textarea, checkbox, radio,
button, and a select that opens a list. Editing uses one UTF-8 line-editing
model shared with the TUI, extracted from its `prompt_*` functions rather
than written twice. A textarea is that model over several lines; the TUI's
single-line textarea is a known gap it can close from the same code.

Submission goes through `form_submit.c`, as both other shells do, so the
three send identical requests.

### Viewer mode and media

A full-screen state swap, as PLAN.md specified, with zoom as a scale on
`d_render_draw_image`. The audio core and transfers are linked unchanged.
Downloads use the TUI's save-as prompt. External hand-off is unavailable on
a console, and PLAN.md already accepts that.

## The window sink

The development surface: a plain X11 window, default 1920×1080 and resizable
to any size, including 2560×1440 to match this machine's console. It is
Xlib with MIT-SHM, no toolkit, and it delivers keys through `Xutf8LookupString`
and the pointer through X events.

It is honest about what it covers. It exercises the painter, the shell, the
chrome, forms, focus and navigation, which is most of the phase. It does not
exercise fbdev probing, VT switching, console restore, tty key decoding or
evdev. To keep it from hiding the stride bug, it presents from a back buffer
whose stride is **deliberately padded** past `width × 4`.

It is a separate target, `distill-fb-x11`, built from the same sources plus
the X11 sink, so that libX11 can never leak into `distill-fb`. It is off in a
minimal build.

### And the real console, early

The window is a convenience, not a substitute. The first time the painter
draws a page, the fbdev sink should draw the same page on VT3 of this
machine, because VT switching, `KD_GRAPHICS` and restore on crash are the
parts that go wrong, and only a real VT shows them. Phase D below sits
immediately after the first pixels for that reason, not at the end.

### QEMU, if VT switching gets in the way

A small VM booted to a console at 1920×1080 appears as a window on the
desktop, and inside it are a real `/dev/fb0`, real `/dev/input/event*` and
real VTs. It is the full target, in a window. It costs a guest image to set
up once. It is also the clean machine the permission story should be tested
on, since this user's groups are already set up. Optional, not a phase.

## Delivery phases

### A — the software painter

A second implementation of `render.c`'s paint half against a plain XRGB8888
buffer: fills and strokes with alpha, glyph masks composited in a colour,
images scaled (box filter down, bilinear up, to stand in for
`CAIRO_FILTER_GOOD`), the bullet circle, a clip stack, PNG through libpng.
Font loading, shaping and measurement do not change. `d_render_surface`
gains a pixel-and-stride accessor so nothing outside `render.c` casts to
`cairo_surface_t`.

A build option chooses the painter. The GTK shell keeps Cairo until parity is
shown; after that, no target but a GTK host window links Cairo (muroxi_compat.md
M3). PLAN.md's reason for bundling fonts was typography identical on every
target, and one painter is how that extends to pixels.

The painter keeps within the portability rules: no local buffer over 4 KiB and
no unbounded recursion, since it runs in the long-lived shell, which on Muroxi
has 64 KiB of guaranteed stack.

**Done when** a new `check.py --paint` stage renders every render-gate page
with both painters and the difference stays under a stated threshold (mean
per-channel error and the largest differing region), and the stage has been
shown to fail against a painter with a deliberate defect.

*Done 2026-09-19, as muroxi_compat.md M3* (see its M3 result):
- The painter is `src/paint_soft.c` behind `src/paint.h`.
- `d_render_pixels` is the pixel-and-stride accessor.
- The largest difference from Cairo on any page is a mean of 0.051 per
  channel, and no pixel differs by more than 32.
- Every shipped binary uses the software painter, the GTK shell included, so
  "one painter everywhere" is settled for all but the window the GTK host
  shows it in.

### B — the seam, the memory sink, a headless shell

`d_display_*` with the memory sink, and a `distill-fb` that opens a reader
page, lays it out, paints it and scrolls. The memory sink takes its input from
a script (keys, clicks, waits) and writes screenshots and a state line
(URL, scroll offset, focus) on request. Its stride is padded.

**Done when** `check.py --fb` drives a corpus article through scripted
scrolling and link focus and asserts on the state and the screenshots,
headless, and the stride padding has been shown to catch a flat `memcpy`
blit.

### C — the window

The X11 sink and `distill-fb-x11`. From here on this is the daily loop.

**Done when** a corpus article reads end to end in a 1920×1080 window by
keyboard and by mouse, and in a resized one.

### D — the console

The fbdev sink: `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO`, a check that the
format is 32-bit XRGB (refuse anything else with a clear message rather than
convert), `mmap`, `KD_GRAPHICS`, `VT_PROCESS`, restore on exit. Keyboard
from the tty, mouse from evdev, the drawn cursor.

**Done when**, on VT3 of this machine, a corpus article reads by keyboard and
by mouse; switching to VT7 and back redraws correctly and the shell reads no
input while away; `SIGTERM` and `SIGSEGV` both leave a usable text console;
and starting from a terminal inside X refuses with the message.

### E — shared plumbing, navigation

Move the TUI's engine run, progress reading, transfer servicing and line
editor out of `tui.c` into `distill-shell`, proven unchanged by
`check.py --tui`. Then the framebuffer shell gets history, the address bar,
link activation, section jumps, reload, zoom and theme from them.

**Done when** the TUI suite passes against the extracted code, and a
framebuffer session can type an address, follow links, go back and forward
and change theme, gated through the memory sink.

### F — forms and text entry

The drawn controls, focus order across links and controls, the shared line
editor, textarea, select popups, and submission through `form_submit.c`.

**Done when** the corpus pages that exercise forms and search can be driven
to a result in the memory sink, and the requests they send are identical to
the GTK shell's for the same input.

### G — media and viewer mode

Viewer mode, zoom, the audio transport, downloads through the save-as prompt.

**Done when** a page's audio plays and its image opens in the viewer on the
console, and the same is gated through the memory sink with
`DISTILL_AUDIO_BACKEND=null`.

### H — build, permissions, budget

HarfBuzz built GLib-free into `external/install`, from its amalgamation with
`-fno-exceptions -fno-rtti` and no C++ standard library; the fonts installed
under the data directory, since `DISTILL_FONT_DIR` points into the source tree
today; the permission story decided and documented; the link line checked.

**Done when** the static link of `distill-fb` passes muroxi_compat.md's
`--symbols` gate, with no GLib, GObject, Pango, fontconfig, Cairo, pixman, X11
or C++ runtime in it, and the footprint is measured against PLAN.md's budget
under the same reading the TUI's record used.

*(Revised 2026-09-18. This condition was "`ldd distill-fb` shows no GLib".
The static-link check is stronger — `ldd` cannot see a statically linked
dependency — and it covers Muroxi too.)*

## Testing and gates

- **Painter parity** (`--paint`), phase A. The render gate only proves a PNG
  exists; this is the gate that proves it is the right PNG.
- **The memory sink as the shell's gate** (`--fb`), in the TUI suite's shape:
  scripted input, asserted state, screenshots. Everything above the sink is
  gated here.
- **Padded stride in every test surface**, because the real device on this
  machine does not have one.
- **Model-level self-tests** for key handling, focus order and the line
  editor, as `--self-test` already does for the GTK shell.
- **What cannot be gated**, stated rather than papered over: VT switching,
  `KD_GRAPHICS` and console restore, tty decoding under a real keymap, and
  evdev. These are checked by hand on a VT against the phase D list. `vkms`
  may give a headless fbdev device with the real ioctls and a readable buffer,
  which would move probing and mode handling into a gate; it needs root to
  load and is worth an afternoon to find out, not a dependency.

## Changes to PLAN.md

- **Input.** PLAN.md: libinput for keyboard and pointer, raw evdev as the
  fallback. This plan uses the tty for the keyboard and raw evdev for the
  pointer. libinput links GLib here, and the tty keeps the console's keymap
  and VT switching for free.
- **A second, non-target sink.** PLAN.md has one surface. This plan adds a
  memory sink for gates and an X11 window for development, behind an
  interface the fbdev sink also implements.
- **Order.** The real console comes at phase D, straight after the first
  pixels, not at the end of the phase.

## Changes made to this plan by muroxi_compat.md

- **The page model.** `reader_layout`'s box list replaces the GTK shell's
  `DLinkBox`, `DFormBox` and litehtml render bands, because litehtml cannot be
  built on Muroxi.
- **Input.** The tty key decoder becomes shared core code with SGR mouse
  decoding added, because Muroxi delivers the pointer on the same stream. The
  evdev pointer stays a Linux-only input source.
- **A fourth sink.** `libmxpresent`, with its console and compositor
  providers. A changed presentation generation maps to
  `D_EVENT_SUSPEND`/`D_EVENT_RESUME`.
- **Phase H.** "`ldd` shows no GLib" becomes "the static link passes
  `--symbols`".
- **One painter.** The open question of whether the GTK shell moves to the
  software painter is decided for every target but a GTK host window.

## Open decisions

- **Touchpads.** Raw evdev handles mice. Supporting touchpads properly means
  libinput built without GLib (without libwacom and gudev) or our own
  multitouch handling. Is a touchpad a requirement for "navigable by mouse
  alone"?
- **Permissions.** Group membership (`video` + `input`, as here), a udev rule,
  or logind's `TakeDevice`, which hands devices to the active session and
  revokes them on VT switch but needs a D-Bus client. The groups work today
  and are the least code. `input` is also the broadest grant.
- **fbdev or KMS.** fbdev is what PLAN.md chose and what is here, but the
  kernel treats it as legacy and new drivers reach it only through emulation.
  KMS dumb buffers give page flipping, vsync and a choice of output. The sink
  interface makes KMS a fourth sink, not a rewrite, so this can wait for a
  machine where fbdev is missing.
- **The desktop host.** Once the painter and layout are ours, the GTK shell
  is either retired for `distill-fb-x11` or kept as a thin host for the file
  chooser, clipboard, input methods and accessibility. This replaces the
  earlier "one painter everywhere" question; see muroxi_compat.md.
- **The budget's reading.** Still open from the TUI's record: libcurl and
  OpenSSL alone exceed 8 MB as this distribution packages them. The
  framebuffer target adds FreeType and HarfBuzz to that line; litehtml and
  libstdc++ (2.9 MB) leave it with muroxi_compat.md M4, and Mbed TLS in place
  of OpenSSL is an open decision there.

## Non-goals

- Running on `/dev/fb0` while X or a Wayland compositor owns the screen.
- More than one output, rotation, or mode setting; the mode the console has
  is the mode we use.
- Hardware acceleration of any kind.
- A clipboard shared with anything outside the process.
- Input methods for scripts the console keymap cannot type (CJK input).
- Bidirectional reordering, as in the TUI plan: a decision for the reader as
  a whole, not for one front end.
