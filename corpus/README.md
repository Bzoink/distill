# Distill corpus

The ruler, built before the thing it measures.

[PLAN.md](../PLAN.md) makes this load-bearing: Phase 0's acceptance criterion is
*"every corpus page carries a CSS selector plus an expected substring that exists
only after the page's own script has run,"* and the A/B between Arm A, Arm B and
Arm B′ is decided on corpus pass rate. Until this exists, Phase 0 cannot start.

## Running it

```sh
./check.py                    # all gates, engine expectations
./check.py --extract          # Phase 1 extraction and golden-output gates
./check.py --render /tmp/png  # Phase 2 reader PNG smoke renders
./check.py --viewer /tmp/png  # Phase 2 media viewer smoke render
./check.py --app              # Phase 3 desktop-shell model smoke test
./check.py --tui              # Phase 6 terminal shell, driven through a pty
./check.py --bundles          # recorded saved-bundle smoke checks, never gates
./check.py --baseline         # validate the fixtures against a real browser
./check.py --id 07-churn      # one page
./check.py --keep-dom /tmp/d  # save each dumped DOM for inspection
./check.py --live             # also list the live rungs
```

`check.py` starts `serve.py` itself. The default mode drives the selected C23
headless engine and exits non-zero if any Phase 0 gate fails; `--extract` drives
the C23 Arm A extractor and checks the Phase 1 assertions and golden contracts.
These are CI conditions, not judgement calls. `serve.py` can also be run alone
(`./serve.py -p 8731`) to poke at a fixture by hand.

### Native form lab

Run the local server and open `http://127.0.0.1:8731/forms-lab.html` in
Distill with native forms enabled:

```sh
./serve.py -p 8731
../build/distill-gtk --url http://127.0.0.1:8731/forms-lab.html --native-forms
```

The lab covers GET and POST submission, hidden and disabled fields, text,
password, multiline text, radios, checkboxes, a select menu, required fields,
and reset controls. Every submission opens a local echo page showing the exact
successful fields received by the server.

For the session flow, follow the **Open the local sign-in form** link in that
lab. Sign in as `corpus` / `fixture-password`; the Account page confirms the
session and offers **Sign out**. Visiting Account again after logout shows
`SIGNED-OUT`.

`--extract` emits a versioned JSON contract containing the final URL and base
URL, shape/confidence, selected extractor, Readability metadata/content, block
text, tables, search results, forms, video metadata, media references, links,
and rejected URI counts. Its stable field checks live in
[`golden/`](golden/); the combined saved-page bundle under `local/bundles/` is
recorded for breadth and is never a gate.

`--render DIR` sends the same settled extraction result through the reader
template, litehtml layout, and the FreeType/HarfBuzz/Cairo paint backend. It
writes one PNG per corpus page and gates only that the renderer creates a
non-empty PNG with valid dimensions; visual quality remains a human review.

`--viewer DIR` runs the gallery fixture through the fit-to-width media viewer
path and checks its navigation surface produces a valid PNG.

The Phase 3 desktop shell is `build/distill-gtk`. Run it with `--url URL` to
open a page in the GTK reader, or run `./check.py --app` for the display-free
history/form-geometry/address-bar shell gate. The window provides an address bar, cached
back/forward history, a Cairo reader surface, native GTK form controls placed
from litehtml geometry, simple GET/POST submission, URI-validated external
hand-off, three reader themes, eight zoom levels, and a reading-list toggle.

Zoom is a layout setting, not a scale applied to the finished surface: the
shell passes `--zoom` to the engine and the page is laid out again at the new
size. Ctrl+plus and Ctrl+minus step through the levels the menu offers and
Ctrl+0 returns to 100%; `--self-test` covers the stepping and the fallback for
a saved level the menu no longer has.

