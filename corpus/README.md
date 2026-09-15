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
history/form-geometry shell gate. The window provides an address bar, cached
back/forward history, a Cairo reader surface, native GTK form controls placed
from litehtml geometry, simple GET/POST submission, URI-validated external
hand-off, three reader themes, and a reading-list toggle.

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

`00` is the control: it has no scripts, so a failure there is never a binding
bug. `07` is where a raw-pointer binding over Lexbor's arena-allocated nodes
crashes — better here than on a live site. `08` deliberately resembles the top of
the live ladder while still naming its own cause on failure.

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

## The assertion contract

`manifest.json` is the machine-readable contract. Each assertion carries:

- `stage` — `dom` (Phase 0, checked against the post-JS DOM at settle time) or
  `extract` (Phase 1, checked against extractor output). Only `dom` assertions
  gate Phase 0; `check.py` reports `extract` ones as pending.
- `selector` + `expect` / `expect_absent` — the contract PLAN.md specifies.
  Only `#id` selectors are supported, on purpose: every fixture is written to
  make its assertion unambiguous.
- `browser_expect` — present only where we deliberately diverge, as above.

## Live rungs

Five rungs in ascending difficulty, in `live.md` and `manifest.json`. **None is a
gate.** Per PLAN.md's reasoning about curl's TLS/HTTP2 fingerprint, a live
failure is not attributable, so live results are recorded rather than asserted.

`www.dn.se` is the top rung and is explicitly a **diagnostic**: its article body
is absent from the HTML behind both a Didomi consent wall and a paywall, so a
failure there has three possible causes. Being able to say *which one fired* is
the result — that is the whole conflation Phase 0's design exists to prevent.
