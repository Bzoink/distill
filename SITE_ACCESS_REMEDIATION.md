# Site access: findings and proposed measures

Date: 2026-09-15

Companion to [`SITE_ACCESS_REPORT.md`](SITE_ACCESS_REPORT.md). That report
recorded *what happened* across the 34-site comparison ladder and correctly
refused to treat access failures as extraction bugs. This document asks the
next question: for the 16 entries that did not produce a usable comparison,
**what legitimate route to the information exists, and what would we build to
take it?**

Tier 1 has now been implemented in the C23/libcurl bridge and checked with a
local browser-valid contract fixture. The site-by-site predictions below are
updated with the first live retest; Tier 2 and later items remain proposals.

## Tier 1 implementation status

Implemented:

- per-run libcurl cookie state shared by navigation, fetch, XHR, and scripts;
- `document.cookie` round-tripping through that cookie state;
- `Accept`, `Accept-Language`, `Accept-Encoding`, and subrequest `Referer`
  defaults, with browser-like `Accept: */*` for fetch/XHR subrequests;
- page-supplied `fetch` headers and XHR `setRequestHeader` forwarding;
- caller-selected request content types instead of a forced JSON content type;
- effective redirect URLs and response-header access through `get`, `has`,
  `forEach`, `entries`, `keys`, and `values`;
- a local regression fixture covering cookies, headers, POST bodies, response
  headers, redirects, and XHR.

The local engine gate and Chromium baseline both pass. This does not bypass
anti-bot challenges, and HTTP-version negotiation, structured-data harvesting,
imported user sessions, robots handling, and rate limiting remain open work.

## Executive summary

Three findings change how the unsuccessful entries should be prioritised.

1. **Distill's HTTP client is materially less conformant than its user-agent
   string claims.** It sends no `Accept`, `Accept-Language`, or
   `Accept-Encoding`; captures no response headers; keeps no cookie jar; and
   never sends a `Referer`. Some share of the `antibot` column is likely a
   consequence of this rather than of a site's intent to block automation.
2. **`fetch()` silently discards page-supplied request headers and forces
   `Content-Type: application/json` on every request with a body.** Modern
   application data fetches depend on `Accept`, `Authorization`, API-key, and
   CSRF headers. This is a plausible — and cheap to test — alternative
   explanation for the Airbnb result, which the previous report attributed to
   layout-dependent hydration.
3. **Most of the blocked sites publish a sanctioned machine-readable route**
   (API, RSS, oEmbed, or bulk dataset). For those, there is no protection to
   work around; the pipeline is simply knocking on the wrong door.

The first two are protocol-correctness defects that happen to affect access.
They are the highest value work on this list and involve no bot-challenge
handling whatsoever.

## Part 1 — Findings in the network layer

All items below were verified against the pre-Tier-1 tree that motivated this
document. The implementation-status section above records which findings are
now addressed. "Absent" means the symbol appeared nowhere under `src/` in
that baseline.