The shell opens on `share/welcome.html`, which presents the browser and its
keys to a first-time reader. It is an ordinary local page: the shell navigates
to it, so it is fetched, extracted and laid out by the reader template like any
other, and reading it is a demonstration of the thing it describes. Settings ->
Start page saves the current page, the welcome page or blank under
`browser/start-page`; `--url` overrides it. The page is found through
`DISTILL_WELCOME_PAGE`, then the source tree, then the installed copy --
the same order the engine uses for its profiles -- and `--self-test` gates
both that search and what each setting resolves to.

Text selection is drag, double-click for a word, triple-click for a block,
Ctrl+A for the page, Ctrl+C to copy, and Escape to clear; a finished drag also
goes to the X11 primary selection. The runs come from the renderer -- litehtml
hands its container one per line with the box it filled -- and are collected
over the whole document rather than the painted band, so a selection reaches
text that has never been on screen. Where a character sits inside a run is a
shaping question, answered in cluster steps, which is what keeps a boundary
from landing inside an "fi" ligature or between the two bytes of an "o".
Copying rejoins what the layout broke: runs of one block become one paragraph,
a change of block is a line break. `distill-litehtml-geometry` gates the runs,
the cluster round-trip and that collecting text leaves link hit-testing alone;
`--self-test` gates what a selection turns into.

A load reports itself: `distill --progress` writes one `progress PERCENT PHASE`
line per step to stdout, and the shell spawns the engine with pipes so those
records arrive while the run is still going, turning into a spinner and a
percentage on the status line. The same channel carries one `location URL`
record once the engine knows where the page really came from, which is what
the address bar shows and what history stores: a redirect means the reader is
not on the address that was asked for, and reloading the address that merely
pointed there is a different request. `--progress` is refused with
`--dump-dom` and `--extract`, whose artifact is stdout itself. `--self-test`
covers the parse of both records, including ones split across reads, and
checks that every phase the engine names has a message of its own.

A bare hostname typed in the address bar gets `https://` from the shell, not
from the reader, and the shell says so by passing `--https-optional`. A host
that serves no TLS at all -- kp.se is one -- then fails before there is any
response to read or redirect to follow, and the engine retries that one
request over http. Only that: an explicit `https://` address, or one that came
from a link, is an instruction and stands, because stepping it down to
plaintext would hand the page to anyone able to break the handshake. The
retry is limited to a connection that never opened, so a certificate the run
distrusts still fails as one. The default `check.py` mode gates it against the
corpus server, which speaks http and nothing else and so has exactly that
shape; `--self-test` gates which inputs count as a guess.

The page width follows the window between 480 and 760 design pixels, both
scaled by zoom, and the page is centred on the dark desk either side of it.
Above the span a wider window only adds margin, since the measure is capped;
below it the window scrolls. Resizing re-lays out the template the shell
already has rather than fetching anything, and keeps the reading position
across the reflow. `--self-test` covers the span; the reader template itself
is width-agnostic, so `--render --width N` shows any point in it.

The address bar takes either a URL or a search. The rule is the one browsers
converged on, because users have already learned it: a scheme, a path, or a
dotted host is somewhere to go, and a bare word or anything containing a space
is something to look for. `dn.se` becomes `https://dn.se`; `kingfisher
hunting` becomes a search; `3.14` is a version number rather than a host;
loopback names get `http`, having no certificate to present. `--self-test`
covers each of these cases, and caught the version-number one.

Search goes to DuckDuckGo's HTML endpoint, chosen on measured behaviour rather
than preference. It is server-rendered, needs no JavaScript, and wraps results
in a `result` class, which is the signal the extractor's `search-results`
shape keys on. On the same query it produced 27 structured result records
where Mojeek, Brave, Ecosia, Startpage and SearX produced none, and Marginalia
rate-limited the request. `DISTILL_SEARCH_URL` overrides it; the encoded query
is appended to whatever it is set to.

DuckDuckGo hands out results as `/l/?uddg=<target>` redirects, and that
endpoint does not resolve here — it answers 400 without its token and returns
nothing with it, so every result would be a dead end. The destination is
already in the link, so the shell reads it instead of following the wrapper.

