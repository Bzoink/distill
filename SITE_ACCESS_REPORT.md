# Live-site access and compatibility report

Date: 2026-09-15

This report records what happened when Distill was compared with a local
Chromium reference browser. It focuses on pages that were blocked, incomplete,
or materially different. It is a diagnostic report, not a claim that Distill
should reproduce every browser feature.

The comparison suite uses the same requested URL for Chromium and Distill,
captures each post-JavaScript DOM, and compares visible vocabulary, headings,
links, images, forms, and Distill's structured extraction result. The live
reports are stored under `reports/site-comparison/`; that directory is ignored
because live pages change and are not suitable CI fixtures.

## Executive summary

The 34 entries in the current comparison ladder ended with:

| Result | Count | Meaning |
| --- | ---: | --- |
| Checked and working | 18 | Distill preserved the useful information; source-site decoration or pixels may differ |
| `antibot` | 11 | A protection, denial, CAPTCHA, or equivalent page prevented a meaningful content comparison |
| `out of scope — deferred` | 5 | The page was reachable, but matching it needs browser/runtime capabilities deliberately outside the current design |

The important conclusion is that the unsuccessful cases do not have one
cause. Eleven are access-control results. Five are capability or target-scope
results. Treating both as ordinary extraction bugs would lead to the wrong
engineering work.

## Current state after network remediation

The network-layer remedies proposed after the first comparison have now been
implemented in commit `c25b538` (`Improve network compatibility and cookie
handling`). They improve protocol compatibility and session continuity, but
they do not attempt to defeat anti-bot challenges, CAPTCHAs, paywalls, or
headless-browser detection.

| Remedy | Current implementation | Result |
| --- | --- | --- |
| Per-run cookies | libcurl cookie state is shared between navigation, `fetch`, XHR, and asynchronously loaded scripts; `document.cookie` reads and writes the same state | Cookies now round-trip between JavaScript and network requests during one run |
| Request defaults | Navigation requests send an HTML `Accept`; fetch/XHR subrequests send `Accept: */*`; all requests send `Accept-Language`, negotiated `Accept-Encoding`, a desktop user agent, and a subrequest `Referer` | Requests are materially closer to ordinary browser traffic |
| Page request headers | `fetch(..., {headers})`, `Headers`, and XHR `setRequestHeader()` are forwarded to libcurl, subject to browser-style restricted-header filtering | API and form requests can supply their own content type and application headers |
| Request bodies | POST and other request bodies are passed through without forcing `Content-Type: application/json` | Form-encoded and other caller-selected body formats are preserved |
| Response metadata | Response headers are captured, exposed through `get`, `has`, `forEach`, `entries`, `keys`, and `values`, and effective redirect URLs populate `url` and `redirected` | Scripts can branch on ordinary response metadata |
| XHR response state | XHR now exposes response URL, response headers, `getResponseHeader()`, and `getAllResponseHeaders()` | The local XHR path matches the fetch path more closely |

The new local fixture `18-network-contract` exercises cookies, `Headers`,
custom request headers, POST bodies, response headers, redirects, and XHR. It
passes both Distill and the Chromium baseline. The complete local Phase 0 and
Phase 1 gates, the three CTest tests, and the rebuilt binaries also pass.

The live retest was deliberately limited to previously problematic targets:

| Target | Current result after remediation |
| --- | --- |
| Stack Overflow (`10-stack-overflow`) | Still Cloudflare's `Just a moment...` challenge; ordinary cookies and request metadata did not make content available |
| Airbnb (`13-airbnb`) | Network requests now have the required basic bridge support, but the application still does not hydrate into Chromium's content; remains out of scope |
| Zillow (`22-zillow`) | Still access-denied/verification behavior; Distill's challenge scripts consume the settle budget |
| IMDb (`23-imdb`) | Still `403 Forbidden` for both clients |

Therefore the remedy changed a genuine engine limitation and established a
tested network contract, but it did not change the anti-bot classifications or
the Airbnb scope classification. Persistent user-supplied sessions,
structured-data harvesting, robots/rate-limit handling, explicit HTTP-version
selection, and sanctioned API/RSS adapters remain future work.

