# Rendering dn.se: why the page is 20,000 pixels tall

Date: 2026-09-15

A note on three questions raised by the dn.se front page in the desktop
shell: why the whole page cannot be rendered, whether there is a cheaper way
to do it, and why every link is the size of a headline.

The short version is that the second and third questions have the same
answer. The page is enormous *because* every headline is a link rendered at
article-heading size, and fixing the typography removes most of the problem
that the rendering strategy is trying to solve. They are worth fixing
separately anyway, because each is wrong on its own terms.

## What dn.se actually is

The front page is not an article. Extraction reports:

| Blocks | Count | Of which are links |
| --- | ---: | ---: |
| `h2` | 132 | 117 |
| `h3` | 37 | 37 |
| `p` | 59 | 51 |
| `li` | 31 | 29 |
| `h1` | 1 | 0 |
| **Total** | **260** | **234** |

Ninety percent of the page is links, and two thirds of those links are
headings. That is what a news front page is: an index. The reader is
currently treating it as a document.

## Why the links are so large

They are large because they are headings, and the reader styles headings for
an article — where an `h2` appears every few paragraphs and is meant to
interrupt the text.

```
body { font-size: 18px }
h2   { font-size: 1.45em; line-height: 1.2; margin: 1.7em 0 .55em }
```

Worked through at the reader's own base size:

- `h2` text is 26.1px, so one line occupies 31px;
- its margins are `1.7em + 0.55em` of 26.1px, which is **59px** — nearly
  twice the height of the text itself;
- a one-line headline therefore costs about **90px**, a two-line one about
  **121px**.

There are 132 of them. Headlines alone account for roughly **11,900px**, and
the `h3`, `p` and `li` blocks take the total past 20,000.

So the answer to "are they headlines with a link?" is yes, exactly that: the
source marks each headline as `<h2><a href=…>`, extraction now preserves the
anchor, and the reader renders the anchor inside a heading that was sized for
article typography. The link is large because the heading is large.

**This is a styling bug, not a link bug.** An index wants a dense list: one
line per item, a modest size, tight leading. The reader has one stylesheet and
applies it to every shape it can produce, so a page that is 90% links gets the
spacing rules written for a page that is 90% prose.

The fix is a shape-aware rule rather than a different renderer: when the
selected extractor is the block/link-index path rather than `readability`,
headings in the stream should be set at something near body size with normal
leading. At roughly 34px per item instead of 90, dn.se would come out near
**7,000px** — which the current whole-page renderer handles without
difficulty. Most of the height problem is self-inflicted.

## Rendering the whole page

Today the shell asks the engine for a single bitmap as tall as the laid-out
document, bounded at 20,000px. dn.se hits that bound, so its tail is still
cut, and the cost of getting that far is real:

| Surface | Bytes in memory | PNG written | litehtml time |
| --- | ---: | ---: | ---: |
| 760 × 1,100 | 3.2 MB | 103 KB | 36 ms |
| 760 × 20,000 | **58.0 MB** | **2.14 MB** | **327 ms** |

(Measured against the same saved reader template, timing litehtml only, so
network, JavaScript and extraction are excluded from both rows.)

Every navigation therefore allocates 58MB, paints all of it, compresses it to
a 2.14MB PNG, writes that to disk, and the shell reads it back — in order to
display about 1,100px of it. Scrolling shows pixels that were painted once and
kept; nothing is recomputed, but nothing is saved either.

Raising the cap does not fix this, it just moves it. The surface is
`width × height × 4` bytes, so a 40,000px page would want 116MB.

### The cheaper way: draw a band, not the document

litehtml separates layout from painting. `document->render(width)` computes
the layout once and `document->draw(hdc, x, y, clip)` paints whatever part of
it you ask for. Nothing requires the surface to be as tall as the document.

I prototyped this to check it actually works rather than assuming it:

```c
document->render(width, litehtml::render_all);
litehtml::position clip(0, 0, width, viewport);
document->draw(renderer, 0, -scroll_y, &clip);
```

Rendering dn.se at `scroll_y = 14000` into an 1,100px surface produces exactly
the band of the document at that offset, correctly laid out. Measured against
the whole-page render of the same template:

| Approach | Memory | PNG | Time |
| --- | ---: | ---: | ---: |
| Whole page, 20,000px | 58.0 MB | 2.14 MB | 327 ms |
| One 1,100px band at offset 14,000 | **3.2 MB** | **97 KB** | **28 ms** |