For a single page, `build/distill --url URL --viewer /tmp/media.png
--media-index 0 --save-media /tmp/original.bin` renders one media reference and
saves its original fetched bytes without decoding or rewriting them.

**Validate with `--baseline` before trusting a failure.** A fixture bug and a
binding bug look identical, and these fixtures are written to be hard. As of the
last run, `--baseline` is fully green.

## Two kinds of local fixture

PLAN.md originally described the local corpus as "saved SPA bundles served from
localhost." That conflates two jobs, and only one of them is Phase 0's:

- **Isolating fixtures** — hand-written, one capability each, unambiguous
  assertion. When one fails you know exactly what broke. *These are the gates,
  and they are what `local/*.html` contains.*
- **Realistic bundles** — whole saved pages. They test breadth and the
  interaction between features, which is where a real SPA's mess shows up. A
  failure names no cause, so they are recorded, not gated. `local/bundles/` is
  the slot; it is empty until Phase 1.

## The ladder

Each rung adds one capability over the last and names the PLAN.md section it
exists to test.

| Rung | Tests | Plan section |
|---|---|---|
| `00-static` | Control. No scripts at all | Minimal computed style |
| `01-sync-dom` | Parse-time DOM construction, `innerHTML` splice, `classList`, `dataset` | DOM ↔ JS |
| `02-lifecycle` | Script order, `defer`/`async`, `DOMContentLoaded`, `load` | Browser globals and document lifecycle |
| `03-settle` | What must be waited for: timers, microtasks, in-flight requests | Settle policy |
| `03b-hang` | What must *not* be: a hung request, a never-resolving promise | Settle policy |
| `04-fetch-render` | `fetch` + `async`/`await` + XHR rendering from JSON | Network ↔ JS bridge |
| `05-geometry` | `IntersectionObserver`, `getBoundingClientRect`, `getComputedStyle` | What we tell the page about layout |
| `06-storage-env` | `localStorage`, cookies, `navigator`, `matchMedia`, `history` | Browser globals |
| `07-churn` | DOM churn, node identity, detached nodes, `MutationObserver` | Arm A: node lifetime |
| `08-messy` | Undeclared encoding, `<base href>`, hostile schemes, dynamic `import()` | Step 4c |
| `08b-charset-lie` | An open decision, recorded not settled. Not a gate | Step 4c |
| `09-article` | Post-JS article extraction through Readability | Phase 1 extraction breadth |
| `10-docs` | Headings, code blocks, structured-text fallback | Phase 1 extraction breadth |
| `11-table` | Caption, headers, and cell data | Phase 1 extraction breadth |
| `12-search` | Repeated result records and mail links | Phase 1 extraction breadth |
| `13-form` | Action, method, labels, and hidden fields | Phase 1 extraction breadth |
| `14-video` | Embed and OpenGraph video metadata | Phase 1 extraction breadth |
| `15-gallery` | Decorative/content image classification | Phase 1 extraction breadth |
| `16-redirect` | Effective URL after redirect and base resolution | Phase 1 extraction breadth |
| `17-empty-async` | Empty async script response must not crash evaluation | Browser globals and document lifecycle |
| `18-network-contract` | Cookies, page-supplied headers, POST bodies, response headers, redirects, XHR | Network ↔ JS bridge |
| `19-module-graph` | Nested module resolution against the importer's URL; `nomodule` exclusion | Modern module and network behavior |
| `20-structured-data` | JSON-LD harvested from a page that never hydrates; malformed block reported | Phase 1 extraction breadth |
| `21-challenge-shell` | A recognised challenge is classified before its scripts spend the settle budget | Access classification |
| `22-challenge-vendor-scripts` | A working page that names bot-mitigation vendors must not be misclassified | Access classification |
| `23-composite-shape` | Shape selection when form, table, image and embed signals all appear on one article | Phase 1 extraction breadth |
| `24-rtl-arabic` | Right-to-left direction and language metadata; Arabic text preserved | Writing systems |
| `25-cjk-linebreak` | Japanese and Chinese text with no inter-word spaces | Writing systems |
| `26-complex-script` | Thai with no word delimiter; Devanagari cluster reordering | Writing systems |
| `27-login-session` | A login performed in one process and used by another | Sessions |
| `28-link-index` | A front page: document title, links on blocks, render sized to content | Phase 1 extraction breadth |
| `29-main-content` | A permalink into a stream: the requested item is found, deduplicated, and read in document order | Phase 1 extraction breadth |

