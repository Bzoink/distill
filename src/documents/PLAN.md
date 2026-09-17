# Distill

*A small, C23-rooted stack that runs a page's real JavaScript against a real DOM, pulls out what actually matters, and renders it back — clearly, consistently, on our own terms.*

**Status:** Phase 3 complete — Phase 4 not started
**Drafted:** 2026-09-14 · **Revised:** 2026-09-15 (Phase 3 implementation)
**Origin:** NetSurf JS/rendering gap analysis (see `../netsurf`)
**Rendered version:** https://claude.ai/code/artifact/fcbd8e5f-f6dc-4abd-a2a4-e7282070cf4a *(predates this revision)*

---

## Why

NetSurf (and small browsers like it) can no longer read most of today's web. Its JavaScript engine, Duktape, tops out around ES5.1 syntax and has no `Promise`, no `fetch`/`XMLHttpRequest`, and no working `localStorage` — so any site that loads its own content dynamically, which is most of the web now, fails partway through and shows nothing.

Patching that engine in place turned out to be the wrong shape of fix: the syntax gap and the missing network APIs both point toward replacing the engine, and once you're doing that, chasing pixel-perfect layout for arbitrary sites is the wrong goal to chase at all. What we actually want is narrower and more achievable: get the real content out of a page, and present it well ourselves.

