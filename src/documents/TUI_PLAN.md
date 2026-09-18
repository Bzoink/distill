# TUI plan

Status: proposed. Phase 6 of [PLAN.md](PLAN.md), and the last visualization
target.

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
- **`tb_wcwidth`** and the UTF-8 helpers give the cell-measuring callback the
  breaker needs, including the double-width behaviour the CJK fixtures already
  exercise.

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

## Architecture

### Process model

Unchanged, and that is the argument for it. `build/distill` stays one-shot: the
TUI spawns it per navigation and reads back `--template`, exactly as the GTK
shell does. Anything that must survive a navigation already lives in a file —
cookies through `--session-file`, the page itself through `--template`.

The TUI is a long-lived process, so it is where playback lives, as the media
plan requires.

### The reader-page walker

The load-bearing idea. The Markdown exporter already walks the reader template
and emits a linear styled document: headings, paragraphs, lists, tables, links,
cards, form controls, dropping what only the shell understands. **A terminal
renderer is that same walk with a different sink.**

So factor the walk out of the Markdown exporter and give it a sink interface —
open a block, emit a run of text with a style, close a block — with two
implementations:

- the Markdown sink, which is the code that exists today;
- the cell sink, which wraps each run with `d_text_wrap_utf8` against a
  cell-measuring callback and writes cells.

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

### Settings

An outstanding gap this phase has to close. The GTK shell keeps preferences in
a `GKeyFile` at `~/.config/distill/settings.ini`, which is GLib, which the
minimal targets may not link. The file format is fine; the reader is not. A
small GLib-free INI reader over the same file lets both shells agree on theme,
start page and the video player setting the media plan defines.

## Delivery phases

### A — free the reader page

Move the reader-template family out from behind `DISTILL_RENDER`; confirm a
configure without litehtml still produces `--template` and `--markdown`. No
terminal code.

**Done when** a build with the renderer disabled passes the extraction and
markdown gates.

### B — the walker

Factor the reader-page walk out of the Markdown exporter behind a sink
interface; re-express the Markdown exporter in terms of it, unchanged in
output, proven by the existing markdown gates.

**Done when** the markdown gates pass against a walker with two sinks and one
of them is not yet written.

### C — text on screen

Vendor termbox2. Cell sink, cell-measuring callback into `d_text_wrap_utf8`,
scrolling, resize. Articles, docs and link indexes read end to end.

**Done when** a corpus article is readable in a terminal at three widths,
including the CJK and complex-script fixtures.

### D — navigation and interaction

`app_state.c` history, address entry, link focus and activation, section jumps,
tables with their own scroll axis, forms and submission.

**Done when** the corpus pages that exercise forms and search can be driven to
a result without leaving the terminal.

### E — media

Link the audio core and draw the transport. Then the capability probe, the
image encoder and viewer mode.

**Done when** a page's audio plays from a terminal over SSH, which is the case
that motivated putting audio in scope at all.

### F — settings and packaging

The GLib-free settings reader, shared with the GTK shell. Confirm the link line:
libunibreak, libcurl, libpng, libjpeg, libwebp, vendored termbox2 and miniaudio.
No GLib, GObject, Pango, fontconfig, cairo, FreeType, HarfBuzz or litehtml.

**Done when** the footprint is measured against PLAN.md's budget rather than
asserted to be inside it.

## Testing and gates

The corpus is HTML and assertions, and none of it assumes a window; a terminal
front end should be the easiest target to gate honestly, not the hardest.

- **A cell-dump mode.** The TUI writes its cell grid as plain text on demand,
  the way `--template` writes HTML. That makes a terminal rendering a
  deterministic artifact a gate can assert on: a `tui` stage in `check.py`
  beside the `markdown` one, asserting on the text at a fixed width.
- **Width sweeps.** Assert the same page at 40, 80 and 120 columns. Wrapping
  bugs hide at one width.
- **The scripts that already exist.** `--self-test` conventions carry over: key
  handling, history and the address bar are model-level and testable without a
  terminal.
- **What cannot be gated**, stated rather than papered over: real terminal
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
