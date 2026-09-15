# Live-site comparison test

This repository now has a diagnostic test setup for comparing what Distill
loads and extracts with what a regular Chromium browser loads from the same
URL.

It is intentionally separate from the local corpus gates. Local fixtures are
stable regression tests. Live sites change, personalize responses, show
consent or paywall states, and may block one of the clients, so a live result
is evidence for investigation rather than a permanent pass/fail assertion.

## What is included

[`corpus/site-comparison.json`](corpus/site-comparison.json) contains the 25
sites from the supplied list, corrected where the original links were search
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
25 entries:

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
6. Preserve the before/after report when the result is useful; never promote
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

The first comparison has been completed:

| Site | Status | Result |
| --- | --- | --- |
| Example Domain (`01-example-domain`) | checked and working | Same substantive information; decorative markup differences ignored |
| Info.cern.ch (`02-info-cern`) | checked and working | Same substantive information, headings, and links |
| Markdown Nice (`03-markdown-nice`) | checked and working | Same article titles and visible article metadata; decorative navigation, icons, advertising, and footer omitted |
| W3Schools HTML Tutorial (`04-w3schools-html`) | checked and working | DOM content and interactive exercise question match; decorative navigation and service links omitted from extraction |
| Hacker News (`05-hacker-news`) | checked and working | Same 30 ranked stories, metadata, and navigation links; table presentation intentionally reduced to reader content |
| BBC News (`06-bbc-news`) | checked and working | Same headlines, summaries, sections, and links; cookie consent and other decorative UI omitted |
| DEV Community (`07-dev-community`) | checked and working | Same article feed, titles, descriptions, tags, and surrounding content; navigation and ad UI omitted |
| Medium (`08-medium`) | stopped — anti-bot state differs | Chromium receives a hard Cloudflare block and Distill receives a Cloudflare challenge; neither reaches Medium content |

Testing stopped at site 8; sites 09-25 were not started.