Assertion stages are `dom` (the post-JS DOM), `extract` (the extraction JSON,
with `path`) and `template` (the reader HTML, as plain substrings). A page that
carries `template` assertions costs one extra engine run, and only that page.

`00` is the control: it has no scripts, so a failure there is never a binding
bug. `07` is where a raw-pointer binding over Lexbor's arena-allocated nodes
crashes — better here than on a live site. `08` deliberately resembles the top of
the live ladder while still naming its own cause on failure.

`23` exists because every other extraction fixture is clean in a way no real
page is. Each presents one shape — a table, a form, a gallery — so nothing ever
asked the classifier what to do when the signals compete. Real pages always
have them competing, and the classifier was deciding on whichever element it
checked first: an encyclopaedia article carrying a site search box was
classified as a search form and rendered as one, with the article body
discarded. `23` is an article that also carries a search form, a data table,
four images, and an iframe, and asserts that the prose wins.

That is the isolating corpus's structural blind spot, and it is worth
remembering when adding fixtures: a capability can be correct in isolation and
wrong in combination, and only a composite page catches it.

`21` and `22` are a matched pair and only mean something together. `21` asserts
that a challenge shell is recognised; `22` asserts that naming a bot-mitigation
vendor is not by itself evidence of one. A detector that passes `21` alone is
easy to write and wrong: the first version of ours matched Airbnb and Product
Hunt, which serve full pages while naming those vendors, and it would have
skipped their scripts. `22` is deliberately a small document so that the
detector's size guard cannot rescue it — only the choice of markers can.

## Front pages

`28-link-index` is a front page rather than an article: headlines that are
themselves links, and almost no prose. Three defects showed up together on
dn.se in that shape, and none of them was visible on any existing fixture.

- Readability finds nothing on a page with no article, so the reader had **no
  title at all** and displayed "Untitled page" — while the document said what
  it was in `<title>` the whole time. Extraction now carries a top-level
  `title` taken from the document, and Readability's own title is preferred
  only when it has one.
- Blocks carried their text but not the link around them, so headlines
  rendered as plain words. On a front page the link *is* the information: a
  headline you cannot follow is not a front page. Blocks now carry `href`,
  taken from the nearest wrapping anchor or a single contained one.
- The page was rendered into a fixed-height surface, which dropped everything
  past the bottom. The result did not look truncated; it looked finished and
  simply stopped. `--height 0` now sizes the render to the laid-out content,
  bounded, and the shell sizes its scroll area from the surface it gets back.

A page may declare `render_fit` to be rendered that way and
`min_render_height` to assert it grew rather than cropped. The reader now
recognises its dense linked-heading stream and applies index typography, so
the fixture is deliberately shorter than the old article-heading layout while
still asserting that the full page was rendered.

## Permalinks into a stream

`29-main-content` is a liveblog: seven posts in one document, reached by the
permalink of one of them. Nothing in the isolating corpus had that shape, and
the shape breaks the assumption every extractor here rests on — that the
document *is* the thing the reader asked for.

On a DN Direkt permalink the requested post was 851 characters out of 34,401,
and the reader showed all of it: the site navigation, fifty other headlines,
the footer, and the bodies of forty-nine posts the reader had not asked for.
Readability cannot separate them and it is not a defect in Readability: it
scores the densest container, and the densest container is the stream.

What settles it is that the page says so itself. schema.org has a vocabulary
for this — a `LiveBlogPosting` whose `liveBlogUpdate` entries each carry a
`mainEntityOfPage` and a `hasPart` naming the region:

