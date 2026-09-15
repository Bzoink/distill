# Live-site comparison test

This repository now has a diagnostic test setup for comparing what Distill
loads and extracts with what a regular Chromium browser loads from the same
URL.

It is intentionally separate from the local corpus gates. Local fixtures are
stable regression tests. Live sites change, personalize responses, show
consent or paywall states, and may block one of the clients, so a live result
is evidence for investigation rather than a permanent pass/fail assertion.

## What is included

[`corpus/site-comparison.json`](corpus/site-comparison.json) contains the 34
sites from the supplied lists, corrected where the original links were search
redirects or incomplete examples. It records the URL, tier, site class, test
focus, and important reproducibility notes.

The test runner is
[`tools/compare_sites.py`](tools/compare_sites.py). It uses:

- local Chromium as the reference browser;
- the built `build/distill` executable as the system under test;
- no Python packages beyond the standard library.

If a future request names a site without giving a URL, the assistant may use
web search to resolve it. The comparison itself still uses local Chromium so
the reference is a captured browser DOM rather than a search-result summary.

For each site it captures Chromium's post-JavaScript DOM, Distill's
post-JavaScript DOM, and Distill's structured extraction JSON. It then writes a
machine-readable comparison containing unique visible-word overlap,
information fidelity and coverage, headings, links, images, forms, timings,
exit codes, and stderr diagnostics. Screenshots can also be requested, but they are for
inspection only: Distill deliberately renders its own reader presentation and
is not expected to match the source site's pixels.

## Run one supplied site

After building Distill, run a manifest entry with:

```sh
python3 tools/compare_sites.py --id 01-example-domain
```

Artifacts are written below `reports/site-comparison/`, which is ignored by
git. A user-supplied URL can be compared without editing the manifest:

```sh
python3 tools/compare_sites.py https://example.com/ \
  --out reports/site-comparison/example
```

To capture PNGs as well:

```sh
python3 tools/compare_sites.py --id 06-bbc-news --screenshots
```

To run a selected group, repeat `--id`; omitting both URLs and `--id` runs all
34 entries:

```sh
python3 tools/compare_sites.py \
  --id 01-example-domain \
  --id 05-hacker-news \
  --id 11-react
```

The default command reports capture errors but exits successfully so the live
suite remains diagnostic. Add `--strict` when a surrounding script should
fail on a Chromium, Distill DOM, or Distill extraction capture error. A
successful capture does not mean the outputs are equivalent; inspect each
`report.md` and `comparison.json`.

## How to interpret “same information”

The comparison has two separate questions:

1. Did both clients reach usable, settled content?
2. Did Distill preserve the information that a normal browser exposed?

The reference summary includes visible text, headings, links, images, forms,
and the document title. The Distill information summary comes from its
extraction schema (`readability`, blocks, tables, search results, forms,
videos, and media). The report gives two directional measurements:

- `reference_coverage`: how much of the reference's unique visible vocabulary appears in Distill's
  extracted information;
- `distill_information_fidelity`: how much of Distill's extracted information's
  unique vocabulary appears in the reference text.

These are signals, not universal thresholds. A news homepage contains menus,
ads, and recommendations that Distill is supposed to omit. Conversely, a
client-rendered application may expose little useful text to either client.
The raw DOMs and stderr files are kept so a low score can be classified as
missing browser API support, an extraction mistake, a settle-timing issue, a
blocked request, consent/paywall behavior, or an intentional scope boundary.

## Iteration workflow

When a site is supplied for investigation:

1. Run the same URL through Chromium and Distill.
2. Read the report and inspect the saved raw artifacts.
3. Classify the difference before changing code.
4. If the difference is within Distill's intended scope, add a focused local
   fixture and regression assertion.
5. If it is a missing capability or a real bug, modify Distill and rerun the
   same comparison.
6. If both clients are stopped by an anti-bot, CAPTCHA, access-denied, or
   equivalent protection page, mark the site `antibot`, preserve the evidence,
   and continue with the next site. Do not treat that state as a Distill bug.
7. If a site requires browser-level behavior outside Distill's current scope
   and the difference remains after focused fixes, mark it `out of scope —
   deferred`, document the evidence and likely architectural cause, and
   continue. This is neither a pass nor an anti-bot result; it is a candidate
   for a later browser-runtime project.
8. Preserve the before/after report when the result is useful; never promote
   an unstable live URL directly into a CI gate.

