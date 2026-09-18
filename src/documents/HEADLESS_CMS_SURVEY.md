# Headless CMS survey: public examples and extraction paths

Inspected 2026-09-17, using Distill revision `b367725`. Companion to
[the extraction proposal](HEADLESS_CMS.md). Measurements and response hashes
are in [HEADLESS_CMS_SURVEY.json](HEADLESS_CMS_SURVEY.json).

**All eight selected examples delivered useful content in their initial HTML.
Five also exposed readily identifiable page data through ordinary JSON or
Nuxt's serialized state.** Distill should preserve that HTML, then recover
additional structure from data where it improves the reader. Successful
framework hydration is not a prerequisite for extracting these pages.

## Selection and evidence

This is a practical shortlist of prominent headless CMS platforms: Contentful,
Sanity, Storyblok, Strapi, Prismic, DatoCMS, Directus, and Payload. It spans
hosted content services and self-hostable systems. It is not an ordered
market-share ranking: a CMS used only during server rendering or a build can
leave no reliable public fingerprint. Review rankings, developer surveys,
and counts of detectable websites measure different things.

For context, Storyblok's sponsored [2024 CMS survey](https://s3.amazonaws.com/a.storyblok.com/f/88751/x/b875061a67/state-of-cms-2024_global_a4.pdf)
reports usage of several of these products, but its sample is not a census
of headless websites. This investigation prioritizes concrete extraction
evidence over assigning popularity percentages.

Each example below has a vendor-hosted project attribution or an official
demo repository. I also fetched the current page, inspected its HTML and
embedded data, and ran the existing Distill extractor. Historical project
attributions and today's frontend observations are recorded separately.