```json
"mainEntityOfPage": { "@id": "https://www.dn.se/direkt/2026-09-15/sheeran-.../" },
"hasPart": [{ "cssSelector": ".direkt-post[data-id=\"dn.ifragasatt.39868f81-...\"]" }]
```

Match the `@id` against the URL that was requested and the page hands over the
answer. Extraction reports it as `main_content`, the reader shows it first, and
the rest of the page follows under a divider.

Three details the fixture exists to hold down, each of which was wrong in a
first version:

- **The item commonly appears twice** — pinned at the top of the stream and
  again in its chronological place, with the same `data-id`, so one selector
  matches both. Every match counts as main content, which is also what removes
  the duplicate from the remainder. The fixture asserts `matches` is 2.
- **The page-level block list is grouped by tag**, which is harmless for a
  stream of headlines and wrong for one item: it would print the headline, then
  the sub-heading, then all the paragraphs. `main_content.blocks` is walked in
  document order instead, and the fixture asserts a `h3` in third position to
  say so.
- **Promotion is for an item inside something larger**, which is a question of
  proportion, not of whether a locator matched. Publishers mark the free part
  of a paywalled article with the same `hasPart` selector, so without a guard
  an ordinary article would arrive here as often as a stream does and would
  lose Readability's cleanup for a raw block walk. A region holding more than
  half the page's prose is treated as being the page. That is why the fixture
  is a stream of seven and not of three: with three posts the requested one is
  most of the document, and refusing to promote it is the correct answer.

Without a selector there is a second route — an `og:title` that matches the
heading of one entry among several. The entry may be an `<article>`, or a
semantic liveblog wrapper such as `li[data-testid="liveblog-entry"]`. It is a
fallback, and the same proportion guard applies to it.

## Media references and viewer mode

`corpus/local/images/` holds real image files, not names. Three formats with
three decoders behind them -- JPEG, PNG with an alpha channel, and an SVG that
deliberately has no decoder at all -- because until they existed `15-gallery`
referenced files that were not there and every assertion about media was an
assertion about a URL string.

The reader does not fetch images. A placeholder is an activation target
carrying `distill-media:<the image's own URL>`, and only activating one costs
a request. That is the plan's rule and it is also what keeps the renderer free
of a network: the shell has no HTTP client, so the engine fetches on its behalf
with `--fetch-media`, which runs no scripts and does no extraction.

Two things the fixture gates that nothing else could:

- **`--viewer` now passes `--save-media`.** Without it the viewer renders a
  placeholder and the gate proves only that a dark rectangle can be drawn.
  With it the run covers the whole path -- fetch, sniff the format from the
  bytes, decode, paint -- and the saved file is checked to be an image.
- **A `template` assertion stage.** The reader HTML had no assertions at all:
  extraction cannot see it and the render stage only checks that a PNG came
  out with a plausible size. Whether a placeholder is something a reader can
  activate, and whether a URL is printed in a form that fits the column, exist
  only in that file.

The decoder and the drawing of it are covered by `ctest`, not here, and
deliberately at pixel level. The image path's first bug was an offset:
litehtml hands `draw_background` a resolved document position, and adding it
to the origin box drew every image at twice its own offset. That looks like a
partly clipped image rather than a wrong coordinate, and a smoke test that
asked only "did anything get drawn" passed the whole time.

## Sessions

`27-login-session` and `33-logout-session` exercise what happens *between*
runs. Each navigation is a separate engine process, so a session that lives in
one process is no session at all: sign in, click a link, and you are signed
out.

The server serves a real two-step login. `GET /login` issues a form cookie and
a one-time token; `POST /login` is accepted only if the credentials and that
token arrive together; `/account` returns `SIGNED-IN-AS-corpus` only when a
valid session cookie is present, and `401 SIGNED-OUT` otherwise. The two steps
run as separate processes sharing nothing but a `--session-file`, and the
assertion is simply that the third process reaches the gated page.