## Second round: module resolution, structured data, classification

The network remediation above was scored rather than assumed, and scoring it
produced a second round of work. Two of the predictions that motivated Tier 1
were falsified, one site was unblocked that nobody had predicted, and reading
the module path exposed a defect rather than a missing capability.

### A defect, not a missing capability

`module_normalize` discarded the `base_name` argument and resolved every
module specifier against the page URL. A bundle at `/assets/app.js` importing
`./chunk.js` therefore requested `/chunk.js`, received the site's HTML
fallback, and failed to compile with `unexpected token '<'`.

That is the exact error recorded against JSFiddle. A local fixture reproduces
it verbatim on the pre-fix engine:

```
SyntaxError: unexpected token in expression: '<'
    at http://127.0.0.1:8899/chunk.js:1:1
```

The resolved path is `/chunk.js`, not `/modules/chunk.js`. This affected the
second and every subsequent level of any module graph, so it was never
specific to JSFiddle. Alongside it, `module_loader` captured the HTTP status
and never read it, which is why the failure surfaced as a syntax error instead
of a missing module. Both are fixed and gated by `19-module-graph`, which also
asserts that a page shipping a module bundle and a `nomodule` fallback runs
one of them rather than both.

### Structured data without hydration

Script discovery already located every JSON block on a page and discarded it.
JSON-LD is the page's own record of itself and is present whether or not the
application hydrates, so it is now extracted into a `structured_data` field,
with malformed blocks reported rather than dropped. This is the one measure
that addresses layout-dependent hydration without a browser-semantics project:
it does not need the application to start.

### Classifying challenges instead of extracting them empty

A challenge shell runs its own scripts and spends the entire settle budget
before the run can report that no content was ever available. Recognised
challenges are now detected — against the served HTML before scripts run, and
against the settled DOM afterwards for challenges that write their message
from script — and reported with exit status `4`.

Recognising a challenge is not solving one. No challenge is answered, no
token is computed, and every site that blocked us still blocks us. What
changes is that the result is a classification rather than an empty extraction
that looks like a parser bug.

**The first version of this detector was wrong, in an instructive way.** It
used vendor names as markers. Checked against pages that actually serve
content, `datadome` matched Airbnb and `/cdn-cgi/challenge-platform` matched
Product Hunt — both serve full pages while naming those vendors — so the
detector was skipping the scripts of working pages. The markers are now
restricted to strings unique to an interstitial, with a document-size guard as
a second line of defence, and gate `22-challenge-vendor-scripts` locks the
false-positive case in permanently.

### What the retest showed

| Target | Result |
| --- | --- |
| Zillow (`22-zillow`) | **Now returns content** — real title, 1448 characters, 271 links — where both clients previously received an access-denied page. The probable cause is protocol-version negotiation, which was still missing after Tier 1 |
| IMDb (`23-imdb`) | The flat `403` is gone; IMDb now answers `202` with an AWS WAF challenge. The request shape was a real defect; the challenge behind it is not ours |
| Airbnb (`13-airbnb`) | Headers and bodies cross the bridge and no listing request is still made. The architectural diagnosis below is confirmed on evidence rather than suspicion |
| Stack Overflow (`10-stack-overflow`) | Cookies round-trip and the Cloudflare interstitial remains. A policy result, not a defect |
| JSFiddle (`31-jsfiddle`) | The syntax error was ours. The site now serves a Cloudflare challenge, so its editor/frame requirements are untested rather than disproven |

Two conclusions worth separating. Distill genuinely improved: a protocol
inconsistency and a module-resolution bug were real defects, and fixing them
unblocked one site and corrected the recorded cause on two more. And the
anti-bot classifications largely held: cookies and headers did not open
Cloudflare, and no measure here was aimed at making them.

## Sites classified as anti-bot

These sites did not provide a usable content target to both sides of the
comparison. We did not try to bypass their protection. This is intentional:
the project plan explicitly excludes bot-challenge solving, and a regular
browser run without login or special cookies is the comparison baseline.