The reference run uses a fresh temporary Chromium profile for each site. It
does not contain login state, extensions, or a user-supplied consent choice.
Therefore a site requiring authentication, geolocation, a particular cookie,
or a concrete article/profile/video URL must be supplied with those details
if that state is part of the test.

## Limitations of the supplied list

Several entries are intentionally placeholders rather than deterministic
content targets:

- Wikipedia `Special:Random` chooses a different article per run.
- Medium, X, Notion, and YouTube need a concrete article, profile, public
  document, or watch URL for a focused content test.
- The Cloudflare URL is the public Cloudflare site, not a guaranteed
  challenge page.

Those entries are still useful for capability and blocking diagnostics, but a
reproducible comparison should use a pinned URL and record the final state.

## Current setup status

The original 25-site comparison has been completed:

| Site | Status | Result |
| --- | --- | --- |
| Example Domain (`01-example-domain`) | checked and working | Same substantive information; decorative markup differences ignored |
| Info.cern.ch (`02-info-cern`) | checked and working | Same substantive information, headings, and links |
| Markdown Nice (`03-markdown-nice`) | checked and working | Same article titles and visible article metadata; decorative navigation, icons, advertising, and footer omitted |
| W3Schools HTML Tutorial (`04-w3schools-html`) | checked and working | DOM content and interactive exercise question match; decorative navigation and service links omitted from extraction |
| Hacker News (`05-hacker-news`) | checked and working | Same 30 ranked stories, metadata, and navigation links; table presentation intentionally reduced to reader content |
| BBC News (`06-bbc-news`) | checked and working | Same headlines, summaries, sections, and links; cookie consent and other decorative UI omitted |
| DEV Community (`07-dev-community`) | checked and working | Same article feed, titles, descriptions, tags, and surrounding content; navigation and ad UI omitted |
| Medium (`08-medium`) | antibot | Chromium receives a hard Cloudflare block and Distill receives a Cloudflare challenge; neither reaches Medium content; continue with site 09 |
| Wikipedia Random Article (`09-wikipedia-random`) | checked and working | Chromium's randomly selected article URL is reused for Distill through the page canonical URL; same article information preserved, with reader-mode reductions in decorative links, images, and forms |
| Stack Overflow (`10-stack-overflow`) | antibot | Chromium receives the site, but Distill receives Cloudflare's `Just a moment...` page; no meaningful content comparison is possible |
| React Official Website (`11-react`) | checked and working | Same rendered documentation content, headings, links, images, and forms; reader extraction omits only a small amount of surrounding UI/code context |
| Twitter / X (`12-x`) | antibot | Chromium reports `Access to x.com was denied` and Distill receives only the unauthenticated login shell; the supplied URL has no concrete public profile |
| Airbnb (`13-airbnb`) | out of scope — deferred | Chromium exposes the homepage content; Distill settles on its navigation shell and `0 of 0 items showing`. Focused fixes addressed the observed missing APIs, POST/XHR bodies, user agent, and layout shims, but the React application still does not hydrate. See the deferral rationale below. |
| Notion (`14-notion`) | checked and working | Same substantive landing-page content and media; a 15-second script budget was needed for dynamic discovery, and minor heading wording differs between captures |
| GitHub Trending (`15-github-trending`) | checked and working | Same trending repository list, headings, descriptions, links, and images; GitHub's large navigation and marketing shell is intentionally omitted from reader information |
| YouTube (`16-youtube`) | out of scope — deferred | The supplied URL is the home page rather than a concrete watch page. Chromium exposes consent/history UI, while Distill cannot complete YouTube's Web Components/shadow-DOM bootstrap; this needs a real browser-level runtime and a pinned watch URL |
| Imgur (`17-imgur`) | out of scope — deferred | Chromium receives Imgur's media/consent page, while Distill extracts only the title after third-party/media scripts fail on missing `TextEncoder`, `Document`, and related browser APIs; this is deferred with the heavy-media runtime work |
| Reddit (`18-reddit`) | antibot | Chromium and Distill both receive Reddit's `Prove your humanity` challenge page; the available information matches, but no Reddit content is accessible |
| CodePen Trending (`19-codepen-trending`) | antibot | Chromium and Distill both receive CodePen/Cloudflare's `Performing security verification` page, which reports the browser is not supported |
| Product Hunt (`20-product-hunt`) | antibot | Chromium receives Product Hunt's Cloudflare verification page while Distill reaches the site content; the reference is blocked, so this is not a valid content comparison |
| Cloudflare (`21-cloudflare`) | checked and working | Same substantive public homepage information with high Distill fidelity; navigation, cookie UI, and decorative marketing structure are reduced, and non-content custom-element/video errors do not prevent extraction |
| Zillow (`22-zillow`) | checked and working (was `antibot`) | After the network remediation Distill reaches Zillow content — real title, 1448 characters, and 271 links — where both clients previously received `Access to this page has been denied`. Zillow still serves the PerimeterX block page to some requests, which Distill now classifies rather than extracting as empty |
| IMDb (`23-imdb`) | antibot | The flat `403 Forbidden` is gone: IMDb now answers `202` with an AWS WAF challenge. The request shape was a real defect and is fixed; the challenge behind it is not ours to solve, and is now classified as `aws-waf` |
| Bloomberg (`24-bloomberg`) | antibot | Chromium and Distill both receive Bloomberg's unusual-activity robot check; matching text is only the shared protection page |
| Target (`25-target`) | antibot | Chromium receives the Target homepage, while Distill receives Target's `Pardon the interruption` bot-mitigation page; this is not a content extraction mismatch |