**Roughly twelve times faster and eighteen times smaller — and that figure
still includes a complete re-parse and re-layout on every band**, because the
prototype throws the document away between calls. Holding the laid-out
document and painting from it would reduce the per-band cost to the paint
alone.

It also removes the height cap as a concept. A band at offset 60,000 costs
exactly what a band at offset 0 costs, so page length stops being bounded by
what fits in one allocation.

### What it would take

The shell is already in a position to do this. `distill-gtk` links the
litehtml adapter directly — it calls `distill_litehtml_form_boxes` and
`distill_litehtml_link_boxes` on the reader template today — so it does not
need to respawn the engine to repaint. The engine writes the template to disk
already.

1. Export a region renderer: `distill_litehtml_render_region(html, out, width,
   viewport, scroll_y, font_dir)`. The prototype above is the whole of it.
2. Have the shell paint the visible band plus a screen of overscan on either
   side, on the scroll adjustment's `value-changed`.
3. Keep the document laid out between bands rather than rebuilding it, so a
   scroll costs a paint rather than a parse. This is the only part that needs
   real design, because it means holding a `litehtml::document` across calls
   and therefore giving the adapter a handle type.
4. Ask the engine for the content height once (`distill_litehtml_content_height`,
   which already exists) to size the scroll range.

Steps 1 and 2 alone are worth having and are small; step 3 is the one that
makes scrolling feel free.

### Which to do first

Fix the typography first. It is a smaller change, it improves the page for a
reader regardless of how it is painted, and it removes roughly two thirds of
the height that the rendering work exists to cope with. Then do the band
renderer, which stops page length from being a constraint at all.

## Do the links lead anywhere?

Yes. The extracted hrefs are absolute and correct, and following them works:

| Link | Result |
| --- | --- |
| `dn.se/direkt/2026-09-15/barnmusik-undantas-…` | loads, title `DN Direkt – Barnmusik undantas…` |
| `dn.se/om/val-2026/` | loads, title `Valet 2026` |
| `dn.se/direkt/2026-09-15/gellerbrant-ska-tillbaka…` | loads, title `DN Direkt – Gellerbrant ska…` |

The navigation is sound and the destination is the right one. But every one of
them extracts **0 characters of body text**: the reader arrives, shows the
correct headline, and has nothing beneath it.

That is the Didomi consent wall and paywall that
[`SITE_ACCESS_REPORT.md`](SITE_ACCESS_REPORT.md) has tracked from the
beginning, and it is a separate problem from either of the two above. Worth
being precise about the distinction:

- the links are **not** broken — they resolve, and the title proves the right
  page was fetched;
- the article body is absent from the served HTML, behind consent and payment;
- so dn.se article pages will stay empty until a session carries a consent
  choice, which is what `--session-file` now makes possible but has not yet
  been tried against DN.

That last point is the cheapest thing on this page to test next: accept DN's
consent dialogue once in a real browser, export the cookie jar, and see
whether an article body appears. It would also be the first real use of the
bring-your-own-session route (M5b in
[`SITE_ACCESS_REMEDIATION.md`](SITE_ACCESS_REMEDIATION.md)).

## Summary

| Problem | Cause | Fix | Size |
| --- | --- | --- | --- |
| Links look enormous | Index headlines styled with article heading rules — 59px of margin on a 31px line | Shape-aware typography for the block/link-index path | Small |
| Page is 20,000px | Same cause; 132 headings at ~90px each | Same fix removes ~two thirds of it | Small |
| Tail is still cut | One bitmap per page, bounded at 20,000px | Band rendering at a scroll offset; prototype confirmed, 12× faster and 18× smaller | Medium |
| Articles are blank | Consent wall and paywall, not a rendering fault | Try a user-supplied session jar | Unknown |

## Reproducing the measurements

```sh
cmake --build build -j2
# The reader template, saved once so timings exclude network and JavaScript:
./build/distill --url https://dn.se --template /tmp/dn.html \
                --render /tmp/dn.png --width 760 --height 1400
# Block and link counts:
./build/distill --url https://dn.se --extract | python3 -c \
  "import json,sys,collections; b=json.load(sys.stdin)['blocks']; \
   print(collections.Counter(x['type'] for x in b)); \
   print(sum(1 for x in b if x.get('href')), 'linked')"
# Full-page height:
./build/distill --url https://dn.se --render /tmp/dn.png --width 760 --height 0
```