| Site | Observed result |
| --- | --- |
| Medium (`08-medium`) | Chromium received a hard Cloudflare block; Distill received a Cloudflare challenge. Neither reached Medium content. |
| Stack Overflow (`10-stack-overflow`) | Chromium received a site response, but Distill received Cloudflare's `Just a moment...` page, so there was no usable same-content comparison. |
| X/Twitter (`12-x`) | Chromium reported `Access to x.com was denied`; Distill reached only the unauthenticated login shell. The supplied URL also lacked a concrete public profile. |
| Reddit (`18-reddit`) | Both clients received Reddit's `Prove your humanity` challenge. |
| CodePen Trending (`19-codepen-trending`) | Both clients received CodePen/Cloudflare security verification reporting that the browser was unsupported. |
| Product Hunt (`20-product-hunt`) | Chromium was stopped at Product Hunt's Cloudflare verification page; Distill reached site content, so the reference side was blocked. |
| Zillow (`22-zillow`) | Superseded. Distill now reaches Zillow content; the access-denied page still appears on some requests and is classified as `perimeterx`. |
| IMDb (`23-imdb`) | Revised. The `403 Forbidden` was partly our own request shape; IMDb now answers `202` with an AWS WAF challenge, classified as `aws-waf`. |
| Bloomberg (`24-bloomberg`) | Both clients received Bloomberg's unusual-activity robot check. |
| Target (`25-target`) | Chromium received the homepage; Distill received Target's `Pardon the interruption` bot-mitigation page. |
| CodePen (`30-codepen-embed`) | Both clients received CodePen's Cloudflare security-verification page; Distill additionally reported that its browser was unsupported. |

These results are access failures, not evidence that the content extractor is
wrong. The useful follow-up for a real user would be authenticated or
user-supplied session state, a permitted API, or a pinned public URL—not a
challenge-specific workaround in Distill.

## Sites reachable but deferred as browser/runtime scope

### Airbnb — `13-airbnb`

Airbnb was not blocked. Chromium exposed homepage content, and Distill got a
successful response and executed scripts, but the application remained in its
placeholder state: `0 of 0 items showing`. The latest run recorded 244
reference vocabulary words and only 16 useful Distill words.

The first diagnostics exposed missing `CSS`, `AbortController`, `CustomEvent`,
`Intl`, `PerformanceObserver`, `NodeFilter`, `URL`, `visualViewport`, and
range/layout methods. We added focused compatibility shims. We also added
POST/body handling for `fetch` and `XMLHttpRequest` and a Chromium-like user
agent. Those changes removed fatal errors but did not make the React
application hydrate.

After the Tier 1 network remediation, ordinary custom headers, POST bodies,
cookies, and response headers cross the bridge, but the remaining problem is
architectural. Airbnb expects accurate layout
measurements, observer updates, complete DOM/event behavior, browser request
metadata and cookies, and browser-like asynchronous scheduling. Distill uses
lightweight layout and browser-global shims, so returning fixed geometry or
empty observer results can leave a responsive application at its loading
branch. Tracing still shows no listing/content request that populates the
page. Matching this reliably needs a much broader browser
compatibility layer or a real browser runtime.

### YouTube — `16-youtube`

The supplied URL was YouTube's home page, not a concrete watch page. Chromium
exposed consent/history UI. Distill could not complete the site's
Web-Components/shadow-DOM bootstrap. This is not a useful watch-page content
comparison until a pinned public watch URL is supplied, and the bootstrap
failure still points at browser-level runtime coverage.

### Imgur — `17-imgur`

Chromium reached Imgur's media/consent page. Distill extracted only the title
after third-party/media scripts hit missing `TextEncoder`, `Document`, and
related browser APIs. The page also depends on media-heavy browser behavior.
The missing APIs are individually approachable, but the complete target is a
media/runtime project rather than a narrowly scoped reader extraction fix, so
it remains deferred.

### JSFiddle — `31-jsfiddle`

**This deferral was wrong, and the reason matters.** The `unexpected token
'<'` was not JSFiddle's complexity; it was our module resolver requesting the
wrong URL and compiling the resulting HTML error page as JavaScript. The
deferral reasoning was built on a symptom we had produced ourselves.

