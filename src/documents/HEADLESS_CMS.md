# Extracting headless CMS content into Distill

Research and proposal, 2026-09-17. Repository revision inspected: `b367725`.
This document describes existing capabilities, a live Folksam investigation,
and proposed changes. It does not implement those changes.

**Distill's JavaScript engine is capable enough to fetch JSON, transform it,
and build the content needed for a reader page. Its browser compatibility
layer is not sufficient to run every Next.js application successfully.**
The most useful improvement is to extract from several available representations
of a page, preserving useful content when application startup fails.

Folksam demonstrates why this matters: its homepage already delivers readable
HTML and structured page data, but running its scripts in the current Distill
build results in an empty extraction.

The follow-up [CMS platform survey](HEADLESS_CMS_SURVEY.md) examines eight
platforms with public example pages, exact data locations, and live-versus-static
Distill extraction results. It covers Contentful, Sanity, Storyblok, Strapi,
Prismic, DatoCMS, Directus, and Payload.

## What we need from a headless CMS

A headless CMS separates content storage from the website that presents it.
Next.js is a frontend framework, not a CMS. Seeing Next.js tells us something
about delivery, but does not identify the CMS or guarantee a public CMS API.

Distill only needs the published information for the requested page: title,
ordered sections, text, lists, tables, images and captions, meaningful links,
and supported forms. The CMS's editorial interface and the site's component
styling are unnecessary for that task.

The same information can reach us through different paths:

| Available representation | How Distill could use it |
| --- | --- |
| Server-rendered HTML | Extract semantic content directly, before executing scripts. |
| Embedded page JSON | Parse the data and map known content structures into reader blocks. |
| JSON fetched by the application | Observe responses to requests the page makes and apply a matching adapter. |
| DOM created by JavaScript | Run scripts within the existing budget, then extract the resulting document. |
| Public content endpoint | Use a site adapter when its route, schema, and page association are known. |

