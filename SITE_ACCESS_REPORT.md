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
| Zillow (`22-zillow`) | Both clients received an access-denied page with a human-verification prompt. |
| IMDb (`23-imdb`) | Both clients received `403 Forbidden` and no usable IMDb content. |
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

Chromium loaded the editor UI. Distill remained at the server-rendered title
shell: its main editor module failed with `unexpected token '<'`, while a
supporting SVG loader reported missing `DOMParser`. The intended product also
uses an editor and isolated/cross-origin preview frames. The observed errors
may contain individually fixable compatibility gaps, but making JSFiddle work
requires reliable module loading, a larger DOM API surface, and multi-frame
semantics together.

### HTML5 Test — `34-html5-test`

The page itself and its detailed results table loaded. However, the page's
central purpose is browser feature detection: Chromium reported `521 out of
555 points`, while Distill reported `113 out of 555 points`. The surrounding
text had about 98% reference vocabulary coverage, but the score is materially
different information. Matching it would require implementing the missing
browser features being measured; this is not a malformed-HTML parser fix.

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
8. Classified anti-bot pages and browser-runtime boundaries explicitly instead
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
| Networking | libcurl with per-run shared cookies, request/response headers, redirects, GET/POST, and the page/subresource bridge |
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
4. **Modern module and network behavior.** Pages may depend on module graphs,
   credentials, cache behavior, streaming, service workers, and browser
   metadata that the lightweight bridge does not fully reproduce. Basic
   cookies and request/response headers are now covered, but not the complete
   Fetch/CORS/cache model.
5. **Untrusted script isolation.** Interrupts, memory limits, and script
   limits prevent hangs and bound resource use, but the JavaScript realm still
   runs in-process. A separate process or stronger sandbox is the eventual
   security direction for hostile input.
6. **C++ rendering dependency.** The production core is C23, but keeping
   litehtml preserves a C++ dependency at the rendering boundary. Removing it
   would require replacing the layout engine or accepting a larger rendering
   project.

## Reproduction and evidence

Build and run the local gates:

```sh
cmake --build build -j2
python3 corpus/check.py
```

Run a live comparison by manifest ID:

```sh
python3 tools/compare_sites.py --id 31-jsfiddle
```

Inspect the corresponding `report.md`, `comparison.json`, raw DOM files, and
stderr files below `reports/site-comparison/<id>/`. The complete per-site
status table remains in [`SITE_COMPARISON.md`](SITE_COMPARISON.md).