"Present it well" has to cover more than articles, though. A newspaper, a blog, a docs site, a search engine, a forum thread all need reading — but a search engine and a forum also need the user to act: submit a query, post a reply, follow a `mailto:` link. Those are still native HTML constructs (`<form>`, `<a href="mailto:...">`), not bespoke site UI, so extracting and re-rendering them fits the same model as extracting an article body — see [Interaction](#interaction) below.

## What this deliberately isn't

Scope cuts made up front, so effort doesn't quietly drift toward rebuilding a general-purpose browser.

1. **Not a layout engine for other people's CSS.** No flexbox/grid solver for arbitrary sites — that's the single hardest, most open-ended part of any browser, and we don't need it if we render our own template instead of theirs. This holds for interactive elements too: a native `<select>` or `<input>` gets extracted and re-rendered as *our* control, same as an article's headings and paragraphs; a bespoke JS-driven control (a custom dropdown, a date picker built from `div`s) is source-site UI, not a native form control, and is out of scope the same way its CSS is. Note the one honest consequence of this cut, spelled out under [What we tell the page about layout](#what-we-tell-the-page-about-layout): with no layout solver, the geometry APIs can't tell the truth, so we give them a deliberate, consistent fiction instead.
2. **Not a media engine.** No WebGL, no video/audio pipeline, no WebAssembly, no in-app playback. Text, structure, and images are the target. A page that embeds video (YouTube/Vimeo iframe, `<video>` tag, OpenGraph/oEmbed video metadata) is still content worth surfacing, though: we extract what metadata is available — title, thumbnail, duration, source URL — and present it as a card with a hand-off action to an external player, rather than trying to decode or play it ourselves.
3. **Not a bot-challenge solver.** Cloudflare-style JS challenges fingerprint the whole browser and are an adversarial problem, not a capability gap. Sites gated this way are out of scope, not a bug to fix.
4. **Not full spec coverage.** We implement the DOM/JS surface real pages actually exercise, driven by what Phase 0 telemetry hits — not the complete WHATWG/W3C surface NetSurf's own gap list runs to 1,500+ items long. The same discipline governs typography: we shape the scripts our corpus actually contains, not the Unicode repertoire.

## Considered and rejected

Decisions already made, recorded so they stop being re-litigated. Where the reasoning was wrong the first time, the corrected reasoning is recorded too — recording a bad reason is how a settled decision comes back.

**A full browser engine (WebKitGTK/CEF) with Readability injected — rejected.** It would delete steps 1–3 of the pipeline wholesale, for roughly an order of magnitude less code, and on a desktop-only project it would be the obviously correct call. It is rejected because the small C23 stack *is* the project: the NetSurf lineage, modest target hardware, and the framebuffer target (Phase 5) all presuppose it. Embedding WebKit contradicts the premise rather than serving it. This is a settled constraint, not an open trade-off — which is precisely what justifies the bespoke work in the DOM↔JS layer below. Nothing else in this plan would justify it.

**Bespoke C bindings as a foregone conclusion — deferred, not adopted.** The binding layer is the largest unscoped item here, and there is a real alternative (a JS-side DOM running inside the engine) that could delete most of it. Phase 0 runs both as an A/B before either is committed to. Recorded here so the alternative isn't quietly forgotten once C work starts. See also the [language boundary](#the-language-boundary-does-this-stay-a-c23-project) that A/B actually decides.

**libuv — cut.** We don't need a general event loop. We need a microtask drain, a timer queue, and socket polling for `curl_multi` — that's `poll()` and a few hundred lines. libuv costs roughly a megabyte and imposes pthreads for work we don't do. (An earlier draft claimed quickjs-ng's runtime depends on libuv; that's the `qjs` CLI's os/timer module, not the engine core. The engine has no such dependency.)

**Pango — rejected, and with it GLib, GObject and fontconfig.** Every capability Pango was chosen for is provided by something Pango itself delegates to: shaping to HarfBuzz, rasterization and metrics to FreeType, hyphenation to libhyphen. Pango's own contribution is itemization, line breaking, bidi integration, and font selection — of which font selection is fontconfig (droppable, since we ship our own fonts) and **line breaking is something we want to own anyway**, because the TUI target wraps in terminal cells, not pixels. So Pango's price — GLib, GObject, GModule, ~3.5 MB, and a runtime philosophy — buys integration we'd replace by Phase 6 regardless. Details and the replacement stack: [Typography](#typography--themes).

**WebSocket in the network bridge — cut.** `fetch` and `XMLHttpRequest` stay. WebSocket returns only if Phase 0/1 telemetry shows a target page actually needs it.

**notcurses — not adopted; decided in Phase 6.** It's heavy for what we'd use it for (capability detection and escape output), and it brings an input model that would compete with ours. A kitty/sixel encoder plus a `$TERM` capability probe is a few hundred lines. Since the TUI is text-only first, this decision isn't needed until images arrive.

**NetSurf source reuse — cut, for a narrower reason than first recorded.** The earlier draft said "no NetSurf code," which over-generalises: NetSurf's *browser* (including the framebuffer frontend) is GPL-2.0 and stays out, but several NetSurf *libraries* — notably **libdom and libcss — are MIT** and separate from the browser core. The decision still stands: libdom is stale and its API awkward, and Lexbor's parser is better. But libdom is the one C DOM explicitly designed to be bound to a JS engine, and NetSurf's `dukky` glue is directly relevant prior art for Arm A — **read both before writing bindings from scratch**, especially for how they handle node lifetime. Image decoding uses libpng/libjpeg/libwebp linked directly; the fb frontend is read-only reference. See [Licensing](#licensing-position).

### Budget

The hard constraint above makes footprint a *design input*, not something to measure later. Provisional targets, set now so Phase 0 and Phase 5 have something concrete to fail against — adjust them once real numbers exist, but don't delete them:

**Runtime**
- **≤ 150 MB peak RSS** for a loaded, extracted, rendered article page.
- **≤ 3 s** from fetch start to painted output for a typical article on broadband, including script execution.
- **≤ 5 s** hard script-execution ceiling per page, enforced (see [Settle policy](#settle-policy)).

**Size and dependencies** — the half of the stack the first draft never budgeted, and the half where "small and portable" actually gets tested:
- **≤ 8 MB** total shared-library footprint on the framebuffer target, excluding bundled font data.
- **Hard rule for framebuffer/TUI/minimal targets: no GLib, GObject, Pango, or fontconfig linked.** The Phase 3 desktop shell is the explicit GTK/GLib exception for native form widgets; fontconfig may still be `dlopen`ed on desktop as an optional convenience, never linked.
- Every new dependency is named in the [Reuse ledger](#reuse-ledger) with its transitive tree understood, not just its own size.

These are also tiebreakers in the Phase 0 A/B: an arm that can't meet them isn't the arm.

## The stack

Every layer is an existing, small project — C23, except litehtml (C++) — other than the rows marked bespoke, which are the actual work. The C++ boundary is intentionally narrow: litehtml's adapter is the only production C++ component; the application, extraction pipeline, bridges, and paint backends are C23.

| Layer | Component | Status | Role |
|---|---|---|---|
| Network & TLS | libcurl | reused as-is | HTTP/HTTPS, redirects, cookies — no change needed |
| HTML parse & DOM | Lexbor | adopted as-is | Modern, actively-maintained HTML5 parser, DOM, CSS selectors, encoding detection, in pure C. **Its role depends on the Phase 0 outcome** — see the structural question below |
| JavaScript engine | QuickJS (quickjs-ng) | adopted as-is | Real ES2020 — `Promise`, `async`/`await`, classes, modules — replaces Duktape. quickjs-ng over upstream for active maintenance, newer ES coverage, and a shorter fix backlog. *(Verify rather than assume: check its actual `Intl` coverage, and don't record internal string representation as a tiebreaker without confirming it — both engines index by UTF-16 code unit, as the spec requires.)* |
| DOM ↔ JS | custom | **new, bespoke** | A DOM and a Window surface for QuickJS. Two candidate shapes — see Phase 0 |
| Network ↔ JS bridge | custom over libcurl | **new, bespoke** | `fetch`/`XMLHttpRequest`, resolving real Promises from curl callbacks |
| Scheduler | custom, `poll()` + `curl_multi` | **new, small** | Microtask drain, timer queue, socket readiness. A few hundred lines; see the libuv rejection above |
| Style resolution | custom over Lexbor CSS | **new, minimal** | `display`/`visibility` only — see below |
| Content extraction | Readability.js | runs unmodified | Mozilla's Reader View heuristic — one extractor among several, not the extraction stage itself |
| Box layout | litehtml | adopted as-is | Lays out *our own* reader template/theme HTML+CSS — never a source site's. The only credible embeddable HTML/CSS engine in this weight class; libstdc++ is its price and worth paying, since the alternative puts our actual deliverable on the critical path beside the DOM↔JS layer |
| Text shaping & breaking | HarfBuzz + FreeType + libunibreak, behind our own run shaper | **new, scoped small** | See [Typography](#typography--themes) |
| Paint | backend interface | **new, small** | Cairo on desktop; direct ARGB32 blitter on framebuffer; no raster path at all on TUI |

### The language boundary: does this stay a C23 project?

The Phase 0 A/B is usually described as choosing a binding shape. It isn't only that. If the JS-side-DOM arm wins, Lexbor has no remaining job — **litehtml parses HTML itself** (`document::createFromString`), so it can consume the serialized extraction output directly, and the C-side parser disappears from the stack. What's left is QuickJS + a JS DOM + litehtml + libcurl, with the only C23 written being the bridge, the shaper, and the shell.

That may well be the right answer for "small and easy." But it is a different project from the one the tagline describes, and it should be chosen deliberately rather than arrived at in month four. Phase 0 therefore measures a **third, no-Lexbor baseline** explicitly, so the question asked is "does Lexbor earn its place," not merely "which binding shape wins."

The implementation language is now explicit: bespoke production code is C23. litehtml stays because it is the selected box-layout engine, and its C++ adapter is an intentional exception at the rendering boundary. The Phase 0/1 application and extraction code has been ported to C23; the adapter exposes a C ABI so the C23 core never depends on C++ types.

One thing that leaves with Lexbor, and is easy to forget: **character-encoding detection.** Real pages routinely declare the wrong charset or none at all. Whoever replaces Lexbor inherits that job.

### What we tell the page about layout

Cutting the layout solver has one consequence the rest of the plan has to absorb: the DOM APIs that report geometry — `getBoundingClientRect`, `offsetWidth`/`offsetHeight`, `IntersectionObserver`, `scrollTo`, `getComputedStyle` — have nothing truthful to report. They are common in framework code and real sites gate content on them, so "not implemented" isn't an option.

So we implement a **deliberate, consistent fiction** rather than an error boundary:

- A fixed viewport (1280×1024), reported consistently by `innerWidth`/`innerHeight`/`screen`.
- Every element reports a plausible non-zero rect and is on-screen.
- `IntersectionObserver` fires *intersecting* for every observed target, immediately.
- `scroll*` calls are accepted and recorded, never a no-op that throws.

The payoff is more than crash-avoidance: "everything is visible" makes lazy-loaded images and infinite-scroll content load *eagerly*, which is exactly what an extractor wants. And it makes the real boundary narrow enough to write down: **JS that writes content works; JS that branches on measured geometry gets a plausible constant.** Pages that genuinely depend on measurement will misbehave in ways that look like content bugs, so Phase 0 telemetry flags geometry-API calls specifically, keeping that failure mode visible rather than mysterious.

### Browser globals and document lifecycle

The "Why" section names Duktape's missing `localStorage` and network APIs as the reason the web has become unreadable — so the replacement's answer to those specific gaps has to be enumerated, not left as "a DOM and a Window surface." All of this is Phase 0 shared work, common to both arms.

**Lifecycle and script loading — the part that decides whether a page produces content at all:**
- `readystatechange`, `DOMContentLoaded`, `load`. Most client-rendered content is gated on these firing.
- External `<script src>` fetched and executed **in document order**; `async`/`defer` honoured; inline modules; dynamic `import()`.

Getting these wrong looks identical to a missing DOM method, which is why they can't wait for Phase 1.

**Storage and identity:** `localStorage`, `sessionStorage`, `document.cookie` — cheap stubs with high unblocking value.

**Environment:** `navigator` (including the **User-Agent string**, which materially changes what HTML servers send — it's a content-fidelity setting, not a detail), `screen`, `location`, `history.pushState`/`replaceState`, `matchMedia`.

**Timing and misc:** `requestAnimationFrame`, `performance.now`, `crypto.getRandomValues`.

**Built-ins to verify rather than assume** — real pages hit these and engine coverage varies: `URL`/`URLSearchParams`, `TextEncoder`/`TextDecoder`, `Blob`, `FormData`, `AbortController`, and `Intl` (date/number formatting is common, and coverage differs between upstream QuickJS and quickjs-ng).

### Settle policy

"When is the page done?" has to be answered before the corpus means anything. Without a definition, both A/B arms produce run-to-run variance that looks like signal, and the 3 s budget and 5 s ceiling have nothing to measure against.

**Settled = no in-flight requests AND no timer due within 500 ms AND the microtask queue drained — hard-capped at 5 s total.** Reaching the cap is recorded as a distinct outcome, not silently treated as success.

### Minimal computed style

Readability's `isProbablyVisible` consults `display`, `visibility`, the `hidden` attribute, and `aria-hidden`; page scripts call `getComputedStyle`. With no style resolution at all, a hidden nav menu is indistinguishable from article text, and extraction quality degrades in a way that looks like a Readability bug but isn't.

Scope is deliberately tiny: resolve **`display`, `visibility`, and `hidden`/`aria-hidden` only**. This is under Phase 0's floor, not Phase 1 work — extraction can't be judged without it.

One caveat to settle early: resolving those from *author stylesheets* needs specificity and `@media` handling, and Lexbor's CSS module is a parser and selector engine whose cascade support may be thinner than this assumes. **Spike it in Phase 0** before it becomes load-bearing. The fallback if the cascade isn't there: inline `style=` plus `hidden`/`aria-hidden` plus a small class-name heuristic catches most of the value, with full cascade as a refinement.

## Pipeline

Every page takes the same steps, regardless of what kind of site it came from.

```
1.  Fetch      libcurl retrieves the page and its sub-resources
       ↓
2.  Parse      A real DOM tree is built
       ↓
3.  Execute    QuickJS runs the page's own scripts against it,
               until the settle policy says the page is done
       ↓
4a. Classify   Score the post-JS DOM for page shape
               (article / docs / table / search results / form / gallery)
       ↓
4b. Dispatch   Run the matching extractor against that same DOM
       ↓
4c. Resolve    Rewrite relative URLs; sanitize before templating
       ↓
5.  Template   Content is placed into our reader HTML/CSS
       ↓
6.  Paint      litehtml lays out our template/theme, the paint backend draws it
       ↓
    (a form submit or link click starts the loop over at step 1,
     against a new URL/request — see Interaction, below)
```

The split at step 4 matters and isn't cosmetic. **Readability is one extractor, not the extraction stage** — and it *mutates and prunes* the tree, stripping `<form>` elements and discarding tables it scores as layout. So every extractor runs against the same post-JS DOM, in parallel, and none of them runs downstream of Readability. Anything that needs form or table markup must read the tree before Readability touches it.

**Step 4c is small, unglamorous, and breaks everything if forgotten:**
- **Base-URL resolution.** Extracted `href`/`src`/`action` values are usually relative and must be resolved against the *final* URL after redirects, honouring `<base href>`. Otherwise every link, image, and form action is broken.
- **Sanitization.** Step 5 injects attacker-controlled HTML into our own template. Our reader page runs no JS, so the exposure is narrower than a browser's — but an allowlist pass is still needed, and two cases are genuinely dangerous: `javascript:` URLs surviving into rendered links, and **any URI reaching the OS handler**. Validate the scheme against an allowlist (`http`, `https`, `mailto`, `tel`) *before* anything is passed to `xdg-open`/`xdg-email`; that hand-off is the sharpest edge in the whole pipeline.

**One design constraint on step 3, for an option we're not exercising yet:** don't build the pipeline so that the QuickJS realm *must* be destroyed at step 4. See [the deferred fork](#deferred-fork-live-realm-re-dispatch) under Interaction.

## Interaction

Reading is the common case, but a page isn't done being useful once it's rendered — a search box needs a query submitted, a forum thread needs a reply posted, a `mailto:` link needs to reach a mail client. Two native constructs cover this, extracted the same way an article body is:

- **Forms.** A `<form>`'s fields, labels, method, action, enctype, and hidden inputs are extracted as data (step 4) and re-rendered as functional controls in our own styling (step 5) — hidden fields and existing values pass through untouched, since some carry CSRF tokens or session state the server checks. If the page's own script intercepts the submit event instead of a plain HTML submit, that's already covered — QuickJS + the `fetch`/XHR bridge run it like any other page script.
  - **Desktop uses native GTK widgets**, composited over the painted template rather than drawn into it. litehtml has no widget model — it lays out and paints boxes — so drawing our own text input means building caret, selection, clipboard, key repeat and IME handling from scratch. That's a mini-project with no payoff on a target that already ships GTK. The cost of this choice is honest: the desktop paint path is template-plus-overlay rather than one clean surface, and Phases 5 and 6 will each need their own answer for text entry.
  - **v1 round-trips GET search and simple `x-www-form-urlencoded` POST only**, constructed via libcurl and fed back into step 1. Multi-step flows, per-step CSRF, `multipart/form-data`, and redirect chains are an unbounded tail — they move to Phase 4, driven by evidence rather than anticipated in full.
- **Links.** A same-origin or `http(s)` link re-enters the pipeline at step 1, becoming a new history entry. A `mailto:` or `tel:` link isn't a page to render at all — it hands off to the OS's registered handler, after scheme validation (step 4c), the same mechanism as the video-player hand-off.

This is why interactivity doesn't reopen scope cut #1: it only requires recognizing native form/link markup, never a source site's own interactive widgets.

### Deferred fork: live-realm re-dispatch

The model above treats every interaction as a **full reload** through step 1. For a document that's correct and cheap. For a single-page app it's close to pathological — a click re-runs the entire boot sequence and discards the state the page just built.

The alternative: keep the QuickJS realm and DOM **alive** after extraction, dispatch a real `click` into the page's own handlers, let the settle policy run again, and re-extract. Tabs, accordions, "load more", JS-submitted forms, and in-app navigation all come along for free, faster than a reload.

This is **not being built now** — it complicates realm lifetime, memory (the RSS budget assumes a realm that dies), and teardown policy, and it bends the clean six-step pipeline. But it is plausibly worth more than Phase 3's form round-tripping, so it's recorded as a real fork rather than defaulted away. The only cost of keeping the door open is the one design constraint noted in the pipeline: **don't require the realm to die at step 4.**

## Typography & themes

The reader template (steps 5–6) is where the project stops being a pipe and becomes a page worth reading — text quality there isn't a nice-to-have, it's the deliverable.

- **Themes, not the source's CSS.** A small, fixed set of themes we design and ship — typeface pairing, color palette, line measure, spacing — is the only thing a user picks from; nothing derived from the page being read, per scope cut #1. Light/dark is one axis of a theme, not a separate toggle bolted on top of it.
- **We ship the fonts.** With fontconfig gone, the framebuffer target needs actual font files anyway — so bundle three or four SIL-OFL faces (a serif, a sans, a mono, and one with broad script coverage) and have the themes name them. That costs a couple of megabytes of data and buys something better than it costs: typography identical on every target and every machine, rather than inherited from whatever the host distro happens to have installed. That is closer to "on our own terms" than fontconfig ever was.

### The text stack

litehtml does box layout; everything about *glyphs* is ours. This is deliberate and it is contained, because **litehtml already abstracts the font engine**: `document_container` hands back an opaque font handle from `create_font` and asks only for `text_width` and `draw_text`. Pango was an implementation detail of the container litehtml ships, never a requirement of litehtml. Plug our own engine in behind the same handle and nothing above the container changes.

| Concern | Component |
|---|---|
| Shaping — kerning, ligatures, complex scripts | **HarfBuzz**, direct |
| Glyph raster and metrics | **FreeType**, direct |
| Break opportunities (UAX #14) | **libunibreak** + our own greedy breaker |
| Font selection & fallback | our own font list + small matcher over FreeType name tables |
| Bidi | **SheenBidi** — *deferred, see scope* |
| Hyphenation | **libhyphen** + pattern files — *deferred to Phase 2, only if wanted* |

**Scope the shaper the way we scoped the CSS.** The full article — itemization, bidi ordering, per-character fallback, correct interaction between breaking and reordering — is a real 1–2k-line subsystem, and building it before Phase 2 would put typography on the critical path beside the DOM↔JS layer, which is exactly the mistake litehtml exists to avoid.

So v1 is small because its scope is small: **Latin, Cyrillic and Greek; one face per run; greedy line breaking over libunibreak opportunities; no bidi; no per-character fallback.** A few hundred lines. We render *our* template in *our* bundled fonts, so this covers the corpus.

Then grow it on evidence, like everything else here: add SheenBidi when a corpus page needs RTL; add `.notdef`-driven run splitting and re-shaping when a page needs a script the primary face lacks. The telemetry that tracks unimplemented DOM calls tracks unshaped scripts the same way.

The breaker earns its place twice: the TUI wraps in **cells**, not pixels, so a breaker with a swappable measurement callback is reused directly in Phase 6. A Pango-shaped pipeline would have been rewritten there.

### Paint backend

Paint sits behind one interface, and the implementations behind it are decided now rather than in Phase 5:

- **Desktop:** Cairo, already resident under GTK.
- **Framebuffer:** our own ARGB32 blitter straight to the mmap'd surface — roughly 300 lines, and it keeps Cairo and pixman out of the minimal build entirely.
- **TUI:** no raster path at all for the common case — layout only, emitted as escapes.

Note what this doesn't solve: **libstdc++ stays as long as litehtml does.** If some future target can't take C++, litehtml is the blocker, not the paint layer. That dependency is isolated behind the C ABI adapter; the rest of the application remains C23.

## Media presentation

Images and video embeds are both **media references**: an inline placeholder (icon + description/alt text) in the reading flow, not automatic rendering — text is king, and showing a media reference is always a deliberate action, never a side effect of scrolling past it.

Classification happens during Extract (step 4b), reusing Readability's own image scoring rather than new heuristics:
- **Decorative** (empty `alt=""`, icon/spacer-sized, CSS `background-image`) — dropped entirely, never a placeholder.
- **In-flow content images** — kept inline at their original position, as a placeholder.
- **No clear anchor** (gallery/carousel markup, sidebar images Readability judged content-relevant but couldn't place in the paragraph flow) — collected into an end-of-content list instead of guessing where to attach them.

Nothing is fetched until a placeholder is activated — no bandwidth or decode cost for content the user never looks at. Activating an image opens **viewer mode**; activating a video reference hands off externally, after scheme validation.

**Viewer mode** is a mode, not a separate window — the same screen/surface swaps to a full-view state with its own input handling, which is what makes it work on framebuffer (no window manager to open a second window in) and TUI (one terminal canvas), not just desktop.

**v1 is deliberately minimal:** fit-to-width display, next/previous across the ordered media-reference list that backs the end-of-content gallery, **save** (writes the original fetched bytes to disk, unmodified — a copy of the source file, not our rendering of it), "open externally", and **exit** (Esc/q) back to the reading view at the same scroll position.

Per-target zoom semantics arrive with each target, not now. Decoding uses libpng/libjpeg/libwebp directly.

## Render targets

Steps 1–5 are the same regardless of output; only step 6 is target-specific, behind the paint interface above.

| Target | Mechanism | Phase |
|---|---|---|
| **Desktop** (Cinnamon/X11) | Cairo into a GTK window; native GTK widgets composite over it for form entry | 3 |
| **Framebuffer** | Own ARGB32 blitter to the mmap'd surface | 5 |
| **TUI** | Styled terminal text; no raster path for the common case | 6 |

**Input** follows the same split, and every target must work by keyboard alone or by mouse alone — nothing assumes both are present. Desktop gets both from GTK. The other two are specified in their own phases.

## Build phases

Each phase produces something checkable before the next begins. No visual work happens before Phase 2 — everything before that is proven from the terminal. **Desktop (Phases 0–4) is the near-term focus.** Framebuffer and TUI are kept in mind but deliberately deferred, framebuffer before TUI: a modern terminal is the last visualization target, not the next one.

Each phase below carries its work **in order**, and in most cases the order is just convenience. Three places it isn't, because getting them late means redoing work rather than merely delaying it:

- **Phase 0's arm decision gates everything.** Phases 1–6 all assume a DOM exists; which one it is changes what they are built on.
- **Phase 2 opens with a spike**, not with building. The entire Pango rejection rests on litehtml being genuinely font-engine agnostic — an assumption worth a day to confirm and a month to discover.
- **Phase 2's measurement callback is what makes Phase 6 cheap.** Break against a callback rather than against pixels and the TUI reuses the breaker; hard-code pixels and Phase 6 rewrites it.

Two threads run through every phase rather than living in one: the **corpus** (`corpus/check.py` green before a phase is called done) and the **telemetry log**, which accumulates from Phase 0 and is what Phase 4 spends.

### Phase 0 — Headless proof of concept, run as an A/B
*No UI · no rendering*

**Goal:** Size the DOM↔JS problem — the largest unscoped item in the plan — with evidence rather than estimate, and answer [the structural question](#the-structural-question-does-this-stay-a-c-project).

**Work, in order.** Two orderings matter and the rest is taste: *resource limits and telemetry come before any real page JS runs*, or the first week is spent debugging hangs and guessing at gaps; and *the arms come last*, after everything they share is stable, or the A/B measures the harness instead of the arms.

*0.a — Harness.* Build system; libcurl fetch into a buffer; a CLI taking a URL or local path and printing the parsed DOM, no JS involved (rung `00-static` passes here with no engine at all). Embed QuickJS and run an inline script with `console`. Add the scheduler: `poll()` + `curl_multi` + microtask drain + timer queue. Then the resource limits — interrupt handler, memory cap, script count. Then telemetry, wired as the **default behaviour of every unimplemented member**: log and return a benign value rather than throw, so the binding surface is *discovered* rather than designed.

*0.b — Page environment* (shared by every arm; the list below is the specification). Script loading and lifecycle first, since nothing else is observable without them; then the settle policy; then the browser globals; then the geometry fiction; then the minimal style resolver and its Lexbor-cascade spike. Run `corpus/check.py` continuously from here on — rungs `01` through `08b` come green roughly in their numbered order, which is what they were numbered for.

*0.c — The arms,* built against that frozen harness: Arm A, then Arm B, then Arm B′ (B′ is a build-configuration change to B, not a third implementation).

*0.d — Decide.* Collect the five comparison criteria, pick an arm, and write the decision and its reasoning into [Considered and rejected](#considered-and-rejected) — including the structural question, answered out loud.

**Built once, shared by every arm:**
- libcurl fetch, and the scheduler (`poll()` + `curl_multi` + microtask drain + timer queue).
- **The browser-globals and lifecycle surface** enumerated above — storage stubs, `navigator`/UA, `location`/`history`, and especially `DOMContentLoaded`/`load`/`readystatechange` plus in-order script loading with `async`/`defer`. This is shared work precisely because getting it wrong is indistinguishable from a binding bug.
- **The settle policy**, implemented and enforced, with cap-reached recorded as its own outcome.
- **The corpus** — built, in [`corpus/`](corpus/), with [`corpus/manifest.json`](corpus/manifest.json) holding the assertion contract and `corpus/check.py` exiting non-zero when a gate fails. Two kinds of local fixture, and only one of them gates Phase 0: **isolating fixtures**, hand-written, one capability each, so a failure names its own cause — and **realistic bundles**, whole saved pages that test breadth but attribute nothing. Eleven isolating fixtures exist and are validated against a reference browser; the bundles slot is empty until Phase 1. Alongside them, five live rungs in ascending difficulty as a reality check — **recorded, never gates**, because curl's TLS/HTTP2 fingerprint plus a missing browser API surface gets flagged on exactly the JS-heavy sites we're targeting, *without any JS challenge ever running*. The top live rung (`www.dn.se`) is explicitly a **diagnostic**: its article body sits behind both a consent wall and a paywall, so being able to say *which* of three causes fired is the result. Local fixtures are also the only route to golden-output regression testing, since live sites redesign under you and a regression becomes indistinguishable from a redesign.
- **Where we must beat the browser.** Three fixtures require behaviour a conformant browser does not have, each restating a design choice above as a testable fact: the settle cap (a browser waits forever on a hung request), the layout fiction (a browser leaves below-fold lazy content unloaded; we load it), and encoding detection (a browser falls back to windows-1252 on an undeclared UTF-8 page and produces mojibake). Matching the browser is not the goal — `corpus/README.md` records where and why.
- **Call telemetry, from day one.** Log every unimplemented DOM/JS call real pages hit, with geometry-API calls flagged separately. This is Phase 4's mechanism pulled forward, and it's the only thing that keeps the binding surface evidence-driven instead of open-ended. It's also the primary A/B measurement.
- **Resource limits, from day one.** A QuickJS interrupt handler enforcing the script ceiling, the memory cap, and a maximum script count. We execute untrusted JS in-process; without these, one page hangs the whole app and Phase 0 gets spent debugging hangs that aren't binding bugs.
- The minimal style resolver, plus the Lexbor-cascade spike noted above.

**Acceptance asserts on the DOM, not on Readability.** This is the criterion the first draft got wrong: with Readability as the bar, a docs page failing tells you nothing about DOM fidelity, because "the page's JS didn't run" and "Readability doesn't like this page's shape" produce the same red. The second of those is Phase 1's problem, not Phase 0's.

So: **every corpus page carries a CSS selector plus an expected substring that exists only after the page's own script has run.** Pass = that assertion holds at settle time. Readability output is a secondary signal, recorded but not the bar. Assertions are tagged `dom` (Phase 0) or `extract` (Phase 1) in the manifest, so the two stages can never quietly grade each other.

**Arm A — bespoke C bindings over Lexbor.** Best fidelity and memory; also the jsdom problem, which is person-years of work if approached openly. So the binding checklist is derived *mechanically* — from Readability.js's own source, whose DOM surface is finite and enumerable, plus one framework runtime. "Wide enough for a real SPA" is not a specification and won't be used as one.

Three things to decide **before** Arm A is written, because they are the hardest part of any jsdom-style binding and retrofitting them means rewriting it:
- **Node lifetime.** Lexbor nodes are arena-allocated and page scripts mutate the tree constantly, so a naive "one JS wrapper holding a raw `lexbor_dom_node_t *`" binding dangles. The symptom is a crash that only appears under heavy DOM churn — i.e. on exactly the SPAs in the corpus. Use opaque integer handles into a side table with tombstoning on removal, not raw pointers.
- **Node identity.** The same C node must yield the same JS object across calls, or `===` comparisons in page scripts silently break.
- **Wrapper cache strength.** Strong or weak, decided deliberately — it's the difference between leaking every node a page ever touched and collecting one a script still holds.

NetSurf's libdom and `dukky` (both MIT) are prior art for exactly these questions and worth reading first.

**Arm B — a JS-side DOM inside QuickJS** (linkedom or domino), with only `fetch`, timers, `console` and `URL` bridged from C. Page scripts and Readability both run against the JS DOM; the result serializes to HTML. Costs speed and memory; may delete the binding layer outright.

**Arm B′ — the no-Lexbor baseline.** Arm B with litehtml parsing the serialized output directly and Lexbor absent from the build entirely. This is the arm that actually answers the structural question; without it, Lexbor's place is assumed rather than earned. Encoding detection is the known gap to account for here.

**Comparison criteria**, so the decision is mechanical rather than argued: DOM-assertion pass rate across the corpus; unimplemented-call volume from telemetry; peak RSS and fetch→extract wall-clock against the budget; total shared-library footprint against the size budget; and the rough size of the remaining bespoke surface.

**Done when:** one arm passes the corpus assertions, meets both budgets, and has been chosen on recorded evidence — with the structural question answered explicitly rather than implied. Keep the losing arms' notes.

**Phase 0 result (2026-09-14):** Arm A and the JS-side Arm B both pass all local `dom` gates, including the deliberate settle, geometry, and encoding divergences. Arm B′ is built as the no-Lexbor configuration and parses Arm B's serialized output through litehtml; it also passes the corpus. Arm A is selected for the next phase because it is the lower-RSS implementation (14.4 MiB sampled versus 15.0 MiB for Arm B), keeps the binding boundary in the planned C23-rooted stack, and exposes geometry calls in the default telemetry stream. Both stay below the 150 MiB RSS, 3 s typical-page, 5 s cap, and 8 MiB shared-library budgets; the measured Arm A shared set is 5.18 MiB (QuickJS-ng + Lexbor). Arm B and Arm B′ remain in-tree as comparison targets (`distill-arm-b` and `distill-arm-b-pruned`). The language decision is C23 for the production core, with litehtml's C++ adapter as the deliberate exception. Lexbor earns its place for direct DOM ownership, encoding detection, and the lower-memory native path. Detailed evidence is in [`PHASE0.md`](PHASE0.md).

**C23 port result (2026-09-15):** The application and extraction pipeline now build as C23. The headless runtime uses the linkedom-compatible DOM surface, while Lexbor remains the native parser used by extraction; this preserves the settled DOM and extraction contract without leaving C++ in the application. litehtml remains isolated behind the C ABI adapter in the no-Lexbor comparison target. The historical Phase 0 measurements above are retained as recorded evidence and are not claimed as measurements of the new C23 build.

### Phase 1 — Extraction breadth
*Still headless*

**Goal:** Stop assuming every page is an article.

**Work, in order:**
1. **Step 4c first** — base-URL resolution after redirects honouring `<base href>`, sanitization, and the URI scheme allowlist. Everything downstream renders URLs, so a wrong one here is a bug in every later phase. Rung `08-messy` already asserts the resolution half.
2. **The dispatch skeleton** — page-shape classifier and confidence scoring, with Readability as the *only* registered extractor. Prove selection works while there is only one thing to select.
3. **The extractors, one at a time**, each landing with its own corpus page: structured text dump (the fallback that must never fail), table, search-results, form, video metadata, media-reference classification.
4. **Turn on the `extract`-stage assertions** in `corpus/manifest.json` — they exist and are reported as pending today. This is the phase that earns them.
5. **Golden outputs** per extractor, so extraction quality regressions are visible rather than argued about.
6. **Fill `corpus/local/bundles/`** with saved real pages — breadth, recorded and never gated, per the two-kinds-of-fixture split.

**Done when:** reasonable output exists for at least one article, one docs page, one table-heavy page, one search-results page, one video-embedding page, one form, and one image-heavy page with a plausible decorative/content split — all from the corpus, so the results are re-runnable.

**Phase 1 result (2026-09-15):** The selected Arm A now exposes a post-settle
extraction command (`--extract`) with a dispatch skeleton, confidence-scored page
shape classification, and stable JSON output. Step 4c is implemented first:
libcurl's effective URL is retained across redirects, `<base href>` is honored,
relative references are resolved against that final base, and extracted URIs are
sanitized to `http`, `https`, `mailto`, and `tel` before they enter reader data.
Readability 0.6.0 runs unmodified against a normalized snapshot of the settled
Lexbor DOM; the native tree is also walked for the structured-text fallback,
tables, search results, forms (including hidden fields), video metadata, and
decorative/content media references. The new fixtures `09` through `16` pass
all extract-stage assertions, golden contracts, and the redirect check. The
original Phase 0 DOM gates remain independently green. Phase 2 can now consume
the JSON extraction contract; realistic saved-page bundles remain recorded
fixtures and never become gates.

### Phase 2 — Reader rendering
*First visual output*

**Goal:** Turn extracted content into the actual "beautiful, clear" output — this is where the project stops being a pipe and starts being a page.

**Work, in order:**
1. **The spike, before anything else.** Implement `create_font`/`text_width`/`draw_text` over FreeType in a throwaway litehtml container and confirm litehtml is genuinely font-engine agnostic. The whole Pango rejection rests on this being true; find out in a day rather than a month.
2. **Bundle the faces** and write the font matcher — walk a configured directory, read family and style from FreeType name tables, match on request. Unglamorous, and it is the piece fontconfig was doing.
3. **Run shaper v1** — itemize into runs of uniform style, shape each with HarfBuzz, compute break opportunities with libunibreak, greedy-break against a **measurement callback**. The callback is not over-engineering: it is what Phase 6 reuses to break in cells instead of pixels.
4. **Cairo paint backend** behind the paint interface. One backend now, but the interface exists from the start so Phase 5 adds rather than refactors.
5. **The reader template and stylesheet, with exactly one theme.** Get one genuinely pleasant to read before there is a set to be inconsistent across.
6. **Then breadth:** the remaining themes, media-reference placeholders, video cards, form-control styling.
7. **Viewer mode's paint side** — fit-to-width, next/previous, save.

**Done when:** output from Phase 1's test pages renders consistently and is genuinely pleasant to read.

**Phase 2 result (2026-09-15):** The font-engine spike is complete. The
litehtml adapter now uses a C23 FreeType/HarfBuzz backend for font creation,
metrics, shaping, measurement, glyph painting, decorations, clipping, fills,
borders, and list markers; it does not depend on Pango. The matcher scans a
configured font directory and reads family/style metadata from FreeType, with
standard system-font directories as the development fallback. A reader template
and first theme are wired to Phase 1's Readability output, and `--render` emits
Cairo PNGs. The bundled development faces live under `external/fonts`, while
`--font-dir` and `DISTILL_FONT_DIR` support another configured collection.
The libunibreak-backed greedy UTF-8 breaker is exposed through the C23 render
API and has a CTest contract; litehtml uses the same shaped measurement callback
for its layout. Structured table, search, form, video, and media-reference
cards are rendered from the Phase 1 JSON contract. The CLI media viewer provides
fit-to-width staging, previous/next state, external hand-off links, and original
byte saving. `corpus/check.py --render DIR` and `--viewer DIR` cover the PNG
paths, and the Phase 1 pages have been visually reviewed. Phase 3 can now add a
window around this completed paint path.

### Phase 3 — App shell
*Daily-usable*

**Goal:** Make it usable without a terminal.

**Work, in order:**
1. **A GTK window hosting the Cairo surface**, rendering one fixed page. Proves the shell before it has anything to drive.
2. **Address bar** wired to the pipeline — the first end-to-end URL-to-page path.
3. **History** over extraction results, back and forward. Note the [realm-lifetime constraint](#deferred-fork-live-realm-re-dispatch) here: history entries must not assume the realm died.
4. **Native GTK form widgets** composited over the painted template, placed from litehtml's box geometry. This is the fiddly one — hit-testing and placement, not the widgets themselves.
5. **Form submission** — GET search and simple POST, the response re-entering the pipeline as a new history entry.
6. **External hand-off**, scheme-validated, via `xdg-open`/`xdg-email` or a configured command.
7. **Reading list and theme picker** — the last things, because they are the easiest to get right and the easiest to defer.

**Done when:** a URL can be typed in and read, start to finish, with no terminal involved.

**Phase 3 result (2026-09-15):** `distill-gtk` is a C23 GTK3 desktop shell
around the existing pipeline. It owns a Cairo-backed reader surface inside a
scrollable window, an address bar and asynchronous URL load path, cached
back/forward history, and a URI-validated `xdg-open` hand-off. The shell asks
the litehtml C++ boundary for form and link box geometry, overlays native GTK
entries/buttons on the rendered reader, and dispatches simple GET forms and
URL-encoded POST forms back through the engine. Warm, night, and paper themes
are selectable through the reader template, as are eight zoom levels from 75%
to 200% (`--zoom`, Ctrl+plus/minus/0), laid out by the engine rather than
scaled after the fact so the type stays sharp and the native controls keep
their geometry. The page follows the window between a floor and the width the
reader was drawn at (480 to 760 design pixels, both scaled by zoom), centred
on its desk: past the top of that span a wider window buys margin and nothing
else, because the measure is capped for readability, and past the bottom the
window scrolls rather than squeezing the page further. A width change is a
re-layout of the template already on disk, not a navigation. While a page
loads, a spinner and a percentage report what the engine is doing: the engine
writes one `progress PERCENT PHASE` record per step on stdout (`--progress`)
and the shell reads the pipe as the run happens rather than waiting for it to
end. The phases -- fetch, scripts, extract, render -- are what is measured;
the percentage is their weighting, with the fetch band following the bytes
when the server declares a length and the script band following the settle
budget it cannot exceed. Text can be selected and copied: the rendering
boundary hands the shell the runs litehtml drew, with the box each filled and
the block it came from, and the shaper answers where a character sits inside a
run in cluster steps, so a selection never lands inside a letter. Selection is
logical-order by design -- visual-order highlighting for bidirectional text is
out of scope, and RTL pages still copy correct text. A reading-list toggle
keeps URLs for the session. `distill-app-state` and `distill-litehtml-geometry` CTest
contracts cover history truncation/replacement and native-control geometry,
while `corpus/check.py --app` covers the display-free shell model. Phase 4 can
now widen the interaction and binding surface from evidence.

### Phase 4 — Hardening
*Open-ended, later*

**Goal:** Widen coverage based on evidence, not guesswork. This phase has no order — it is a set of standing queues, worked by whatever the logs rank highest.

**The queues:**
- **Binding surface** — triage the telemetry log accumulated since Phase 0 and extend against its top hits, never against the spec.
- **Form round-tripping** — multi-step flows, per-step CSRF, `multipart/form-data`, redirect chains, as and when the log demands them.
- **The shaper** — bidi (SheenBidi), per-character fallback via `.notdef` run splitting, hyphenation. Same rule: a corpus page has to ask first.
- **HTTP caching and conditional requests** — nothing before this point has any, and every link click pays full price.
- **[Live-realm re-dispatch](#deferred-fork-live-realm-re-dispatch)** — revisit with real usage data. It is plausibly worth more than Phase 3's form work.
- **Sandboxing** — decide seccomp or a separate process for the JS realm, rather than leaving it recorded-and-deferred forever.
- **Budgets and packaging** — tune against both, then package for distribution.

### Phase 5 — Framebuffer
*Deferred until desktop is finished*

**Goal:** Same core, a different paint backend — no new pipeline, extraction, interaction, or theme work.

**Work, in order:**
1. **The second paint backend** — an ARGB32 blitter to the mmap'd `/dev/fb0`. The surface is 32-bit host-order XRGB8888, the same format we composite in, so no conversion — but the surface stride and the fb's `line_length` can differ, so blit **row by row**, never one flat `memcpy`.
2. **Surface lifecycle** — fb probing, mode information, teardown on exit and on VT switch.
3. **`libinput`** for keyboard and pointer both, reading `/dev/input/event*`, since no compositor is handing out events. Raw `evdev` is the fallback if `libinput` is too heavy for a minimal build.
4. **Decide the permission story** — `input` group, a udev rule, or root. It has been an open question since the first draft; this is where it closes.
5. **Text entry without GTK.** The genuinely new work of this phase, and the bill for the Phase 3 decision to composite native widgets.
6. **Viewer mode** as a full-screen state swap, with zoom as a scale transform.
7. **Measure the size budget for real** and strip whatever quietly pulled GLib back in. This phase is where "small and portable" stops being an assertion.

NetSurf's fb frontend is **read-only reference** (GPL-2.0) throughout, never lifted code.

**Done when:** the same URL, read and interacted with, is navigable by keyboard alone and by mouse alone on the framebuffer, matching the desktop experience — within budget.

### Phase 6 — TUI
*The last visualization target*

**Goal:** Same core again, text-first — most of the reading experience needs no raster path at all.

**Work, in order:**
1. **Terminal text renderer** — reuse the Phase 2 line breaker with a cell-measuring callback instead of a pixel one. If step 3 of Phase 2 was done properly, this is small.
2. **Input** — raw-mode stdin for keyboard, SGR mouse reporting for pointer.
3. **The reading view**: articles, tables, and forms as styled terminal text, including in-terminal text entry.
4. **Capability probe** — `$TERM` plus a terminal query, deciding what the image path can be.
5. **Images**: our own kitty/sixel encoder where the protocol exists, `[image: alt text]` everywhere else. Unicode block-art is not planned — it would not meet the Phase 2 bar.
6. **Viewer mode** by re-encoding at a different crop and cell size.

**Done when:** the same URL is navigable the same way as Phases 3 and 5, in a terminal.

## Licensing position

Everything linked is permissive and compatible: Lexbor and Readability.js (Apache-2.0), QuickJS (MIT), litehtml (BSD-3-Clause), libcurl (curl license), HarfBuzz (MIT), FreeType (FTL or GPLv2 — **take FTL**), libunibreak (zlib), SheenBidi (MIT), libpng/libjpeg/libwebp (each permissive), bundled fonts (SIL OFL).

**The GPL-2.0 line is NetSurf's browser, not NetSurf's libraries.** The framebuffer frontend is part of the browser and stays out — read it, don't lift it. But libdom and libcss are MIT and separate, and `dukky` (NetSurf's Duktape↔DOM glue) is directly relevant prior art for Arm A; reading them carries no licensing consequence at all. NetSurf remains the project's origin and a design reference.

FreeType's dual license needs the FTL election recorded explicitly, since the alternative is GPLv2 and would contaminate everything else here.

## Open risks

- **[unknown effort]** The DOM↔JS layer is still the largest item. It is no longer unmeasured: Phase 0's A/B, DOM-level assertions, and telemetry size it before Phase 1 or 2 is committed to.
- **[structural]** If Arm B or B′ wins, this stops being a C23-rooted stack in any meaningful sense. That's a legitimate outcome, but it changes what the project *is* — it needs to be chosen out loud, not absorbed silently.
- **[unproven]** The layout fiction is a working hypothesis. Pages that branch on measured geometry will misbehave in ways that look like content bugs; telemetry flags geometry calls so the failure mode stays visible, but we don't know how common it is.
- **[unproven]** Lexbor's CSS cascade may not be deep enough for the `display`/`visibility` resolver. Spiked in Phase 0; the inline-style fallback is the contingency, and the failure mode — nav and footers leaking into output — is already known.
- **[unproven]** Readability.js is tuned for article-shaped content; its behavior on docs, tables, and search results is a genuine open question — Phase 1 tests it directly.
- **[scope creep risk]** The run shaper is scoped small on the bet that our own template in our own fonts needs no bidi and no per-character fallback. If the corpus disagrees early, that's a real Phase 2 overrun, and the honest fallback is to narrow the theme's script coverage rather than grow the shaper mid-phase.
- **[unproven]** Form round-tripping assumes hidden fields pass through untouched and libcurl's cookie jar carries session state across submit → refetch. v1's GET/simple-POST scope contains this; it doesn't answer it.
- **[security, deferred]** We execute untrusted site JS in-process. The interrupt handler and memory cap bound runaway scripts, not exploitation — QuickJS has no JIT and is memory-safe-ish, but it has had CVEs. seccomp, or a separate process for the JS realm, is the answer if this ever faces hostile input deliberately. Recorded now so "later" is a decision rather than an oversight.
- **[accepted cost]** Native GTK widgets composited over the painted template means the desktop paint path isn't one clean surface, and text entry is unsolved for Phases 5 and 6. Taken knowingly over building a text input from scratch.
- **[deferred]** Bot-challenge and login-gated pages have no plan and aren't meant to.
- **[unset]** Lazy image fetch means the first activation of a placeholder has real network latency before viewer mode shows anything — needs a loading state, not assumed to be instant.

## Reuse ledger

What this project is actually building, once the borrowed parts are set aside.

**Taken as-is**
- libcurl — networking & TLS
- Lexbor — HTML parsing, DOM, selectors, CSS, encoding detection *(role pending the Phase 0 outcome)*
- QuickJS (quickjs-ng) — the JS engine itself
- Readability.js — runs unmodified, as one extractor
- litehtml — box layout for our own template
- HarfBuzz, FreeType, libunibreak — shaping, rasterization, break opportunities
- SheenBidi, libhyphen — deferred until the corpus demands them
- Cairo — desktop paint backend only
- GTK3 — desktop window shell and native form widgets, desktop target only
- libpng / libjpeg / libwebp — image decoding, linked directly
- SIL OFL font faces — bundled, three or four
- linkedom or domino — only if Phase 0 Arm B/B′ wins

**Built new**
- The DOM ↔ JS layer (shape decided by the Phase 0 A/B)
- `fetch`/XHR ↔ libcurl bridge
- The scheduler: microtask drain, timer queue, `curl_multi` polling
- Browser globals, document lifecycle, and script loading semantics
- The settle policy
- The layout fiction (geometry-API surface) and the minimal style resolver
- Page-shape classification, extractor dispatch, and confidence scoring, plus the extractors behind it
- Base-URL resolution, sanitization, and the URI scheme allowlist
- Video metadata extraction + external-player hand-off (no playback)
- Form extraction, native re-render, and the submit → refetch → re-pipeline loop
- Media-reference classification and lazy fetch on activation
- The run shaper (scoped small) and the font matcher
- The paint backend interface, and the framebuffer ARGB32 blitter
- Viewer mode across all three render targets
- The test corpus, DOM-assertion harness, golden outputs, and call telemetry
- The reader stylesheet & template
- The desktop app shell (address bar, history, reading list)
- The framebuffer shell (Phase 5), and the TUI shell plus kitty/sixel encoder (Phase 6)

---

*Working plan. The component choices still open — Lexbor's role above all — are resolved by the Phase 0 A/B rather than argued in advance. The choices in [Considered and rejected](#considered-and-rejected) are not open; where one was made for a wrong reason, the corrected reason is recorded beside it.*