That resolver bug is fixed and gated. JSFiddle now returns a Cloudflare
challenge, so the remaining claims — that it needs a larger DOM API surface
(`DOMParser` is genuinely absent) and isolated cross-origin preview frames —
are untested rather than disproven. They remain plausible and unproven.

The lesson generalises: a symptom that looks like a site being too complex can
be our own defect, and a scope boundary inferred from such a symptom is not
yet evidence. Airbnb below is the counter-example — its deferral was re-tested
against exactly this suspicion and survived.

### HTML5 Test — `34-html5-test`

The page itself and its detailed results table loaded. However, the page's
central purpose is browser feature detection: Chromium reported `521 out of
555 points`, while Distill reported `113 out of 555 points`. The surrounding
text had about 98% reference vocabulary coverage, but the score is materially
different information. Matching it would require implementing the missing
browser features being measured; this is not a malformed-HTML parser fix.

## A defect the live ladder could not see: shape selection

The comparison harness scored Wikipedia as "checked and working" throughout.
The reader was rendering it as a search box.

`shape_for` decided a page's kind from the first matching element, checked in
a fixed order with the article test last. Wikipedia's first form is the site
search box, so every Wikipedia article — in every language — was classified
`form`, and the reader rendered the search field while discarding 137,800
characters of extracted article body. Python's documentation behaved the same
way. BBC News and GitHub Trending fell to `image-heavy` for the same reason.

Two separate blind spots let this stand:

- **The live comparison could not see it.** Its information summary counts
  blocks, links, and media, which are present regardless of which extractor is
  selected. Vocabulary coverage stayed high while the reader output was wrong.
- **The local corpus could not see it either.** Every extraction fixture
  presents one shape cleanly — a table, a form, a gallery — so the classifier
  was never asked what to do when signals compete, which is what every real
  page does.

The fix measures proportion rather than presence: a page whose visible
paragraph text passes a floor is an article even when it also carries a search
box, a table, images, or an embed. A table wins only when it holds most of the
page's text *and* the page is not otherwise prose. Documentation is
distinguished from prose by `<pre>` blocks rather than inline `<code>`, since
prose that mentions a function name is still prose.

Gate `23-composite-shape` pins it with an article that also carries a search
form, a data table, four images, and an iframe. It reports `form` on the
pre-fix engine and `article` after.

The lesson is about the corpus's structure, not this one function. An
isolating fixture suite proves each capability correct alone and says nothing
about them in combination; a page that is only ever one thing cannot catch a
classifier that picks the wrong thing.

## Writing systems

Until now no fixture in the corpus and no entry in the live ladder contained
any non-Latin text, in a stack that carries HarfBuzz for shaping and
libunibreak for line-break opportunities. Neither was exercised beyond what
space-separated Latin requires.

Three local fixtures (`24-rtl-arabic`, `25-cjk-linebreak`,
`26-complex-script`) and five live rungs (Arabic, Hebrew, Japanese, Thai, and
Hindi Wikipedia) now cover it. Extraction proved sound: direction and language
metadata survive, and text comes through intact in every script tested.

Rendering did not. Rendering the fixtures and looking at the output found:

| Finding | Status |
| --- | --- |
| RTL articles laid out flush left | **Fixed.** The reader renders its own template, so the source document's direction was simply never carried across. It now emits `dir` and `lang` and mirrors its physical-side rules |
| CJK, Thai, and Devanagari render as tofu | **Open.** `select_font_file` chooses one face by family name and never asks whether it contains the required glyphs; `--font-dir` also suppresses the system font scan. Per-run fallback needs a face list and run itemisation, which is a contained feature but not a tweak |
| Thai paragraphs overflow the column | **Open.** No inter-word spaces and no dictionary segmentation makes the paragraph one unbreakable run |
| Mixed-direction ordering unverified | **Open.** There is no Unicode Bidirectional Algorithm in the stack. HarfBuzz orders a single run correctly — pure Arabic renders in the right order — but nothing reorders between runs, which is where embedded Latin, digits, and mirrored punctuation live |

These are recorded rather than gated. A substring assertion cannot judge
whether a glyph was drawn in the right place; that needs reference images,
which the corpus deliberately does not have. What has changed is that the gaps
are now attached to pages that demonstrate them.

## A failure that was fixed: Three.js