Testing completed for all 25 original entries: 12 checked and working, 10
marked `antibot`, and 3 marked `out of scope — deferred`. The additional
challenge entries below have also been completed.

### Additional challenge ladder

The second supplied challenge list adds nine entries, numbered 26–34:

| Tier | Sites | Primary challenge |
| --- | --- | --- |
| 6 | MDN Web Components; Shoelace | custom elements, slots, and shadow DOM |
| 7 | Hacker News Newest; DEV Community Feed | long feeds, pagination, and dynamic list growth |
| 8 | CodePen; JSFiddle | isolated and cross-origin iframe content |
| 9 | Three.js Examples; Chart.js | canvas, SVG, and WebGL-rendered information |
| 10 | HTML5 Test | malformed HTML and parser recovery |

Site 29 intentionally retests the DEV Community URL from site 07 with the
infinite-feed focus. Site 30 uses CodePen's landing page because the supplied
list did not provide a concrete Pen/embed URL; a pinned Pen should be added for
a deterministic iframe test.

The additional entries are tested sequentially. The same classification rules
apply: fix focused Distill defects, mark anti-bot responses as `antibot` and
continue, and mark genuine browser-runtime limits as `out of scope — deferred`
with evidence rather than treating them as extraction failures.

### Additional challenge status

| Site | Status | Result |
| --- | --- | --- |
| MDN Web Components (`26-mdn-web-components`) | checked and working | Same substantive documentation, headings, and links; shadow-DOM concepts are represented in the page's ordinary extracted content |
| Shoelace (`27-shoelace`) | checked and working | Same substantive component-library content, links, and images; a promotional banner's heading classification differs, but its text is preserved |
| Hacker News Newest (`28-hacker-news-newest`) | checked and working | Same newest-story feed information with 100% reference vocabulary coverage and identical links, images, and form; extra reader terms reduce reverse fidelity but do not remove feed content |
| DEV Community Feed (`29-devto-infinite-feed`) | checked and working | Same hydrated feed cards, titles, descriptions, links, and images; reader extraction omits surrounding UI and formats one date as an ISO timestamp rather than Chromium's short date |
| CodePen (`30-codepen-embed`) | antibot | Chromium and Distill both receive CodePen's Cloudflare security-verification page; Distill also reports that its browser is unsupported, so no landing-page or iframe content is available |
| JSFiddle (`31-jsfiddle`) | antibot | The `unexpected token '<'` was our own defect, not JSFiddle's: module specifiers resolved against the page URL instead of the importing module's, so every nested import fetched the site's HTML fallback. That is fixed and covered by gate `19-module-graph`. JSFiddle itself now returns a Cloudflare challenge, so the editor/iframe scope question is untested rather than settled; `DOMParser` and multi-frame semantics remain genuinely absent |
| Three.js Examples (`32-threejs-examples`) | checked and working | Reader now preserves the example index as links when an inline viewer iframe selects the embed path; refreshed comparison has 100% vocabulary coverage/fidelity, all 15 headings, and 608 links/images, while WebGL pixels remain outside text extraction |
| Chart.js (`33-chartjs-gallery`) | checked and working | Same chart-library overview, headings, and links; 100% reference vocabulary coverage with identical links and images, while canvas-rendered chart pixels are intentionally not extracted |
| HTML5 Test (`34-html5-test`) | out of scope — deferred | The page and detailed table load, but its central feature-detection score differs substantially: Chromium reports 521/555 while Distill reports 113/555; matching that diagnostic requires a much broader browser API surface, not a focused malformed-HTML parser fix |

