# Journal scan

Checked 16 September 2026. Each URL was captured once with headless Chromium
as the scan baseline and once with Distill using `--dump-dom` and `--extract`.
The percentage is the comparison harness's shared-unique-word coverage between
the browser scan and Distill's information-bearing output. It is a diagnostic
signal, not a quality score; it is not meaningful when one side only contains a
security challenge or an unreachable-page message.

## Results

| # | Journal and tested page | Browser scan | Distill | Main-content handling | Brief note |
|---:|---|---|---|---|---|
| 1 | [Aftonbladet](https://www.aftonbladet.se/nyheter/a/Rr77qd/aftonbladet-direkt?pinnedEntry=1495188) | article/liveblog, 91.1% | article/liveblog | **Profile: `aftonbladet-liveblog`** | Existing profile selected the pinned headline “Ung kvinna häktad – misstänkt grov misshandel” with `profile-headline`. |
| 2 | [AP News](https://apnews.com/article/1437f497b384741c143c97db9803426c) | security-verification page | security challenge | Not available | The direct article retry also returned verification; no article content could be compared. |
| 3 | [The Guardian](https://www.theguardian.com/theguardian/2026/sep/15) | front-page feed, 92.7% | front-page feed | Generic C | Headlines and front-page sections were retained; no site profile needed. |
| 4 | [Euronews](https://www.euronews.com/just-in) | latest-news feed, 88.7% | latest-news feed | Generic C | The live “Latest news” list was extracted; no site profile needed. |
| 5 | [DW](https://amp.dw.com/en/data-centers-face-backlash-from-us-communities/video-79204026) | video article, 51.7% | article text | Generic C | Title and article text were present; the lower overlap is expected from video metadata and related stories. |
| 6 | [SVT Nyheter](https://www.svt.se/nyheter) | news feed, 95.0% | news feed | Generic C | Main headlines, live reports, and sections matched well; no profile needed. |
| 7 | [Expressen](https://www.expressen.se/nyheter/) | news feed, 84.9% | news feed | Generic C | News headlines and short video entries were retained; no profile needed. |
| 8 | [Dagens Nyheter](https://www.dn.se/direkt/2026-09-15/sheeran-inte-mitt-beslut-att-ta-bort-macklemore/) | live report, 92.2% | live report | Generic C (`json-ld-haspart`) | Structured article data identified the live report; no DN-specific profile needed. |
| 9 | [Svenska Dagbladet](https://www.svd.se/) | homepage feed, 69.5% | homepage feed | Generic C | “Just nu” and SvD Direkt headlines were extracted; the navigation and shell account for the lower overlap. |
| 10 | [Omni](https://omni.se/) | news feed, 97.7% | news feed | Generic C | Headline-heavy content matched closely; no profile needed. |
| 11 | [Reuters](https://www.reuters.com/world/) | security-verification page | security challenge | Not available | The site requested bot verification and did not expose news content to either capture. |
| 12 | [Al Jazeera](https://www.aljazeera.com/middle-east/) | section page with headlines | `Loading` shell, 2.0% | Article follow-up profiled | The section still exposes only a loading shell to Distill, but the [direct liveblog article](https://www.aljazeera.com/amp/news/liveblog/2026/9/16/iran-war-live-irans-top-diplomat-heads-to-china-as-war-drags-on) is handled below by `aljazeera-liveblog`. |
| 13 | [France 24](https://www.france24.com/en/) | news feed, 92.1% | news feed | Generic C | Breaking-news headlines and sections were retained; no profile needed. |
| 14 | [NPR](https://www.npr.org/sections/news/) | news feed, 96.0% | news feed | Generic C | National and international headline content matched well; no profile needed. |
| 15 | [CBC News](https://www.cbc.ca/news) | unreachable-page message | partial CBC homepage | Article follow-up profiled | The homepage comparison is not valid, but the [direct article](https://www.cbc.ca/news/world/trump-voting-mail-in-ballots-midterms-9.7344847) exposes a `<main>` article region and is handled below by `cbc-news-article`. |
| 16 | [ABC News Australia](https://www.abc.net.au/news/2026-09-15/south-east-sa-local-news-briefing/107153584) | article, 87.9% | article | Generic C | The regional article title, summary, and body headings were retained; no profile needed. |
| 17 | [NHK World-Japan](https://www3.nhk.or.jp/nhkworld/news/) | unreachable-page message | partial homepage | Article follow-up profiled | The homepage comparison is not valid, but a [direct article](https://www3.nhk.or.jp/nhkworld/en/news/20260905_100/) is extracted and handled below by `nhk-world-article`. |
| 18 | [RNZ](https://www.rnz.co.nz/news) | news feed, 95.8% | news feed | Generic C | Top stories and section content matched well; no profile needed. |
| 19 | [The Hindu](https://www.thehindu.com/) | homepage feed, 65.2% | homepage feed | Generic C | Current India/world headlines were present; the large homepage navigation and layout reduced overlap. |
| 20 | [Dawn](https://www.dawn.com/) | security-verification page | partial homepage | Not available | The [direct article](https://www.dawn.com/news/2029992/oil-climbs-over-2pc) also returned verification in Chromium and a different homepage response in Distill. |

## Article follow-up

The six pages that did not yield a usable main-content comparison above were
retried with direct articles. A profile is useful when the article DOM exists
but the generic matcher deliberately does not promote a full-page article; it
is not useful when the site returns a challenge or the wrong page.

| Journal and article | Browser scan | Distill | Result |
|---|---|---|---|
| [AP News](https://apnews.com/article/1437f497b384741c143c97db9803426c) | security-verification page | security challenge | **Could not get content.** No profile added. |
| [Reuters](https://www.reuters.com/business/aerospace-defense/russia-says-us-acknowledgement-orbital-arms-ignores-space-conflict-consequences-2026-09-15/) | security-verification page | security challenge | **Could not get content.** No profile added. |
| [Al Jazeera](https://www.aljazeera.com/amp/news/liveblog/2026/9/16/iran-war-live-irans-top-diplomat-heads-to-china-as-war-drags-on) | liveblog article | liveblog article, 98.3% | `aljazeera-liveblog` selected the liveblog region; 33 main-content blocks. |
| [CBC News](https://www.cbc.ca/news/world/trump-voting-mail-in-ballots-midterms-9.7344847) | unreachable-page message | article response, 53.6%* | `cbc-news-article` selected the `<main>` article region; 50 main-content blocks. |
| [NHK World-Japan](https://www3.nhk.or.jp/nhkworld/en/news/20260905_100/) | unreachable-page message | article response, 13.0%* | `nhk-world-article` selected the `<main>` article region; 7 main-content blocks. |
| [Dawn](https://www.dawn.com/news/2029992/oil-climbs-over-2pc) | security-verification page | homepage response | **Could not get the article.** No profile added. |

\* The CBC and NHK percentages are not valid fidelity scores because the
browser baseline was an unreachable-page message rather than the article.

## Profile decisions

- Aftonbladet used the existing declarative profile at
  `profiles/aftonbladet-liveblog.json`; it is required to select the pinned
  liveblog entry.
- Al Jazeera, CBC, and NHK World now use declarative profiles for their direct
  article/liveblog pages. These profiles use the generic C matcher with the
  `document-h1` title source; no site-specific rules were added to C.
- Dagens Nyheter was handled by the generic C matcher through JSON-LD article
  parts, and all other successful pages remain on the generic C path.
- AP, Reuters, and Dawn still need access follow-up rather than a profile: the
  direct article URLs returned verification or a different homepage response.
- The `document-h1` title source prefers an h1 inside `<main>`, which avoids
  mistaking a short site masthead such as “News” for NHK's article headline.