Three.js (`32-threejs-examples`) initially looked like a large extraction
failure. The post-JavaScript DOM actually matched Chromium's 608 links and 608
images, but the reader selected its embed path because the page contains an
inline `srcdoc` iframe. That path emitted only category headings and a video
reference, leaving the 608 example names out of the reader.

The focused fix was to preserve extracted link records as a link index in the
reader's `video/embed` and `image-heavy` branches. The comparison scorer was
also taught to count those links as information for media/embed extractors.
After rebuilding, the report reached 100% vocabulary coverage and fidelity,
with all 15 headings and 608 links/images represented. The engine still does
not render or extract WebGL pixels, which is intentional; the example names
and destinations are the useful information on the index page.

## Historical `dn.se` crash

Before the 34-site comparison ladder, an external `dn.se` smoke test caused
the child process to terminate with signal 11. The repository's older live
diagnostic identifies DN article pages as a particularly ambiguous target:
the article body is absent from the fetched HTML behind a Didomi consent wall
and a paywall, while the front page is a different, server-rendered target.

The local corpus now includes DOM-churn, node-identity, detached-node, and
observer gates specifically to expose the raw-pointer lifetime class of crash
before it happens on live sites. This report does not claim that the current
DN article target has been revalidated; it remains a diagnostic rather than a
passing comparison.

## What we changed during the investigation

The work was deliberately incremental and evidence-driven:

1. Built the live comparison harness with local Chromium as the reference and
   `build/distill` as the system under test. Each run saves DOM, extraction,
   timing, exit-code, and stderr evidence.
2. Corrected the harness's HTML handling for void elements such as `meta` and
   `link`, so these tags cannot make the parser hide the rest of a page.
3. Added canonical-URL reuse for Wikipedia's nondeterministic
   `Special:Random`, ensuring Chromium and Distill compare the same selected
   article.
4. Added runtime compatibility for APIs observed on modern pages, including
   CSS visibility helpers, `AbortController`, `CustomEvent`, `Intl`,
   `PerformanceObserver`, `NodeFilter`, `URL`/`URLSearchParams`, viewport and
   geometry shims, `IntersectionObserver`/`ResizeObserver`, and animation
   scheduling fallbacks.
5. Added POST and request-body support to the `fetch`/XHR-to-libcurl bridge and
   aligned the reported user agent with Chromium's desktop form.
6. Added per-run shared cookies, browser-like request defaults, page-supplied
   fetch/XHR headers, response-header capture, redirect metadata, and a local
   network-contract regression fixture.
7. Preserved link information for media/embed-shaped reader output, which
   fixed the Three.js example-index case.
8. Fixed module specifier resolution to follow the importing module's URL,
   added HTTP status and content checks to the module loader, and excluded
   `nomodule` fallbacks from execution.
9. Harvested JSON-LD into the extraction result, so a page's own record
   survives an application that never hydrates.
10. Negotiated HTTP/2 and bounded redirect following.
11. Classified recognised bot challenges with a distinct exit status, before
    their scripts spend the settle budget, and gated the false-positive case
    after the first detector misclassified two working pages.
12. Classified anti-bot pages and browser-runtime boundaries explicitly instead
    of repeatedly changing extraction code for problems it cannot solve.

The Airbnb work is the clearest example of the boundary: several missing APIs
were fixed, then the comparison was stopped when the remaining evidence
showed a broader browser-semantics problem rather than one missing symbol.

## Current technical stack

Distill is a C23-rooted, content-first browser-like pipeline:

| Layer | Current implementation |
| --- | --- |
| Application/core | C23 production code; CMake requires C23 and bespoke runtime, extraction, rendering, and GTK-shell code are written in C23 |
| JavaScript | Embedded QuickJS-ng, with resource limits and a settle policy for timers, microtasks, requests, and script execution |
| Page DOM | A LinkeDOM-compatible JavaScript DOM surface runs page scripts inside QuickJS; the settled result is serialized for native extraction |
| Native parsing/extraction | Lexbor parses the settled HTML and provides native traversal; Readability.js is run as the article extractor, alongside structured blocks, tables, search results, forms, video metadata, media references, and links |
| Networking | libcurl with per-run shared cookies, request/response headers, negotiated HTTP/2, bounded redirects, GET/POST, and the page/subresource bridge |
| Reader layout | litehtml lays out Distill's own reader template rather than arbitrary source-site CSS |
| Font/render path | FreeType for glyph metrics/rasterization, HarfBuzz for shaping, libunibreak for line-break opportunities, and Cairo for desktop PNG/surface painting |
| Desktop shell | GTK3 with a Cairo reader surface and native GTK form widgets positioned from litehtml geometry |
| C++ boundary | litehtml is C++; `src/litehtml_probe.cpp` is the deliberate C++17 adapter. The rest of the bespoke application is C23, but binaries using litehtml still carry the C++ runtime |
| Testing | Python standard-library harness, local Chromium reference, local HTML corpus, CTest unit tests, and ignored live comparison reports |

External source dependencies are downloaded and built below `external/`, which
is gitignored. The build uses the external prefix for components such as
QuickJS-ng, Lexbor, litehtml, and Gumbo, while system/pkg-config dependencies
provide libcurl, GTK3, Cairo, FreeType, HarfBuzz, and libunibreak.

## Intentional scope limits that explain the results

The project is not trying to become Chromium. The plan intentionally excludes:

- solving Cloudflare, CAPTCHA, paywall, or bot-fingerprint challenges;
- arbitrary source-site CSS layout, including a complete flexbox/grid engine;
- a full WebGL, video, or audio pipeline;
- complete Web Components, closed/open shadow-tree traversal, and browser-grade
  cross-origin iframe contexts;
- full WHATWG browser API coverage and perfect framework scheduling semantics;
- a general-purpose browser's unbounded waiting behavior.

Distill extracts content and re-renders it using its own template. This is why
decorative pixels, ads, source CSS, and many site-specific controls can be
discarded while article text, headings, forms, links, and media references are
kept. It is also why a page whose useful output is entirely inside a live
canvas, a shadow tree, or a multi-frame editor can hit a real boundary.

## Main remaining technical problems

1. **Browser API breadth.** Modern applications continue to expose missing
   globals and methods. Adding them one at a time helps, but the long tail is
   not a complete browser implementation.
2. **Layout-dependent hydration.** Responsive applications can branch on
   geometry and observer state before they request their actual content.
   Benign fixed values are enough for many pages and wrong for others.
3. **Frames and encapsulation.** The current pipeline owns one settled DOM;
   it does not provide independent browsing contexts with origin, lifecycle,
   storage, and messaging semantics for every iframe or shadow tree.
4. **Modern module and network behavior.** Cookies, request/response headers,
   HTTP/2, and module specifier resolution are now covered, and a module that
   fails to load reports why. Import maps and bare specifiers are still
   unimplemented, and credentials, cache behavior, streaming, service workers,
   and the complete Fetch/CORS model remain outside the bridge.
5. **Untrusted script isolation.** Interrupts, memory limits, and script
   limits prevent hangs and bound resource use, but the JavaScript realm still
   runs in-process. A separate process or stronger sandbox is the eventual
   security direction for hostile input.
6. **Text rendering beyond Latin.** Extraction handles every script tested,
   but the render path selects a single font face by family name with no
   coverage check or fallback chain, has no Unicode Bidirectional Algorithm,
   and cannot break lines in scripts without word delimiters. Font fallback is
   the tractable piece and the precondition for the rest.
7. **C++ rendering dependency.** The production core is C23, but keeping
   litehtml preserves a C++ dependency at the rendering boundary. Removing it
   would require replacing the layout engine or accepting a larger rendering
   project.

## Reproduction and evidence

Build and run the local gates:

```sh
cmake --build build -j2
python3 corpus/check.py
python3 corpus/check.py --extract
ctest --test-dir build
```

Run a live comparison by manifest ID:

```sh
python3 tools/compare_sites.py --id 31-jsfiddle
```

Inspect the corresponding `report.md`, `comparison.json`, raw DOM files, and
stderr files below `reports/site-comparison/<id>/`. The complete per-site
status table remains in [`SITE_COMPARISON.md`](SITE_COMPARISON.md).