The additional challenge ladder is complete: 6 entries are checked and
working, 1 is marked `antibot`, and 2 are marked `out of scope — deferred`.
For media/embed-shaped pages, the reader now preserves extracted link records
as a link index, so pages such as Three.js retain their example names even
when their visual content is canvas/WebGL or an iframe. The comparison scorer
counts those links as information for the corresponding media/embed
extractors.

Across all 34 entries, 18 are checked and working, 11 are marked `antibot`,
and 5 are marked `out of scope — deferred`. No entry remains pending.

### Revisions after the module and classification work

Three rows above changed, and the direction of the changes is worth stating
plainly: one site moved out of `antibot` because our client improved, one
stayed `antibot` for a different reason than we recorded, and one was never an
anti-bot problem at all in the way we described.

- **Zillow** moved to `checked and working`. The likely cause is protocol
  version negotiation: the client advertised Chrome while speaking HTTP/1.1.
- **IMDb** stays `antibot`, but the recorded `403 Forbidden` was partly our
  own request shape. It now answers `202` with an AWS WAF challenge.
- **JSFiddle** moved from `out of scope — deferred` to `antibot`. Its
  `unexpected token '<'` was a Distill bug — module specifiers resolved
  against the page rather than the importing module — and the deferral
  reasoning built on that symptom was therefore built on our own defect. The
  site now serves a Cloudflare challenge, so its editor and cross-origin frame
  requirements are untested rather than disproven.

The last of these is the one to learn from. A symptom that looks like a site's
complexity can be our own bug, and a scope boundary inferred from such a
symptom is not yet evidence. The counterpart holds too: Airbnb's deferral was
re-tested against the same suspicion and survived it.

### Exit status and mechanical classification

Distill reports a recognised bot challenge with status `4`, distinct from `0`
for a content run and from any other status for a capture failure. The
comparison harness records that as `challenge` rather than `error`, and
`--strict` does not fail on it: a challenge is an access result we recorded,
not a capture that broke.

The classification happens twice — against the served HTML before any script
runs, and against the settled DOM afterwards. The first is what saves the
settle budget, since a challenge shell would otherwise spend all of it. The
second catches challenges that write their message from script, as Reddit and
Target do; by then the budget is gone, but the run is still classified
correctly instead of being reported as an unexplained empty extraction.

Six entries currently classify this way: Medium, Stack Overflow, Reddit,
CodePen, IMDb, and Target, with Zillow and JSFiddle doing so intermittently.
Marker choice is deliberately conservative — see `22-challenge-vendor-scripts`
in [`corpus/README.md`](corpus/README.md) for why naming a vendor is not
evidence.

### Airbnb deferral rationale

Airbnb is not an anti-bot result: the regular Chromium reference received the
homepage, while Distill received a successful HTTP response and executed its
scripts. The failure is that the two environments do not expose equivalent
browser behavior. The final comparison recorded 245 reference vocabulary words
and seven headings, but only eight extracted Distill words and a placeholder
`0 of 0 items showing` state; Distill also hit its 30-second settle cap after
48 scripts.

The first run exposed missing `CSS`, `AbortController`, `CustomEvent`, `Intl`,
`PerformanceObserver`, `NodeFilter`, `URL`, `visualViewport`, and range/layout
methods. Those were given focused compatibility shims. The runtime also gained
POST/body handling for `fetch` and `XMLHttpRequest`, plus a Chromium-like user
agent. These changes removed the fatal JavaScript errors, but did not make the
React application populate its content.

The remaining gap is architectural rather than one missing function. Airbnb's
hydration path expects real layout measurements and observer updates, complete
DOM and event semantics, browser request metadata/cookies/headers, and browser
accurate asynchronous scheduling. Distill currently uses QuickJS and LinkeDOM
with intentionally lightweight layout and browser-global shims; returning fixed
geometry or empty observer data can leave a responsive application in its
placeholder state even when its scripts technically execute. During tracing,
only telemetry POSTs completed and no listing/content request populated the
page. Matching this site reliably would therefore require either a real browser
runtime or a substantially broader browser-compatibility layer. It is deferred
for later investigation rather than counted as a working comparison.