| CMS | Public example inspected | CMS attribution | Current delivery observed |
| --- | --- | --- | --- |
| Contentful | [Bang & Olufsen, US homepage](https://www.bang-olufsen.com/en/us) | [Contentful customer story](https://www.contentful.com/case-studies/bang-and-olufsen/) | Next.js Pages Router; HTML and `__NEXT_DATA__` with Contentful entry metadata. |
| Sanity | [Figma blog](https://www.figma.com/blog/) | [Sanity project listing](https://www.sanity.io/projects/figma) | Next.js App Router markers, HTML, Sanity assets and Portable Text structures in streamed data. The listing describes an earlier Gatsby frontend. |
| Storyblok | [Oatly, UK homepage](https://www.oatly.com/en-gb) | [Storyblok customer story](https://www.storyblok.com/cs/oatly) | Next.js Pages Router; HTML and a structured Storyblok story in `__NEXT_DATA__`. |
| Strapi | [Moustache Bikes, UK homepage](https://moustachebikes.com/en-UK) | [Strapi customer story](https://strapi.io/user-stories/moustache-bikes-replaced-wordPress-with-strapi) | HTML and Next.js App Router/Turbopack markers. CMS attribution comes from the case study; no raw Strapi document was identified. |
| Prismic | [Salon Zuiver](https://salonzuiver.be/) | [Prismic partner project](https://prismic.io/hire-a-developer/diemas) | Next.js Pages Router; HTML and Prismic document/slices in `__NEXT_DATA__`. |
| DatoCMS | [foryouandyourcustomers](https://foryouandyourcustomers.com/) | [DatoCMS project listing](https://www.datocms.com/partners/foryouandyourcustomers/showcase/foryouandyourcustomers) | Gatsby HTML and a public page-data JSON response with DatoCMS fields. |
| Directus | [Agency OS demo](https://www.agencyos.dev/) | [Official Directus/Nuxt project](https://github.com/directus-labs/agency-os) | Nuxt SSR HTML and `__NUXT_DATA__` containing the page and expanded block relations. |
| Payload | [Paper Triangles](https://www.papertriangles.com/) | [Payload customer story](https://payloadcms.com/case-studies/paper-triangles) | Next.js App Router markers, HTML, and `blockType` records in streamed data. |

A CMS and its frontend framework are separate adapter dimensions. The sample
contains three ordinary Next.js JSON payloads, three App Router streams, one
Gatsby data response, and one Nuxt payload. Those delivery formats should be
handled independently from the CMS's rich-text and component formats.

## What the current executable extracts

“Static” means a saved response with executable script elements removed,
JSON script elements retained, and the original base URL supplied. “Live”
means normal Distill navigation with scripts enabled. These were separate
requests, using default engine limits, up to three concurrent diagnostic
jobs, and a 35-second external process timeout.

| Example | Static blocks / links | Live blocks / links | Observed live behavior |
| --- | ---: | ---: | --- |
| Bang & Olufsen | 68 / 51 | No JSON result | Exit 2; interruption during script discovery, with missing `Document` and `Image` errors also logged. |
| Figma blog | 190 / 158 | 190 / 158 | Missing `TextEncoder`; script cap hit; HTML content survived. |
| Oatly | 36 / 40 | 0 / 0 | React errors and an observer-related `style` error; client exception; exit 0 despite empty content. |
| Moustache Bikes | 237 / 178 | 237 / 178 | Turbopack-related `getAttribute` errors; HTML content survived. |
| Salon Zuiver | 48 / 7 | 48 / 7 | Client-side `TypeError`; HTML content survived. |
| foryouandyourcustomers | 182 / 171 | 182 / 171 | Readability parser diagnostics; content still extracted. |
| Agency OS | 90 / 19 | 90 / 19 | No script errors recorded in this run. |
| Paper Triangles | 6 / 23 | 6 / 23 | Missing `TextEncoder`; HTML content survived. |

Counts measure extraction structure, including possible navigation or repeated
content. They are not completeness scores or counts of rendered reader links.
No Chromium comparison or visual reader review was performed for these eight
samples. Bang & Olufsen's static run also logged Readability parsing errors;
its block extractor nevertheless returned content. DatoCMS and Payload samples
were classified as `video`, which deserves reader-level validation.

This extends the Folksam finding: preserve useful content even when scripts
fail, and do not treat exit 0 as proof of a usable page. The six equal-count
results also do not prove the scripts added nothing semantically.

## Contentful: extract the page's resolved content graph

The Bang & Olufsen capture contains about 431,000 characters of Next.js JSON.
The useful observed path is:

```text
__NEXT_DATA__.props.pageProps.initialState.homePage.data
    title
    seo
    sections[]
```

There are six sections, distinguished by `cardType`, including `herov4`,
`guidanceV2`, and text/image sections. Nested items retain `sys` entry IDs,
content-type IDs, locale, and environment metadata. Images use Contentful's
asset CDN. This is an application-transformed model, not an untouched API
response.

Extract HTML first. A B&O adapter could map these ordered sections to headings,
text, images, and product/editorial links while honoring country selection
and exclusion rules. Keep global menus, cart state, and newsletter controls
out of the page body.

For other Contentful sites, support the standard Rich Text tree:
`nodeType`, `content`, text `value`, and `marks`. Embedded entry/asset links
need resolution through the response's `includes.Entry` and `includes.Asset`
or already-resolved objects. The delivery API requires an access token;
an asset URL alone does not provide access to the content API. The captured
page already contains enough data for an adapter without requesting one.
[Rich Text format](https://www.contentful.com/developers/docs/concepts/rich-text/),
[delivery API authentication](https://www.contentful.com/developers/docs/references/content-delivery-api/overview/).

## Sanity: HTML first, Portable Text when actually exposed

Figma's blog capture contains substantial HTML plus `self.__next_f` stream
records. The stream includes Sanity-style `_type: "block"` and span structures
with `children`, `text`, `marks`, `markDefs`, and `style`; images point to
`cdn.sanity.io`. There is no `__NEXT_DATA__` object to parse on this page.

The immediate extraction path is the HTML blog index: preserve article titles,
summaries, images, and destination links as cards. Follow an article URL to
extract its body; do not mistake index excerpts for complete articles.

When a page exposes Portable Text as ordinary JSON, a shared decoder can map
block styles to headings/paragraphs, spans to formatted text, annotations to
links, and list metadata to ordered groups. Custom blocks and references need
schema-specific handling. This format is designed for serialization into
different presentations. [Sanity Portable Text documentation](https://www.sanity.io/docs/developer-guides/presenting-block-text).

For this example, obtaining standalone Portable Text from the stream would
require a separate RSC transport decoder. A text match for `markDefs` is
evidence of data, not a safe extraction algorithm. No Sanity content API query
or complete RSC decoding was performed.

## Storyblok: a strong embedded-data recovery example

Oatly supplies about 203,000 characters of Next.js JSON. The exact body path is:

```text
__NEXT_DATA__.props.pageProps.story.content.blocks[]
```

The story also provides `full_slug`, identity, and publication metadata. This
capture has 28 `browser-page-card` components. Their `text` fields contain rich
text with nested `blok` nodes; `attrs.body[]` can itself hold components such
as `layout-page-heading`. Much of the homepage is arranged as a graphical
canvas, so the structured content is especially useful for a linear reader.

A mapper should walk known components recursively, preserve card/link
relationships, and turn image-backed labels into readable labels when the
schema provides them. Handle nested rich-text components, asset descriptions,
and multilinks with `linktype` and `cached_url`. Keep market navigation and
translation strings separate from the story body. Validate which cards belong
in the initial reading order rather than displaying all cached content.

The live zero-block result makes this a good regression case alongside Folksam.
A Storyblok API request is unnecessary for the captured story; direct delivery
requests require a configured token and appropriate published-content selection.
[Storyblok delivery API](https://www.storyblok.com/docs/api/content-delivery/v2/stories/retrieve-a-single-story),
[Storyblok rendering SDK](https://www.storyblok.com/docs/libraries/js/js-sdk).

## Strapi: the backend may be completely hidden by the frontend

Moustache Bikes' capture contains HTML, `self.__next_f` records, and Turbopack
script URLs. I did not identify a raw Strapi response or a verified public
content endpoint in this page. The vendor story establishes Strapi use but
describes an earlier frontend arrangement; the current response establishes
the Next.js delivery behavior.

Extract product ranges, editorial cards, links, and product images from HTML.
If a later route fetches Strapi JSON in the browser, capture that particular
response and map its documented schema. Do not assume that `/api/articles`
on the public website is the CMS endpoint.

A reusable Strapi decoder needs version handling: v4 commonly nests fields
under `data.attributes`; v5 flattens fields into `data` and uses `documentId`.
Relations, media, components, and dynamic zones are not populated by default.
A shallow response can therefore omit most of a page's content. Use explicit
field/population rules for known models, with locale and publication-state
matching. [Strapi REST documentation](https://docs.strapi.io/cms/api/rest).

## Prismic: slices map naturally to reader sections

Salon Zuiver is the cleanest ordinary-JSON example in the sample:

```text
__NEXT_DATA__.props.pageProps.doc
    type: dynamic-page
    lang: nl-be
    data.slices[]
```

The payload is about 14,000 characters. Its four slices are `hero`, `hero`,
`information`, and `hero`. Each has `slice_type`, `variation`, `primary`, and
`items`; fields include rich-text arrays with `type`, `text`, and `spans`.
Navigation and footer are separate page props.

Map each slice in order. Convert `primary.title` into a heading, description
fields into paragraphs, image fields into media, and repeated items into
lists/cards. Preserve inline spans and link destinations. The captured
document's `url` is null, so retain the fetched page URL rather than inventing
a canonical URL from a UID.

For direct queries, Prismic uses repository refs to select a content version;
private repositories require a token. The captured document avoids both
endpoint discovery and another request.
[Prismic Rich Text](https://prismic.io/docs/fields/rich-text),
[Prismic client API and refs](https://prismic.io/docs/technical-reference/prismicio-client/v6).

## DatoCMS: use Gatsby's public page data

The foryouandyourcustomers homepage has Gatsby markers and DatoCMS image URLs.
The following public request returned HTTP 200 and JSON during inspection:

```text
https://foryouandyourcustomers.com/page-data/index/page-data.json
```

Its useful content root is:

```text
result.data.datoCmsHomepage.sections[]
```

There are six sections, identified through `internal.type`. A mosaic card has
`active`, `headline`, `description.value`, and a linked `content` record with
`pagePath` and image information. `description.value` contains
`schema: "dast"` and a document tree. This is a verified data source for this
route, not a universal CMS endpoint convention.

Map the ordered sections; render active cards, decode their DAST descriptions,
preserve `content.pagePath`, and take actual image URLs from Gatsby image data
rather than the tiny placeholder image. Other top-level query results contain
employees, locations, and services; only use them where the page references
them. `result.pageContext` carries locale information.

DAST is a structured tree with headings, paragraphs, spans, lists, links,
and embedded records. A reusable decoder can support the standard nodes;
embedded records still need matching content data.
[DatoCMS DAST specification](https://www.datocms.com/docs/structured-text/dast).

## Directus: decode Nuxt state before mapping relational blocks

Agency OS is an official demonstration application with a published source
repository. Its captured HTML includes about 29,000 characters in
`script#__NUXT_DATA__`. After resolving the serialized references, the
homepage is at the logical path:

```text
payload.data["/"]
    permalink: /
    status: published
    blocks[]
```

There are 15 block relations. They have `collection`, `hide_block`, and `item`;
observed types include `block_quote`, `block_form`, `block_hero`, `block_faqs`,
and `block_gallery`. Their related item data is already present.

Nuxt uses `devalue` for payload serialization. This capture is a reference
table, with wrappers such as `ShallowReactive`; numbers inside containers can
refer to other array entries. `JSON.parse()` alone does not produce the
logical page object. Implement a bounded decoder with supported type tags,
reference validation, and cycle handling. Do not treat reference indices as
text or execute payload strings. [Nuxt data serialization](https://nuxt.com/docs/4.x/getting-started/data-fetching).

Then map visible block relations in order and sanitize rich HTML fields.
Directus has application-defined item schemas, and nested relations require
explicit field selection when fetching from its API. There is no universal
Directus article body property. [Directus items API](https://docs.directus.io/reference/items).

## Payload: block data exists, but its transport matters

Paper Triangles returns usable headings, project links, and media in HTML.
Its Next.js stream includes `blockType`, `blockName`, image records, and media
paths. An observed block type is `image-row-block`.

The records contain stream references such as `$34` and can cross script
chunk boundaries. Each `self.__next_f.push(...)` is not an independently
complete CMS JSON document. Use the HTML for the first implementation. A
future RSC adapter must reassemble and resolve transport records before a
Payload block mapper can use them; flattening every string would mix current
content, component machinery, and potentially unrelated records.

Payload can expose collection REST endpoints, normally under `/api`, with
depth controlling relationship/upload population. The route prefix and
collection names are configurable. No collection endpoint was verified on
this example. Payload's current rich-text editor supports Lexical; older or
custom installations can differ, so do not assume a Lexical tree merely from
the CMS name. [Payload REST API](https://payloadcms.com/docs/rest-api/overview),
[rich-text documentation](https://payloadcms.com/docs/rich-text/overview).

## Samples that were replaced

These checks explain why detection should use current evidence:

- [Ski.com](https://www.ski.com/), a [Prismic customer](https://prismic.io/case-studies/ski-com),
  returned HTTP 429 and a Vercel Security Checkpoint. This is an access result,
  not evidence of missing CMS extraction support.
- [Bonjoro](https://www.bonjoro.com/), listed by a
  [Prismic partner](https://prismic.io/hire-a-developer/jake-fulton), returned
  Next.js props dominated by `plasmicData` and `plasmicQueryCache`. No Prismic
  document was identified, so it was not used as the Prismic schema example.
- The [Twin Promotion project](https://directus.com/built-with-directus/twinpromotion)
  links to `www.twinpromotion.fr`, which failed DNS resolution here. Agency OS
  provided an accessible Directus example.

## What to build first

1. **Initial-HTML preservation and fallback selection.** This benefits every
   sampled CMS and directly addresses the destructive outcomes on Folksam
   and Oatly. It must also cover early pipeline failures such as B&O's exit 2.
2. **Ordinary JSON discovery and schema adapters.** Start with Prismic slices,
   Storyblok components, and the Contentful page model. Parse data directly
   using QuickJS/C without booting the frontend application.
3. **Gatsby page-data discovery.** Resolve the current route's data location,
   validate that it is JSON for that route, then support DAST and referenced
   content. Do not recursively fetch the site's entire graph.
4. **Nuxt payload decoding.** Add reference-table/type handling before the
   Directus component mapper; support externally referenced payloads when a
   page explicitly declares them.
5. **RSC only for a demonstrated content gap.** Three sampled pages use this
   transport and already expose useful HTML. Keep transport decoding separate
   from Sanity/Payload mappings, and never interpret the whole stream as the
   current page body without route and component selection.

Across adapters, keep title, section order, locale, images, and useful actions
together in the reader contract. Test missing references, hidden blocks,
duplicate navigation, wrong-route data, and malformed payloads. A restored
paragraph with its action link omitted is still incomplete extraction.

The JavaScript engine is sufficient for the data processing these adapters
need. The main work is delivery-format decoding, content selection, and
mapping structure into the reader—not adding a CMS SDK or implementing every
browser API. This survey adds documentation and measurements only; no CMS
adapter or runtime change has been implemented.