An API used only by the website's server may be inaccessible to the browser.
That is not necessarily an obstacle: its published result may already be in
the HTML or embedded data. Server-side Next.js data fetching can produce both
initial HTML and client-visible props. [Next.js: getServerSideProps](https://nextjs.org/docs/pages/building-your-application/data-fetching/get-server-side-props).

## What Distill already supports

The current pipeline in [main.c](../main.c) loads a linkedom-based DOM into
QuickJS-ng, installs host APIs, executes page scripts, settles pending work,
serializes the DOM, and passes it to [extraction.inc](../extraction.inc).
The reader consumes the resulting extraction JSON.

| Capability | Current implementation and practical limit |
| --- | --- |
| JavaScript execution | QuickJS-ng, promises, microtask execution, modules, and timers. This is enough for ordinary JSON-to-DOM work. |
| Network access | libcurl-backed `fetch`, response `text()` and `json()`, and a limited `XMLHttpRequest` wrapper. |
| Document manipulation | linkedom-based document and elements, selectors, and mutations; not a complete browser DOM. |
| Browser environment | Host shims for storage, events, history, observers, geometry, and other APIs. Some are deliberately approximate. |
| Resource controls | Default script cap of 5,000 ms, QuickJS memory limit of 128 MiB, and script limit of 1,000. The JS heap limit is not a whole-process memory limit. |
| Extraction | Readability, structured text, main-content selection, links, media, tables, search results, forms, and JSON-LD metadata. |
| Site hints | Declarative DOM selector profiles in [profiles](../../profiles/README.md). They do not currently map JSON content. |

The [fetch-render fixture](../../corpus/local/04-fetch-render.html) exercises
the essential CMS flow: fetch JSON, read its title and paragraphs, create
elements, and append them to the document. It also exercises XHR. This supports
a narrow capability claim, not universal React or Next.js compatibility.

The browser API layer is the more immediate limitation. For example,
`HOST_JS` supplies fixed geometry, simplified observers, and approximate
internationalization. A framework can parse and execute successfully until
it reaches a missing constructor or incompatible host behavior. Increasing
the script timeout cannot fix a missing API.

There are also extraction gaps. `structured_data_json()` reads JSON-LD;
it does not extract `__NEXT_DATA__`. Network response bodies are delivered to
scripts but are not exposed as a general CMS extraction source. Extraction
currently uses the settled DOM, without selecting between an initial and a
post-execution candidate.

## Folksam: observed behavior

I inspected the public [Folksam homepage](https://www.folksam.se/) and fetched
its raw HTML on 2026-09-17. Findings apply to that response and route; the CMS
vendor, other routes, and authenticated services were not established.

The raw response was 248,583 bytes. It contained a `<main>` with the headline
“Livet kan vara krångligt”, section headings, body copy, links, and images.
It also contained `script#__NEXT_DATA__[type="application/json"]` with
roughly 46,000 characters of JSON. The page data included:

```text
page: /[[...slug]]
gssp: true
locale: sv
props.pageProps.path: /
props.pageProps.pageData.meta
props.pageProps.pageData.data.pageContent
props.pageProps.menuData
```

These are Pages Router delivery markers. The `gssp` flag is consistent with
server-side props. They do not prove which CMS supplies the data.
The separate JSON-LD block described an organization, not the main page body.

The current executable produced this result:

| Check | Observed result |
| --- | --- |
| Normal live `--extract` | Exit 0, but zero blocks, links, media references, and forms; one JSON-LD record. |
| Script diagnostics | `ReferenceError: SVGElement is not defined`, followed by a Next.js client-side exception message. |
| Script budget | `scripts=11 cap_hit=false`; the failure was not a reported timeout. |
| Saved response with executable script elements removed | 20 blocks, 15 links, six media references, and a detected main-content region. |
| Rendering that saved response | Successfully produced a 760 × 1531 PNG with the main headings and prose. |

The static diagnostic retained JSON script elements and inserted a `<base>`
pointing to the original site. It was an offline experiment using the existing
extractor, not an implemented fallback or a full browser comparison. The live
run and raw capture were separate requests.

The observed sequence strongly suggests that failed client initialization
replaces or clears usable server-rendered content before extraction. The
precise mutation was not traced. Fixing `SVGElement` might get initialization
further, but does not establish that the remaining application will work.

The static reader is also incomplete as a useful homepage. Visual inspection
showed headings and prose but omitted product shortcut links and card actions.
The JSON's link count therefore does not prove that the reader presents those
links. Its `article` classification is another reason to check landing-page
behavior rather than relying on article extraction alone.

## Recommended extraction design

Keep several candidates for the same navigation and normalize the selected
content into Distill's existing extraction contract:

```text
Fetched response, final URL, and base URL
    ├── initial HTML extraction
    ├── embedded data → matching content adapter
    └── bounded script execution
          ├── settled DOM extraction
          └── selected JSON responses → matching content adapter
                         ↓
          validate, select, and deduplicate
                         ↓
             extraction JSON → reader
```

**First preserve the initial document.** Retain its bytes or an immutable
snapshot before page scripts execute. Extract it independently of the live
DOM, so a crash or error screen cannot destroy the only copy of the content.
Capture embedded JSON at this point too. The current flow frees the fetched
HTML after loading the DOM; this is a natural place to change ownership.

**Then evaluate whether scripts improve the result.** Compare main-content
coverage, headings, coherent sections, useful links, placeholder/error text,
and route identity. More characters alone is insufficient: a navigation menu
can be larger than the actual page. A title or organization JSON-LD record is
not enough to count as content success.

**Select one primary representation.** Prefer a coherent current-route DOM
when it is complete; use the initial HTML when execution destroys useful
content, and known JSON mappings when they recover missing structure. Merge
only clearly matched missing sections or metadata, preserving source order
and suppressing duplicates. Do not concatenate every string from every source.

**Record the decision.** Proposed diagnostics should identify the selected
source, adapter and version, script errors, budget exhaustion, and why a
fallback was chosen. These are new fields or diagnostics, not existing CLI
features. A partial page should be distinguishable from a complete result.

Candidate selection must retain current access/challenge classifications.
An access-denied or expired-session response should not silently become a
successful content result merely because some embedded text remains.

## Mapping Folksam's embedded data

The most promising data root is:

```text
props.pageProps.pageData.data.pageContent[]
    regionMeta
    regionData[]
        templateName
        templateData.data
```

The captured page had 12 regions, five with component data. A small adapter
could preserve region and component order and translate the observed types:

| Component | Proposed reader representation |
| --- | --- |
| `marketingBanner` | Heading, paragraph, image, and labeled link. |
| `headingParagraph` | Ordered headings and paragraph content. |
| `expandableShortcutButtonCollection` | Section heading and a readable list of labeled destination links. |
| `imageCardCollection` | One group per card, keeping heading, text, image, and action together. |
| `tempSimpleCardCollection` | Ordered text cards with their destination links. |
| `languageSelector` | Language information and explicit destinations where present; otherwise indicate that the interactive selector belongs on the original site. |

Here `templateData.data` is sometimes an object and sometimes an array.
Validate by component type. Respect explicit disabled-region flags such as
`regionMeta.turnOffRegion`; do not render hidden variants merely because they
exist in JSON. Unknown types should produce diagnostics and fall back to DOM
content, without guessing their meaning.

This adapter should read the primary content tree, keeping `menuData` as
separate navigation. Layout settings, analytics configuration, and consent
settings must not become reader paragraphs. Resolve relative destinations
against the source page, preserve descriptive link text and image alt text,
and sanitize any rich HTML through the same reader boundary as other content.

Start with existing block, link, and media fields. If those cannot preserve
the relationship between a card and its action, extend the extraction
contract and reader together with an explicit grouping model. Merely adding
entries to `links` would not fix the omission seen in the rendered diagnostic.

Use `JS_ParseJSON` and C-side mapping in keeping with Distill's C23 production
code policy. QuickJS is already sufficient for parsing these payloads; running
React to reconstruct the same component information is unnecessary here.
DOM profiles remain useful for container selection, but JSON mapping needs
its own explicit schema and versioning.

## Other Next.js and CMS delivery patterns

For Pages Router pages, `__NEXT_DATA__` is a useful detection point, but
`pageProps` has an application-defined schema. Folksam's `pageContent` layout
must not be assumed for other sites. Next.js can also provide JSON during
client navigation; static props have a generated JSON representation.
Use an observed route or a verified adapter instead of hard-coding a build ID
or assuming every page has the same data endpoint. [Next.js: getStaticProps](https://nextjs.org/docs/pages/building-your-application/data-fetching/get-static-props).

App Router pages use server-rendered HTML and a React Server Component payload.
That payload represents a component tree and client component references; it
is not a portable CMS document schema. Prefer HTML first and add payload
decoding only for a demonstrated content gap. Server components execute on
the site's server; our engine does not need to execute their server code.
[Next.js: Server and Client Components](https://nextjs.org/docs/app/getting-started/server-and-client-components).

For content that arrives only through browser requests, add a bounded response
observer near `finish_request()` / `make_response()`. Retain selected JSON
responses with final URL, status, content type, and request/page association.
Apply explicit adapters for the route's content, rather than treating every
JSON response as text to display. Tracking, search suggestions, configuration,
and prefetched other pages are not the requested page body.

Direct CMS APIs can be useful when a published endpoint and its schema are
known. Their payloads may require rich-text decoding, referenced-asset
resolution, locale selection, and pagination. Fetch only what is needed for
the current page. Do not assume that a CMS administration or preview API is
part of the public reading path.

A client-only page whose application cannot run and whose payloads cannot be
mapped remains a compatibility gap. Report it with useful diagnostics and an
original-site link. This proposal fits the small-engine constraint in
[PLAN.md](PLAN.md); it does not require embedding Chromium.

## Suggested implementation order and acceptance checks

1. **Preserve and compare initial HTML.** Add a fixture whose hydration script
   removes useful server-rendered content and throws. Require the original
   title, paragraphs, and links to survive extraction and reader rendering.
   Also test a shell that becomes real content after JS, so initial HTML does
   not incorrectly win every time.
2. **Add embedded-data candidates.** Parse bounded JSON without executing it.
   Test malformed input, excessive nesting, wrong-route data, duplicates, and
   an embedded-data-only page. Cap bytes, node counts, and adapter work as well
   as script execution.
3. **Add a Folksam component adapter and reader support.** Use small synthetic
   fixtures modeled on the observed schema. Require shortcut links and card
   actions to be present in the reader output, in addition to prose. Cover
   arrays, disabled regions, unknown components, and relative URLs.
4. **Add response capture when a target requires it.** Test content fetched by
   JS, unrelated JSON, and route/locale matching. Keep capture transient and
   bounded; do not persist full session response bodies in routine logs.
5. **Improve host APIs from isolated failures.** Reduce the `SVGElement`
   failure to a fixture before changing the host surface. Recheck the live
   site after a fix without assuming the first missing API was the only one.

For implementation changes, validate new fixtures against Chromium where
appropriate, then run both `corpus/check.py --extract` and
`corpus/check.py --render DIR`, as required by the repository guidance.
Live sites are diagnostic evidence, not deterministic gates. For this
documentation task, only the live extraction and saved-page extraction/render
experiments described above were run; no production code was changed.

## Reproducing the investigation

From the repository root, these existing commands capture the response and
exercise the current executable. They require network access and may produce
different results after a site deployment:

```sh
curl -L --max-time 30 -sS -D /tmp/folksam.headers \
  -o /tmp/folksam.html https://www.folksam.se/
./build/distill --url https://www.folksam.se/ --extract \
  > /tmp/folksam-extract.json 2> /tmp/folksam-extract.log
```

The inspected raw response had SHA-256
`5ff538347d06552f122f1896139dc87d6baecd8089b30dfc55f2dd9fad4dd8e6`.
The build ID was `NNWhlj2tPEsms_Dn2Ksoi`; it is evidence about that capture,
not a value to put into an adapter.

For the static diagnostic, make a local copy that removes executable `<script>`
elements, retains `application/json` and `application/ld+json` elements, and
adds `<base href="https://www.folksam.se/">` inside `<head>`. Then run:

```sh
./build/distill --url file:///tmp/folksam-static.html --extract
./build/distill --url file:///tmp/folksam-static.html \
  --render /tmp/folksam-static.png --height 0
```

This is a diagnostic preparation step, not an existing `--no-js` mode. The
recommended first implementation is automatic preservation and selection of
the initial HTML, followed by content adapters where they improve completeness.