`33-logout-session` signs in, posts the Account page's sign-out form, and then
opens Account in a fourth process. It passes only when the cookie expiry sent
by `/logout` survives that process boundary and Account reports `SIGNED-OUT`.

The fixture fails without its `session` steps — dropping them yields
`SIGNED-OUT` — so it tests the mechanism rather than the server.

Two safety properties are deliberately built into the path this gate
exercises, because they are easy to get wrong and invisible when wrong:

- **Credentials never enter the argument vector.** `argv` is readable from the
  process table and lands in shell history. The harness writes the POST body to
  a private file and passes `--request-data-file`; so does the desktop shell
  when it submits a form. `--request-data` remains for bodies that are not
  secrets.
- **The session file is private or refused.** It holds live credentials for
  every site in it. It is created `0600`, and a run against a file others can
  read fails with an explanation rather than writing cookies into it.

## Writing systems

`24`-`26` are the first fixtures in this corpus containing any non-Latin text.
That gap mattered: the stack carries HarfBuzz for shaping and libunibreak for
line-break opportunities, and nothing exercised either beyond what
space-separated Latin needs.

They assert what is assertable — that direction and language metadata survive
extraction, and that the text itself comes through intact. Correct *rendering*
of these scripts cannot be asserted with a substring; judging it needs a
reference image. What they did surface immediately, by being rendered and
looked at:

| Observation | Status |
|---|---|
| RTL articles were laid out flush left | Fixed: the reader template now carries `dir` and `lang` from the extraction and mirrors its physical-side rules |
| CJK, Thai, and Devanagari render as tofu | Open. `select_font_file` picks one face by family name and never asks whether it contains the glyphs, and `--font-dir` suppresses the system font scan entirely. Per-run fallback needs a face list and run itemisation |
| Thai paragraphs overflow the column | Open. With no inter-word spaces and no dictionary segmentation, the paragraph is one unbreakable run |
| Mixed-direction runs are unverified | Open. There is no Unicode Bidirectional Algorithm in the stack; HarfBuzz orders a single run but does not reorder between runs |

None of these is a reason to hold the fixtures back. Extraction is correct and
gated today, and the render gaps are now recorded against a page that
demonstrates them instead of being invisible.

## Where we must beat the browser

Three fixtures require us to behave *differently* from a conformant browser, each
on purpose. This is the corpus restating the plan's design choices as testable
facts, and it is why `--baseline` and the default mode expect different things.

| Fixture | A real browser | Us | Why |
|---|---|---|---|
| `03b-hang` | waits forever | caps and extracts | A browser has no settle policy; the cap is the thing under test |
| `05-geometry` | leaves below-fold lazy content unloaded | loads it | The fiction reports everything intersecting — that is *how* lazy content loads eagerly |
| `08-messy` | falls back to windows-1252, mojibake | detects UTF-8 | Only a BOM saves Chrome here. Mojibake is never the useful outcome for a reader |

The third is an assumption worth challenging: it says an undeclared page should be
sniffed. The harder case — an explicit declaration that *contradicts* the bytes —
is `08b-charset-lie.html`, deliberately left undecided and ungated. See
`open_decisions` in `manifest.json`.

## The terminal

Two gates, for two different things.

**What the terminal lays out** is a `cells` assertion, checked by `--extract`
beside `markdown`. The engine lays the reader page out in character cells
(`distill --dump-cells FILE --columns N`) with no terminal involved, and the
assertion is a substring of that text. `columns` is one width or a list, and a
list is the point: wrapping bugs hide at one width.

```json
{"stage": "cells", "columns": [40, 80, 120], "expect": "A careful reader"}
```

Every cells dump is also checked, at every width a page asks for, to fit its
terminal — measured with python-wcwidth where it is installed rather than by
the engine's own width table, so the check is not the renderer grading itself.

