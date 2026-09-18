# TUI plan

Status: implemented, phases A–H — see [Implementation record](#implementation-record)
for what was built, what the plan got wrong, and what is left. Phase 6 of
[PLAN.md](PLAN.md), and the last visualization target.

## Direction

The terminal is the target this project was shaped for. Phase 2 broke lines
against a measurement callback rather than against pixels specifically so the
breaker would be reused here; Phase 3's shell keeps its history in a file so a
second front end could read it; the audio core was written with no front end in
its header. Phase 6 is where those choices are collected or shown to have been
decoration.

The reading experience is text, and most of it needs no raster path at all.

## Library decision

Use **termbox2**, vendored as a single header the way miniaudio already is.

Measured rather than remembered: 4,321 lines, 189 KB, one header, MIT, and its
only includes are libc and POSIX — `termios.h`, `sys/ioctl.h`, `sys/select.h`
and friends. Nothing to install, nothing to link, nothing new in the dependency
graph. It reads terminfo when it is there and falls back to built-in capability
tables for common terminals when it is not, which is what a minimal target
needs.

Three things in its API decide the choice:

- **`tb_get_fds`** hands back its input descriptors instead of owning the loop.
  [PLAN.md](PLAN.md) deferred notcurses partly because it "brings an input model
  that would compete with ours"; this is the shape that does not. The shell
  polls termbox's descriptors alongside anything else it waits on.
- **`tb_send`** writes raw bytes through to the terminal, and **`tb_invalidate`**
  tells the cell model to forget what it thought was on screen afterwards.
  Together they are the escape hatch for a kitty or sixel image, which PLAN.md
  wants us to encode ourselves. A library without that hatch would make the
  image path fight the cell grid.
- **`tb_wcwidth`** and the UTF-8 helpers give the cell-measuring callback a
  base, including the double-width behaviour the CJK fixtures already exercise.
  A base and not the answer: `TB_OPT_EGC` is off unless we define it, and the
  header states outright that "no attempt is made to do proper grapheme cluster
  parsing" — with the option on, zero-width codepoints are tacked onto the
  previous cell as a coarse approximation. Clustering is ours to get right
  above the library; see [Writing systems](#writing-systems).

What it does not provide, and we write: image protocols, block layout, forms,
and everything above the cell grid. That is the correct division — those are
the parts that are ours.

### Alternatives considered

- **notcurses** — already deferred in PLAN.md, and the reasons hold. It is
  capable, and it is large; we would use a fraction of it and inherit its event
  model. Its built-in sixel and kitty support is the real temptation, but the
  encoder is a few hundred lines against a decoded RGBA buffer we already have.
- **ncurses** — universally present, and wrong for this. It is a curses API
  from a different era, its colour and mouse handling are awkward, and linking
  it drags terminfo in as a hard dependency. Nothing here wants a WINDOW.
- **termpaint**, **tickit** — both reasonable, both larger than termbox2 and
  less widely exercised. Revisit only if termbox2's capability tables prove too
  thin in practice.
- **Writing our own** — PLAN.md's instinct, and the honest comparison is
  narrower than it first looks. The cell grid and the escape output are indeed
  small. The parts that are not are the key-escape trie across terminals, SGR
  mouse decoding, resize via SIGWINCH without a race, raw-mode restore on every
  abnormal exit, and diffing the cell buffer so a slow terminal is not
  repainted whole. That is the 4,000 lines, and they are not interesting
  lines. Vendoring keeps them ours to fix without being ours to write.

## What already exists

The reason to be optimistic about this phase, checked rather than assumed.

**The line breaker is already target-neutral.** `src/shaper.h` is the seam:

```c
typedef int (*DTextMeasure)(void *opaque, const char *text, size_t length);
size_t d_text_wrap_utf8(const char *text, size_t length, int max_width,
                        DTextMeasure measure, void *opaque,
                        DTextLine *lines, size_t line_capacity);
```

`src/shaper.c` includes exactly one non-libc header, `<linebreak.h>`. It needs
libunibreak and nothing else; the CMake target over-links it today. The single
caller is `render.c`, measuring shaped glyphs. The TUI adds a second caller
whose callback counts cells. This is the Phase 2 promise, and it was kept.

**The shell model is dependency-free.** `src/app_state.c` implements the
history the GTK shell navigates, and its header includes only `<stdbool.h>` and
`<stddef.h>`. Its unit test links no libraries at all. The TUI reuses it as is.

**Image decoding is already free of cairo.** `src/image.c` includes only
libpng, libjpeg and libwebp, and produces a plain buffer through
`d_image_decode`. A sixel or kitty encoder consumes that directly; nothing in
the image path needs a rasterizer.

**The audio core has no front end in it.** `media_audio.h` deliberately exposes
no miniaudio types, and the GTK transport is an adapter over it. The TUI links
the same static library and draws its own transport — this was the plan, and it
does not need revisiting.

## The finding that shapes this plan

`--template` is what the TUI consumes, and today it is unavailable in a build
without litehtml:

```
} else if (template_path) {
#ifdef DISTILL_RENDER
    ...
#else
    fprintf(stderr, "this build has no litehtml renderer\n");
```

**That guard is an accident of placement.** The region it belongs to runs 1,190
lines and holds `reader_template`, every `append_json_*` card renderer, the
JSON property helpers and the Markdown exporter — and it references **no**
`d_render_*`, `distill_litehtml_*` or `cairo_*` symbol anywhere. `reader_metrics`,
which looks like layout, is arithmetic on a zoom percentage.

So the first work of this phase is not terminal code at all. Move that family
out from behind `DISTILL_RENDER` and a minimal engine build produces the reader
page and the Markdown export with **no litehtml, no gumbo, no cairo, no
FreeType and no HarfBuzz**. Without that move, the smallest target in the
project has to link a C++ HTML layout engine in order to print text, which is
close to the opposite of the point.

### And the build will not permit it yet

Freeing the region is necessary and not sufficient.
[CMakeLists.txt](../../CMakeLists.txt) declares FreeType, HarfBuzz, Cairo and
libunibreak as one `REQUIRED` pkg-config group, so a machine without Cairo
cannot *configure* the project, never mind build a renderer-less binary. The
criterion for freeing the reader page — a configure without litehtml that still
produces `--template` — is unreachable until that group is split, which is why
the split is a phase of its own ahead of it.

The split follows the code rather than a diagram. `shaper.c` needs libunibreak
and nothing else, so libunibreak becomes its own required module; FreeType,
HarfBuzz and Cairo become one optional group, and `DISTILL_RENDER` is derived
from whether it was found rather than asserted beside it. Two link
configurations of the existing binary — the project ships one CLI and one
shell, and inventing a third component to match the layering would cost more
than it explains.

## Architecture

### Process model

Unchanged, and that is the argument for it. `build/distill` stays one-shot: the
TUI spawns it per navigation and reads back `--template`, exactly as the GTK
shell does. Anything that must survive a navigation already lives in a file —
cookies through `--session-file`, the page itself through `--template`.

The TUI is a long-lived process, so it is where playback lives, as
[MEDIA_PLAYBACK_PLAN.md](MEDIA_PLAYBACK_PLAN.md) requires.

### The reader-page walker

The load-bearing idea. The Markdown exporter already walks the reader template
and emits a linear styled document: headings, paragraphs, lists, tables, links,
cards, form controls, dropping what only the shell understands. **A terminal
renderer is that same walk with a different sink.**

So factor the walk out of the Markdown exporter and give it a sink interface,
with two implementations:

- the Markdown sink, which is the code that exists today;
- the cell sink, which wraps each run with `d_text_wrap_utf8` against a
  cell-measuring callback and writes cells.

**The sink vocabulary has to be wider than "open a block, emit a styled run,
close a block."** That was this plan's first answer, and it cannot carry what
the sections below go on to require: a table needs its columns measured across
every row before the first is drawn; a link needs an identity the focus cursor
can land on and an action the shell can perform; a media card needs its
`distill-play:` destination; a form control needs its `forms` binding; the
document needs its direction. Those are events, not styled text runs, and a
sink that sees only text runs will have each of them bolted on sideways.

The interface is therefore an event vocabulary: block open and close carrying
the block kind, styled text runs, and explicit events for links, form controls,
images, audio cards, table structure, anchors and page direction. A sink may
buffer one block before emitting it, which is how the table measurement pass
gets its two passes without the walker having to know what a table is.

What this deliberately is **not** is a materialized intermediate
representation. An IR would become a third contract beside the extraction JSON
and the reader page, and a third thing to keep in step — the exact drift the
walker exists to prevent. These events are a calling convention; the reader
page stays the single source of truth.

Two things fall out of this that are worth more than the code saved. The TUI
inherits the shape dispatch, the cards and the ordering without knowing what a
shape is; and a change to the reader template reaches every front end at once,
rather than being reimplemented per target and drifting. The Markdown gates
added in check.py already guard the walk, so the TUI starts with the walk
tested.

It also means the TUI does **not** parse the extraction JSON. There is one
reader page and everything renders it.

### Layout in cells

The reader template's own structure supplies the block model; the terminal
supplies the measure. Column width is `min(terminal columns - margins, 80)`,
because a reader that lets prose run to a 200-column terminal has misunderstood
the job. Headings, lists and blockquotes take the indent and the blank-line
rules the Markdown sink already encodes.

Tables are the one place cells are harder than pixels: column widths must be
measured across every row before the first is drawn, and a table wider than the
terminal needs a horizontal offset rather than a reflow. Treat a table as a
block that owns its own scroll axis.

### The cell renderer is a pure function

The renderer takes a reader page, a width and a height and produces a cell
grid. It does not initialize a terminal, query a capability or call termbox2 —
those belong to the shell that drives it.

This began as a testing convenience and is worth more than that. It is what
makes `--dump-cells --columns N --rows N` a deterministic artifact a gate can
assert on with no tty present, and it is what lets a framebuffer target later
reuse the renderer whole instead of reimplementing it beside a second I/O
layer. termbox2 is then exactly what the library decision claims: input and
output, with nothing above the cell grid.

### Writing systems

What the terminal can and cannot do here, stated before it is promised.

**Ours to get right in this phase.** Cell width for wide characters, combining
marks that must not consume a cell of their own, and emoji sequences that have
to be treated as one cluster — through an application-level grapheme iterator
over the measure callback, not left to `tb_wcwidth` alone, for the reasons in
the library decision. The CJK fixtures gate the wide-character half of it.

**Carried forward, not solved here.** Bidirectional reordering. The breaker
does not reorder and neither does the desktop renderer;
[corpus/manifest.json](../../corpus/manifest.json) records against
`24-rtl-arabic` that mixed-direction ordering "needs the Unicode Bidirectional
Algorithm and is recorded rather than asserted". The TUI carries the direction
metadata through the walker and sets each block's base direction from it. It
does not become the place this project acquires a bidi implementation: that is
a decision for the reader as a whole, and taking it here would fix it in the
one front end least able to show the result.

### Input

Keyboard through termbox2's event stream; the key map follows the GTK shell so
one set of habits works everywhere, with the media keys from the media plan
(`Space`, `s`, arrows, `[`/`]`). Mouse through SGR reporting, which termbox2
decodes, for clicking links and dragging the audio position handle.

Links are activated the way a terminal can: a focus cursor moved with Tab and
the arrows, not invented shortcut numbers. Forms re-render as in-terminal entry
using the same `forms` contract the GTK shell submits through.

### Images and viewer mode

`$TERM` plus a device query decides the path: kitty graphics where it exists,
sixel where that does, and `[image: alt text]` everywhere else, which is
PLAN.md's position and stays it. Unicode block art is not planned.

The encoder is ours, consuming `d_image_decode` output and writing through
`tb_send`, with `tb_invalidate` afterwards so the cell model does not believe
stale contents. Viewer mode re-encodes at a different crop and cell size — a
mode over the same surface, not a second window, which is what makes it work
here and on the framebuffer.

### Media transfer

A gap this plan missed on its first pass. The media plan's whole answer for
video is **download**, and the audio core opens a file on disk
(`media_audio_open(const char *path, ...)`), so both paths run through a
transfer. The only transfer that exists is `MediaTransfer` in `src/app.c`,
which allocates with `g_new0` and reports progress through GTK idle callbacks.
A front end that links no GLib therefore has no audio hand-off, and no video
story at all.

So the transfer moves down beside the audio core as a front-end-agnostic piece:
the engine run once with `--fetch-media` (the shells open no socket of their
own — this plan first said "libcurl on its own thread", which was written
before reading the code and is not what the desktop does), heard through a
descriptor, with cancellation, a temporary file renamed atomically on success,
failure reported as state rather than as a dialog, and completion handed to
`media_audio_open`. The GTK shell becomes a second caller of it, the
way it is already a second caller of the audio core. This is the first task of
the media phase, not a footnote to playback.

### Settings

An outstanding gap this phase has to close. The GTK shell keeps preferences in
a `GKeyFile` at `~/.config/distill/settings.ini`, which is GLib, which the
minimal targets may not link. The file format is fine; the reader is not. A
small GLib-free INI reader over the same file lets both shells agree on the
three keys that exist: theme, start page and zoom.

## Delivery phases

### A — split the dependency group

Separate libunibreak from FreeType, HarfBuzz and Cairo in CMake; derive
`DISTILL_RENDER` from whether the render group was found; stop `shaper.c`
over-linking. Nothing else changes behaviour.

**Done when** the project configures on a machine with no Cairo and the
existing gates still pass on one that has it.

### B — free the reader page

Move the reader-template family out from behind `DISTILL_RENDER`; confirm a
configure without litehtml still produces `--template` and `--markdown`. No
terminal code.

**Done when** a build with the renderer disabled passes the extraction and
markdown gates.

### C — the walker

Factor the reader-page walk out of the Markdown exporter behind the event
vocabulary above; re-express the Markdown exporter in terms of it, unchanged in
output, proven by the existing markdown gates.

**Done when** the markdown gates pass against a walker with two sinks and one
of them is not yet written.

### D — the cell renderer

The cell sink, the measure callback into `d_text_wrap_utf8`, the grapheme
iterator, block layout, tables and `--dump-cells`. No terminal is initialized
in this phase and termbox2 is not yet vendored.

**Done when** corpus pages dump identical cell grids at 40, 80 and 120 columns
across runs, the CJK fixtures included, with no tty attached.

### E — the terminal shell

Vendor termbox2. Raw mode, the event loop over `tb_get_fds`, drawing the cell
grid, scrolling, resize, and restore on abnormal exit.

**Done when** a corpus article reads end to end in a real terminal and in a
resized one, and the terminal is left clean after a crash.

### F — navigation and forms

`app_state.c` history, address entry, link focus and activation, section jumps,
tables with their own scroll axis, forms and submission.

**Done when** the corpus pages that exercise forms and search can be driven to
a result without leaving the terminal.

### G — media

The front-end-agnostic transfer first, then the audio core and the transport.
Then the capability probe, the image encoder and viewer mode.

**Done when** a page's audio plays from a terminal over SSH, which is the case
that motivated putting audio in scope at all, and a video downloads from one.

### H — settings and packaging

The GLib-free settings reader, shared with the GTK shell. Confirm the link
line: libcurl, Lexbor, QuickJS-ng, libunibreak, libpng, libjpeg, libwebp, and
the vendored termbox2 and miniaudio. No GLib, GObject, Pango, fontconfig,
Cairo, FreeType, HarfBuzz or litehtml.

**Done when** the footprint is measured against PLAN.md's budget rather than
asserted to be inside it — Lexbor and QuickJS-ng counted, since both are shared
objects and both are larger than anything else on that line.

## Testing and gates

The corpus is HTML and assertions, and none of it assumes a window; a terminal
front end should be the easiest target to gate honestly, not the hardest.

- **A cell-dump mode that needs no terminal.** `--dump-cells FILE --columns N
  --rows N` writes the grid as plain text, the way `--template` writes HTML,
  driven by the pure renderer with termbox2 uninvolved. (`--columns`/`--rows`
  rather than the `--width`/`--height` first proposed: those are pixels
  everywhere else in the CLI.) That makes a terminal rendering a deterministic
  artifact a gate can assert on: a `cells` stage in `check.py` beside the
  `markdown` one.
- **Width sweeps.** Assert the same page at 40, 80 and 120 columns. Wrapping
  bugs hide at one width.
- **The scripts that already exist.** `--self-test` conventions carry over: key
  handling, history and the address bar are model-level and testable without a
  terminal.
- **A PTY suite for the shell.** What the dump cannot reach — raw-mode entry
  and restore, resize handling, and leaving the terminal clean after an
  abnormal exit — is testable by driving the binary under a pseudo-terminal,
  and those are the failures a user notices first.
- **What still cannot be gated**, stated rather than papered over: real terminal
  capability negotiation, sixel and kitty output, and mouse reporting need a
  terminal. Two GTK bugs this cycle — a scroll on focus and a bar whose children
  were never shown — were invisible to every headless gate and found by driving
  a real window. The same will be true here, and the cell-dump gate should not
  be mistaken for covering it.

## Non-goals

- Unicode block-art images; the capability probe decides between a real
  protocol and alt text.
- A curses-style full-screen application framework, panels, or widget toolkit.
- Reflowing tables to fit narrow terminals rather than scrolling them.
- Reimplementing the reader's layout decisions; the TUI renders the reader page
  and holds no opinion about shapes.
- Terminal themes beyond the reader's existing three.
- Bidirectional reordering; the direction metadata is carried and the base
  direction is set, and the rest waits on a decision for the reader as a whole.

## Implementation record

What was built against this plan, where it had to change, and what is left.
Written when the phases landed, so the plan above keeps its reasoning and this
keeps the evidence.

### What exists

- **Build (A, B).** libunibreak is its own required module; FreeType, HarfBuzz
  and Cairo are one optional group and `DISTILL_RENDERER=OFF` asks for the
  build without them. `DISTILL_READER` (the reader page, the Markdown export,
  media fetches) is separate from `DISTILL_RENDER` (laying it out and painting
  it). Checked by hiding Cairo from pkg-config entirely: the project
  configures, and the engine it builds links no Cairo, HarfBuzz, FreeType,
  litehtml, GLib, Pango or libstdc++, and passes the extraction and markdown
  gates with results identical to the full build.
- **The walker (C).** `src/reader_walk.{h,c}` over helpers moved verbatim into
  `src/dom_util.h`. `src/reader_markdown.c` is the exporter re-expressed as a
  sink, proven byte-identical to the old one on all 42 corpus reader pages, a
  stress page written to reach every branch, and the recorded bundle.
- **The cell renderer (D).** `src/reader_cells.{h,c}`, with cell widths from
  `src/cell_width.h`, generated from Unicode 16.0 by
  `tools/make_cell_width.py`, and grapheme clusters from libunibreak. Exposed
  as `distill --dump-cells FILE --columns N --rows N` and `--cells-from PAGE`.
  602 runs across every reader page at 8–200 columns: no overflow by an
  independent measure (python-wcwidth), no nondeterminism, clean under ASan
  and UBSan.
- **Shared with the desktop.** `distill-shell` holds the form rules
  (`form_submit.c`), what a typed address means (`address.c`), the settings
  file (`settings.c`, checked against GKeyFile in both directions) and media
  transfers (`media_transfer.c`). The GTK shell now calls all four; its form
  submission and download were checked against the previous binary on a
  virtual display and send identical requests and save identical files.
- **The shell (E–H).** `src/tui.c`, `src/tui_screen.c` (the output writer),
  `src/term_image.c` (kitty and sixel encoders), termbox2 compiled once in
  `src/termbox_impl.c`. Target `distill-tui`.
- **Gates.** A `cells` stage in `check.py --extract` (width sweeps, plus a
  fits-the-terminal invariant on every dump); `check.py --tui`, 38 checks
  through a pseudo-terminal; unit tests for the renderer, forms, settings and
  transfers (ctest now runs nine). Both new gate kinds were shown to fail on
  a real defect before being trusted: the cells stage on a table that
  registered after its blit, the terminal suite on the Escape bug below.

### Where the plan had to change

- **termbox2 is pinned to a master commit, not a release.** v2.5.0 has no
  `tb_wcwidth`; it calls libc's `wcwidth()`, which depends on the locale. The
  4,321-line header this plan measured is master
  (`cdf62e9990d8`), and that is what is vendored.
- **termbox2 does not draw.** Measured on a virtual display: xterm draws the
  Devanagari conjunct क्ष and the Thai กำ in two cells, kitty in one, pyte in
  two. termbox advances by its own answer and repositions the cursor only
  after cells it considers wide, so wherever it and the terminal disagree the
  rest of the row shifts. `tui_screen.c` writes the output instead, through
  `tb_send`, and repositions after every cluster that is not a single
  one-cell code point: a disagreement costs at most a one-cell gap. termbox
  keeps what it was chosen for — raw mode, key and SGR mouse decoding, resize,
  restoring the terminal.
- **Escape.** termbox's ALT input mode never delivers a lone Escape; it only
  marks the next key as Alt. Escape is how every mode here is left, so the
  shell runs in ESC mode and rebuilds Alt itself: an Escape with a key behind
  it in the same read is the chord.
- **Cluster width.** A wide base makes a cluster two cells; otherwise its code
  points' widths add up (how xterm draws a conjunct); an emoji presentation
  selector widens a narrow base.
- **Colours** come from the reader page's own CSS variables, so the three
  themes are the reader's and not a copy that could drift. In a terminal
  without direct colour the shell uses the terminal's own colours.
- **Media transfer** is the engine run with `--fetch-media`, as on the
  desktop, not libcurl on a thread.
- **Each run writes into one private directory** (0700). A clean exit removes
  it; a crash unlinks the session's cookie file from the signal handler, the
  one thing there worth that care.
- **`DISTILL_AUDIO_BACKEND=null`** plays through miniaudio's null backend, in
  real time, silently. It exists so the terminal suite can exercise playback
  on machines with no sound card and without making a sound on ones with one.

### Checked by hand, not gated

Images, in kitty and in xterm (`-ti 340`) on a virtual display: the probe
chose the right protocol in each, the image drew fitted and centred, and
leaving the viewer removed it. tmux answers the device-attributes query with
sixel support and gets sixel. No other terminal was tried; the capability
tables' long tail is untested.

### Footprint (H)

Measured on the Cairo-less build (engine and terminal shell together). No
GLib, GTK, Pango, fontconfig, Cairo, FreeType, HarfBuzz, litehtml or libstdc++
is linked.

| counted as | MiB |
| --- | ---: |
| Phase 0's convention: QuickJS-ng + Lexbor | 4.94 |
| Distill's own shared dependencies, without the network stack | ≈ 6.5 |
| every shared library except glibc | 20.86 |

The last line is over PLAN.md's 8 MB, and about 14 MiB of it is this
distribution's libcurl and what it pulls in — OpenSSL alone is 5.65 MiB, then
Kerberos, HTTP/3, libssh2, brotli, zstd and libunistring. Which reading the
budget intends is the owner's decision; meeting the literal one means a curl
built for the target (a small TLS library, none of the rest), which is
packaging rather than code. The dangling `PHASE0.md` link in PLAN.md is where
the method should have been recorded.

### Fixed on the way, pre-existing

- `--fetch-media` saved an HTTP error page as the file it was asked for: the
  previous binary exits 0 having written a 404 page as an MP3, this one exits
  2 and writes nothing. Both shells reported "Saved".
- The desktop's fetched audio went to a name its temporary file had been
  unlinked from, so the engine recreated it world-readable, and the previous
  track's file was never removed. The shared transfer keeps it 0600 and
  removes it.
- `media_audio_available()` was documented as a probe and is not one; the
  comment now says what it does.

### Found on the way, not fixed here

- Extraction collapses a line break between two CJK characters into a space
  before the reader page is written (`25-cjk-linebreak`: 「表示する ソフト」),
  so both front ends show it. The cell renderer applies CSS's segment-break
  rule to reader pages that keep their newlines; the fix for this one belongs
  upstream.
- The render gate draws CJK as tofu: `external/fonts` has no CJK face. It
  passes on geometry.
- Enter in a form field submits without the form's default button, where HTML
  submits through it (`go=Search` is missing). The desktop has always done
  this; the terminal matches the desktop rather than diverging from it.
- Preserved byte-for-byte from the old exporter, and so present in both
  sinks: `** bold**` with the space inside the markers, a nested table's rows
  repeated, adjacent links run together. Links inside table cells arrive as
  text and cannot be followed.

### Not done

- Images inside the reader are one-line placeholders; they are shown in
  viewer mode. The layout already reserves rows for them (`image_rows`); the
  shell does not yet fetch thumbnails to fill them.
- A textarea is edited as one line.
- Bidirectional reordering, as planned.
- The desktop's play path was not driven end to end: it would have played
  through this machine's speakers. Its download path was, and the transfer
  underneath is the same.