| # | Finding | Evidence | Consequence |
| --- | --- | --- | --- |
| F1 | No cookie jar. `CURLOPT_COOKIEFILE`/`CURLOPT_COOKIEJAR` absent. `document.cookie` is an in-memory string with no connection to the network layer | [`src/main.c:852`](src/main.c#L852) | `Set-Cookie` never round-trips. Cloudflare clearance, consent dismissal, and any session are discarded between requests |
| F2 | No response headers captured. `CURLOPT_HEADERFUNCTION` absent; `Response.headers.get/has/forEach` are stubs returning null/false/undefined | [`src/main.c:555-584`](src/main.c#L555-L584) | The engine cannot see `Set-Cookie`, `Content-Type`, redirects, or rate-limit headers. Scripts that branch on response content type take the wrong branch |
| F3 | `fetch()` reads only `method` and `body` from its init object; `init.headers` is dropped | [`src/main.c:598-601`](src/main.c#L598-L601) | `Accept`, `Authorization`, `X-CSRF-Token`, and vendor API-key headers never leave the process |
| F4 | Any request with a body is hardcoded to `Content-Type: application/json` | [`src/main.c:477-481`](src/main.c#L477-L481) | Form-encoded posts, GraphQL, and text bodies are mislabelled. Servers reject or misparse them |
| F5 | No `Accept`, `Accept-Language`, or `Accept-Encoding` on any request, while advertising a Chrome 152 user agent | [`src/main.c:342`](src/main.c#L342), [`src/main.c:465`](src/main.c#L465) | The request is self-inconsistent. A client claiming Chrome that does not advertise gzip/brotli is anomalous to a CDN on its face |
| F6 | No `CURLOPT_HTTP_VERSION`; HTTP/1.1 is likely negotiated while claiming to be a browser | absent | Another axis on which the request does not match its claimed identity |
| F7 | No `Referer` on subresource or XHR requests | absent | Some CDNs and asset hosts reject cross-origin fetches with no referer |
| F8 | No `robots.txt` parsing and no per-host rate limiting | absent | We have no mechanical basis for claiming our access is well-behaved |
| F9 | No structured-data harvesting. `ld+json`, `__NEXT_DATA__`, `__NUXT__` appear nowhere | absent | Content already present in the server HTML as JSON is ignored, and the pipeline instead attempts a full hydration it may not be able to complete |

### F3/F4 and the Airbnb conclusion

`SITE_ACCESS_REPORT.md` records that tracing "saw telemetry POSTs but no
listing/content request that populated the page", and concludes the remaining
problem is architectural — layout measurement, observer state, and scheduling
semantics.

That conclusion may still be right, but F3 and F4 offer a simpler competing
explanation that was not ruled out. Airbnb's own data endpoints require a
vendor API-key header. Under F3 that header is discarded before the request
leaves the process; under F4 the body is relabelled. A request built correctly
by the page would therefore still fail at the bridge, and the symptom —
telemetry succeeds, content fetch never populates — matches.

The distinction matters because the two diagnoses imply very different
projects. It is cheap to separate them: see M2's verification step below.

## Part 2 — Route inventory for the unsuccessful entries

Reclassifying the 11 `antibot` and 5 deferred entries by *what route exists*
rather than by *what went wrong*:

### Class A — a sanctioned machine-readable route exists

No protection needs to be handled. These are integration work.

| Entry | Route |
| --- | --- |
| `10-stack-overflow` | Stack Exchange API 2.3 (`api.stackexchange.com`); periodic data dumps |
| `18-reddit` | OAuth API with a registered application; `/r/<sub>/.rss` for feeds |
| `08-medium` | Per-author and per-publication RSS (`medium.com/feed/@user`) |
| `20-product-hunt` | Official GraphQL API v2, free developer token |
| `19-codepen-trending`, `30-codepen-embed` | oEmbed endpoint (`codepen.io/api/oembed`) |
| `23-imdb` | Non-commercial bulk datasets (`datasets.imdbws.com`); TMDb or OMDb for per-title lookups |
| `16-youtube` | Data API v3, `youtube.com/oembed`, published caption tracks |
| `17-imgur` | `api.imgur.com` with a registered client ID |
| `31-jsfiddle` | Published embed/show URLs for a given fiddle |

### Class B — gated or commercial only

`12-x`, `24-bloomberg`, `22-zillow`, `25-target`. Each has an API behind a paid
or partner agreement. The honest engineering answer is "licensed feed, a
user's own authenticated session, or nothing". They should be marked as such
in the manifest so they stop reading as extraction failures.

### Class C — plausibly our own conformance defect

`10-stack-overflow` (Cloudflare interstitial with no cookie jar to satisfy it
— F1), `23-imdb` (flat 403, consistent with F5/F6), and the `dn.se` Didomi
consent wall (consent cookie discarded — F1). These should be retested after
Tier 1 below before being called blocks at all.

### Class D — genuinely out of scope

`34-html5-test` measures browser feature support; its score *is* the answer and
matching it means implementing the features. `13-airbnb` remains here if M2's
test does not move it. This class needs no access work.

## Part 3 — Proposed measures

Ordered by value over cost. Each states what to change, where, and how we would
know it worked.

### Tier 1 — protocol correctness (addresses F1–F7) — **done**

**M1. Send a coherent request.** Add `Accept`, `Accept-Language`,
`Accept-Encoding` (let libcurl negotiate via `CURLOPT_ACCEPT_ENCODING`),
`CURLOPT_HTTP_VERSION` for HTTP/2, and a `Referer` on subresource requests, at
both call sites ([`src/main.c:342`](src/main.c#L342),
[`src/main.c:465`](src/main.c#L465)).
*Status:* implemented, except for the previously unproven explicit HTTP/2
selection. The live retest kept IMDb at 403 and Stack Overflow at its
Cloudflare challenge, confirming those are not solved by ordinary request
defaults alone.

*Verify:* re-run `23-imdb` and `10-stack-overflow`; a status change from 403 or
interstitial to 200 confirms the diagnosis.

**M2. Forward page-supplied request headers and stop forcing a content type.**
Read `init.headers` in `fetch_callback` and the XHR `setRequestHeader` path;
send the page's own `Content-Type` instead of the hardcoded JSON at
[`src/main.c:479`](src/main.c#L479).
*Status:* implemented and covered by the local contract fixture. Airbnb now
produces a successful capture, but its information coverage remains low and
the application still does not hydrate into the reference content; it remains
out of scope rather than being marked fixed.

*Verify:* re-run `13-airbnb` with request tracing. If a listing request now
leaves the process and returns content, F3/F4 was the cause and the
"architectural" diagnosis in the previous report should be revised. If it still
does not, that diagnosis is confirmed and Airbnb moves to Class D on firmer
evidence. Either outcome is worth having.

**M3. Real cookie handling.** Wire `CURLOPT_COOKIEFILE`/`COOKIEJAR` to a
per-run jar, add `CURLOPT_HEADERFUNCTION` to capture response headers, and
bridge the jar to `document.cookie` at
[`src/main.c:852`](src/main.c#L852) in both directions.
*Status:* implemented for a fresh per-run session, including response-header
capture and the JavaScript cookie bridge. The Stack Overflow interstitial and
the DN consent/paywall boundary remain; no challenge or paywall bypass was
attempted.

*Verify:* the `dn.se` consent wall and the Stack Overflow interstitial are the
tests. This also makes `Response.headers` real, retiring the F2 stubs.

Tier 1 is bounded, touches one file, and requires no new dependency. It is
protocol conformance — the request becoming what it already claims to be — not
challenge handling.

### Tier 2 — get content without completing hydration — **M4 done, M5 open**

**M4. Structured-data harvester.** Before the hydration path runs, extract
`application/ld+json`, `__NEXT_DATA__`, `__NUXT__`, and `self.__next_f`
payloads from the server HTML and feed them to the extractor as structured
blocks.
*Why it is the best value on this list:* it sidesteps the entire
layout-dependent-hydration problem (remaining problem #2 in the previous
report) for every site that ships its content in the initial HTML, which is
most server-rendered React and Next.js. It helps far beyond the five deferred
entries, and it cannot be defeated by a site's client-side complexity.

**M5. Bring-your-own-session.** A `--cookie-file` flag reading a Netscape jar
exported from the user's own logged-in browser, loaded into the M3 jar.
This is the user reaching their own authorised sessions, and it is what
`SITE_ACCESS_REPORT.md` already recommends as the correct follow-up. One
mechanism covers X, Medium's paywall, Reddit, and consent walls.

### Tier 3 — sanctioned sources and good citizenship — **open**

**M6. Sanctioned-source resolver.** A small adapter layer tried ahead of the
DOM path: per-host, check for an RSS/Atom feed, a sitemap, an oEmbed endpoint,
or a configured API adapter, and extract from that when present. Class A above
is the initial adapter set.

**M7. Archive fallback.** On a hard block, query the Wayback availability API
and extract the snapshot instead. Public, intended for exactly this, and
returns real content for Class B walls.

**M8. `robots.txt` and per-host rate limiting.** Currently absent (F8). If we
intend to argue our access is legitimate, honouring robots is the argument.

**M9. Reconsider the user-agent.** The Chrome 152 string at
[`src/main.c:342`](src/main.c#L342) is the soft edge of fingerprint spoofing,
and after M1–M3 it is no longer needed to paper over a thin request. An honest
`Distill/0.x (+contact-url)` is more defensible and is a precondition for ever
being allowlisted by a site operator. This is a judgement call worth making
deliberately rather than by default.

### Tier 4 — harness hygiene — **open**

**M10.** Pin `12-x` and `16-youtube` to concrete public permalinks rather than
homepages; give the reference Chromium a persistent profile so
`20-product-hunt` and `25-target` are not blocked on the reference side; and
report "reference blocked" as a status distinct from "Distill blocked". These
cost almost nothing and remove four misleading rows.

### Added after the first round

Three items were not in the original plan and came out of scoring it:

- **M11. Module specifier resolution.** Resolve against the importing
  module's URL, and check HTTP status and content type before compiling.
  Done; gated by `19-module-graph`.
- **M12. Challenge classification.** Recognise a challenge shell before its
  scripts spend the settle budget, and report a distinct exit status. Done;
  gated by `21-challenge-shell` and, for the false-positive case,
  `22-challenge-vendor-scripts`.
- **M13. Protocol version and redirect bound.** `CURLOPT_HTTP_VERSION` and
  `CURLOPT_MAXREDIRS`, both missed in the Tier 1 pass. Done; the probable
  cause of Zillow becoming reachable.

## Part 4 — Measures we are not proposing

Recorded explicitly so the boundary stays where `SITE_ACCESS_REPORT.md` and
`PLAN.md` put it. Out of scope, and not made in-scope by anything above:

- TLS/JA3 or HTTP/2 fingerprint spoofing;
- CAPTCHA-solving services, whether automated or human-relay;
- headless-detection patching or anti-fingerprinting shims;
- residential proxy rotation or IP evasion;
- paywall circumvention.

The distinction this document relies on: **making a request honest is not the
same as making it deceptive.** M1–M3 fix a client that misrepresents itself by
omission. M9 arguably reduces deception further. None of them attempt to defeat
a challenge, and Class B sites stay blocked under every measure here — which is
the correct outcome.

## Part 5 — Verification results

Tier 1 landed in `c25b538`. The predictions this document made have now been
scored, and a second round of fixes followed from what they showed.

| Prediction | Measure | Outcome |
| --- | --- | --- |
| IMDb's 403 is request-shape, not policy | M1 | **Partly confirmed.** IMDb no longer returns 403; it returns 202 with an AWS WAF challenge. The request shape was a real defect, and a challenge sits behind it |
| The Stack Overflow interstitial is cookie loss | M3 | **Falsified.** Cookies round-trip and the Cloudflare interstitial still appears. It is a policy result, not a defect |
| Airbnb's missing content request is header loss | M2 | **Falsified.** Headers and bodies now cross the bridge and no listing request is made. The architectural diagnosis in `SITE_ACCESS_REPORT.md` is confirmed, and Airbnb is out of scope on evidence rather than on suspicion |
| The `dn.se` consent wall is cookie loss | M3 | Not yet retested |

One prediction was not on the list and turned out to matter more than any of
them: **Zillow now returns content** (real title, 1448 characters, 271 links)
where both clients previously received an access-denied page. The probable
cause is protocol-version negotiation, added in the second round below.

### Second round: defects found while scoring the first

Reading the module path during this work exposed a resolution bug rather than
a missing capability.

| Fix | Finding | Evidence |
| --- | --- | --- |
| Module base URL | `module_normalize` discarded `base_name` and resolved every specifier against the page URL. A bundle at `/assets/app.js` importing `./chunk.js` reached `/chunk.js` | The local fixture reproduces the exact JSFiddle symptom on the pre-fix engine: `SyntaxError: unexpected token in expression: '<' at /chunk.js` |
| Module diagnostics | `module_loader` captured the HTTP status and never read it, so an HTML error page was compiled as JavaScript | Status and content are now checked and reported by name |
| `nomodule` | Pages shipping a module bundle and a fallback executed both | Gate `19-module-graph` asserts the fallback does not run |
| JSON-LD | Script discovery located every JSON block and freed it | Gate `20-structured-data` asserts the record survives a page that never hydrates |
| Protocol version | No `CURLOPT_HTTP_VERSION`; HTTP/1.1 while advertising Chrome | Zillow and IMDb both changed behaviour |
| Redirect bound | `FOLLOWLOCATION` with no `MAXREDIRS` | Bounded at 20 |
| Challenge classification | Challenge shells spent the whole settle budget before reporting nothing | Gate `21-challenge-shell` runs in 0.0s where the shell's own script would have burned 60s |

### The false positive this produced, and the guard against it

The first challenge detector used vendor names as markers. Checked against
pages that actually serve content, two were wrong: `datadome` matched Airbnb
and `/cdn-cgi/challenge-platform` matched Product Hunt, both of which serve
full pages while naming those vendors. Misclassifying them skipped the scripts
of working pages.

The detector now uses only strings unique to an interstitial, and carries a
size guard as a second line of defence. Gate `22-challenge-vendor-scripts`
locks this in with a small page that names both vendors and must still run its
scripts — small on purpose, so only the choice of markers can save it.

### Current live classification

Six targets classify mechanically as challenges (`exit 4`): Reddit, Target,
IMDb, Stack Overflow, Medium, CodePen. Ten content pages — Airbnb, Product
Hunt, GitHub, Wikipedia, Hacker News, DEV, React, Cloudflare, Three.js,
W3Schools — are unaffected, with Three.js still reporting its 608 links.

Reproduce with:

```sh
cmake --build build -j2
python3 corpus/check.py
python3 corpus/check.py --extract
ctest --test-dir build
```

## Open questions

1. Does the XHR path share `fetch`'s header handling, or does it need a
   separate fix? Worth confirming while implementing M2.
2. Should the sanctioned-source resolver (M6) live in the engine or in a
   separate tool? Putting API adapters inside a browser-like pipeline is a
   scope decision, not just an implementation one.
3. Is per-run cookie persistence (M3) acceptable, or does the eventual
   process-isolation direction (remaining problem #5) constrain where a jar
   may live?