**The shell around it** is `--tui`: `corpus/tui_suite.py` runs
`build/distill-tui` under a pseudo-terminal and reads the screen through
[pyte](https://github.com/selectel/pyte) (`python-pyte`). It covers keys, the
address prompt, history, both ways of submitting a form, a POST login whose
session crosses engine runs, resize, a table's sideways scroll, the mouse,
audio through the null backend (`DISTILL_AUDIO_BACKEND=null`: real time, no
device, no sound), and leaving the terminal as it was found — after `q`, and
after `SIGTERM` and `SIGSEGV`. Whether kitty or sixel images actually appear
needs a real terminal and is not gated; see `src/documents/TUI_PLAN.md`.

## The assertion contract

`manifest.json` is the machine-readable contract. Each assertion carries:

- `stage` — `dom` (Phase 0, checked against the post-JS DOM at settle time),
  `extract` (Phase 1, checked against extractor output), or one of the reader
  page's forms: `template`, `markdown`, `cells` (see [The terminal](#the-terminal)).
  Only `dom` assertions gate Phase 0; `check.py` reports the others as pending
  and checks them under `--extract`.
- `selector` + `expect` / `expect_absent` — the contract PLAN.md specifies.
  Only `#id` selectors are supported, on purpose: every fixture is written to
  make its assertion unambiguous.
- `browser_expect` — present only where we deliberately diverge, as above.

A page may carry `session`, a list of requests to perform before the page
itself, all sharing one private cookie file that is then used for the page.
Each step takes a `path`, and optionally `method`, `data`, and `expect_exit`.
Steps run as separate engine processes, which is the property under test.

A page may also carry `expect_exit`, a process status the engine is required to
report. It defaults to `0`. `21-challenge-shell` sets `4`: a recognised
challenge still serializes and extracts its shell, so the run produces output,
but the status separates it from a run that reached content. `check.py` treats
any other status as a capture failure, as before.

A page may assert what its run costs, checked by `--extract` through the
engine's `DISTILL_STATS` line: `max_rss_mib` for peak memory, `max_run_ms`
for wall time and `max_fetches_at_once` for transfers attached together. These regressions are invisible to every content assertion,
because the output is identical and only the process is larger or slower.
`42-stylesheet-scale` pins extraction memory (339 MiB before its fix, 45 MiB
after) and `44-blocking-script-chain` pins the settle cap (15 s before, 5 s
after), and `45-fetch-concurrency` pins the fetch queue (12 at once before, 8
after). Keep a budget well above the measured value and well below the broken
one, so it catches the defect and not the machine.

## Other modes

- `--in-process` runs the engine as a library, the whole corpus in one
  `build/distill-host` process with each navigation on its own thread
  (`src/engine_host.h`). With `--extract` it runs the golden gates. Budgets
  report "pending" there, since they measure a process. A run that leaves
  state behind for the next one fails the next one.
- `--stack` models Muroxi's stacks: every gated page extracts with the engine
  in an 8 MiB stack, and the terminal suite runs with the shell in 64 KiB.
  Two negative controls must fail: the deep-recursion page with the JS limit
  set to the whole stack, and the shell in 16 KiB.
- `--paint` paints every gated page's reader page, and `paint/primitives.html`
  (every painter operation on one hand-written page), with the software
  painter and with the Cairo reference, and compares pixels: a mean
  difference of at most 0.5 per channel, and no connected region over 16
  pixels differing by more than 32. A painter with a deliberate defect must
  fail it.
- The default mode also checks that an engine run writes no temporary files,
  and that TLS refused under an untrusted clock (`DISTILL_TEST_UNTRUSTED_TIME=1`)
  exits 5.

## Live rungs

Five rungs in ascending difficulty, in `live.md` and `manifest.json`. **None is a
gate.** Per PLAN.md's reasoning about curl's TLS/HTTP2 fingerprint, a live
failure is not attributable, so live results are recorded rather than asserted.

`www.dn.se` is the top rung and is explicitly a **diagnostic**: its article body
is absent from the HTML behind both a Didomi consent wall and a paywall, so a
failure there has three possible causes. Being able to say *which one fired* is
the result — that is the whole conflation Phase 0's design exists to prevent.
