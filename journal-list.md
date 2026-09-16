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

## North American sources

These 20 additional North American sources were checked on 16 September 2026
using direct article pages. The percentage is the same diagnostic coverage
measure as above. The profiled articles were rerun with profiles disabled and
their main-content block boundaries compared before removing a profile.

| # | Journal and tested article | Browser scan | Distill | Main-content handling | Brief note |
|---:|---|---|---|---|---|
| 21 | [ABC News](https://abcnews.com/US/helicopter-crashes-los-angeles-site-earlier-fatal-bus/story?id=136481785) | article, 77.6% | article | Generic C | The profile-disabled run selected the same visible article headline and effective region; live navigation text changed between captures. |
| 22 | [CBS News](https://www.cbsnews.com/news/todd-blanche-obama-concerns-targeting-trump-critics/) | article, 80.9% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 23 | [Fox News](https://www.foxnews.com/politics/exclusive-inside-vance-intimate-capitol-hill-huddle-republicans-confront-tough-midterm-terrain) | article, 97.7% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 24 | [USA Today](https://www.usatoday.com/story/shopping/2026/08/21/submit-products-editors-picks-2026-2027/91336627007/) | article, 93.8% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 25 | [NBC Los Angeles](https://www.nbclosangeles.com/news/national-international/trump-slams-supreme-court-mail-ballots/3942302/) | **Timeout** | article | Generic C | Chromium still timed out, but the profile-disabled Distill run selected the same article region; later related-story text changed between captures. |
| 26 | [Washington Post](https://www.washingtonpost.com/the-seven/2026/09/15/7-things-know-tuesday-sept-15/) | **Error page** | **Error** | Not available | Chromium reported an HTTP/2 unreachable-page error and Distill could not load the page; no profile was added. |
| 27 | [The New York Times](https://www.nytimes.com/2026/09/15/us/politics/ai-polls-midterms.html) | challenge shell | **Challenge** | Not available | The page exposed access/challenge content rather than a stable article to Distill. |
| 28 | [Axios](https://www.axios.com/2026/09/15/trump-supreme-court-justices-mail-voting) | challenge shell | **Challenge** | Not available | Access protection prevented a meaningful article comparison; no profile was added. |
| 29 | [ProPublica](https://www.propublica.org/article/propublica-and-the-connecticut-mirror-win-pulitzer-prize-for-local-reporting) | article, 97.8% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 30 | [The Texas Tribune](https://www.texastribune.org/2026/09/04/texas-ken-paxton-personal-financial-disclosures-ethics-law-us-senate-race/) | article, 96.0% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 31 | [PBS NewsHour](https://www.pbs.org/video/trump-agenda-1784238182/) | video article, 85.0% | video article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 32 | [Los Angeles Times](https://www.latimes.com/politics/story/2026-08-30/midterm-election-senate-control-alaska-iowa-maine-michigan-ohio-texas) | **Access denied** | article | Not found; no safe profile | Chromium received an access-denied page, while Distill received article text whose headline sat outside the article/main candidates; no safe profile was added. |
| 33 | [Minnesota Star Tribune](https://www.startribune.com/minnesota-star-tribune-cuts-jobs-and-pursues-nonprofit-ownership-structure/601852356) | article | **Security checkpoint** | Not available | Distill received a Vercel security checkpoint instead of the article; no profile was added. |
| 34 | [The Boston Globe](https://www.bostonglobe.com/2026/09/14/business/boston-housing-supply/) | article, 4.3% | video shell | Not available | The response exposed a video/article shell without a usable h1 region; no profile improved it. |
| 35 | [Global News](https://globalnews.ca/news/11894575/canso-spaceport-project-criticism/) | article, 67.9% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 36 | [CityNews Toronto](https://toronto.citynews.ca/2026/07/27/toronto-police-us-consulate-shots-fired-suspect/) | article, 86.2% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 37 | [CTV News](https://www.ctvnews.ca/ottawa/article/how-athletes-featured-on-ctv-news-ottawa-performed-in-sundays-ironman/) | article, 1.6% | video shell | Not found; no safe profile | A confirmed direct article URL loaded, but Distill received a video/article shell without a matching headline region; no profile improved it. |
| 38 | [The Daily Beast](https://www.thedailybeast.com/trump-humiliated-as-poll-reveals-brutal-republican-revolt/) | article, 2.2% | video shell | Not found; no safe profile | Distill's DOM did not retain a matching h1/article region; the low overlap is not a matcher score. |
| 39 | [KTVZ](https://ktvz.com/politics/cnn-us-politics/2026/09/15/whats-next-for-mail-voting-after-the-supreme-court-ruled-against-trump/) | article, 96.7% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |
| 40 | [Voice of America](https://www.voanews.com/a/trump-vows-accountability-for-those-who-pursued-him-in-court-cases-/8011522.html) | article, 95.5% | article | Generic C | The profile-disabled run produced the same headline and article block sequence as the profiled run. |

The North American profiles tested here were removed after the generic C
matcher reproduced their article boundaries. No North American site-specific
condition was added to the C matcher. The failed or challenge cases remain
recorded so they can be retried later when the publisher exposes the article
response.

## European sources

These 20 additional European sources were checked on 16 September 2026 using
direct article pages. The percentage is the same diagnostic coverage measure
as above. The first pass used declarative profiles as probes; each accessible
article was then rerun with profiles disabled. Profiles were removed when the
generic matcher produced the same article-headline selection. Challenge,
empty-render, and timeout results are recorded as such.

| # | Journal and tested article | Browser scan | Distill | Main-content handling | Brief note |
|---:|---|---|---|---|---|
| 41 | [BBC News](https://www.bbc.com/news/articles/c62ezg7gkzwo) | article, 94.0% | article | Generic C | With profiles disabled, the visible h1 inside `<article>` was selected as `article-headline`. |
| 42 | [Sky News](https://news.sky.com/story/starmer-latest-burnham-streeting-rayner-leadership-quit-cabinet-resign-labour-local-elections-12593360?postid=10392375) | access-denied shell | access-denied shell | Not available | Both captures were denied by the publisher before article content was exposed; no profile was added. |
| 43 | [Financial Times](https://www.ft.com/content/2b780776-d8f3-4bd3-9daa-f601b5d4206a) | security-verification page | security challenge | Not available | The direct article returned verification rather than content; no profile was added. |
| 44 | [The Independent](https://www.independent.co.uk/news/uk/home-news/dwp-benefits-pension-payment-september-cost-of-living-b2807701.html) | article, 64.1% | article | Generic C | With profiles disabled, the matching visible h1 in the article region was selected as `article-headline`. |
| 45 | [The Irish Times](https://www.irishtimes.com/ireland/2026/09/15/man-killed-30s-and-woman-injured-after-m4-collision-in-co-meath/) | article, 2.6% | title-only article shell | Not found; no safe profile | Chromium saw the article, but Distill retained only the title/meta shell and no usable h1, main, or article region. |
| 46 | [POLITICO Europe](https://www.politico.eu/article/protests-georgia-poll-confirms-russia-meddling-irregularities-denounced-election-results/) | article, 95.6% | article | Generic C | With profiles disabled, the matching visible h1 in the article region was selected as `article-headline`. |
| 47 | [Le Monde](https://www.lemonde.fr/en/politics/article/2026/09/15/oil-prices-and-rising-interest-rates-push-french-public-finances-further-into-red_6757548_5.html) | bot challenge | bot page | Not available | Chromium and Distill received access-protection responses, not the article. |
| 48 | [Le Figaro](https://www.lefigaro.fr/international/guerre-au-moyen-orient-la-chine-prete-a-defendre-les-interets-legitimes-de-l-iran-20260916) | video article, 23.3% | video article | Generic C | With profiles disabled, the matching visible h1 was selected as `article-headline`, including this short video page. |
| 49 | [RFI](https://www.rfi.fr/en/podcasts/the-sound-kitchen/20240824-promises-promises-the-sound-kitchen-20240824) | article, 97.1% | article | Generic C | With profiles disabled, the matching visible h1 in the article region was selected as `article-headline`. |
| 50 | [El País](https://elpais.com/mexico/2026-08-31/mexico-lleva-ocho-periodistas-asesinados-en-lo-que-va-de-2026.html) | bot/redirect shell | security challenge | **Profile retained: `elpais-article`** | The profile is retained because the current no-profile run was blocked before article content was exposed; its requirement could not be disproven. |
| 51 | [El Mundo](https://www.elmundo.es/internacional/2026/09/16/6aaa5b84fc6c837f578b4585.html) | article, 61.1% | article | Generic C | With profiles disabled, the matching visible h1 selected the primary article as `article-headline`. |
| 52 | [Corriere della Sera](https://www.corriere.it/cronache/26_agosto_30/barbara-palombelli-intervista-1ed4f729-3f84-4da8-97e8-a98b4a5d9xlk.shtml) | article, 79.1% | article | Generic C | With profiles disabled, the matching visible h1 in the article region was selected as `article-headline`. |
| 53 | [la Repubblica](https://www.repubblica.it/esteri/2026/07/01/diretta/guerra_ucraina_russia_news_oggi-425443554/) | live article, 92.7% | live article | Generic C | With profiles disabled, the matching visible h1 was selected as `article-headline`. |
| 54 | [Frankfurter Allgemeine Zeitung](https://www.faz.net/aktuell/politik/ausland/kriegsverbrechen-in-kosovo-thaci-zu-25-jahren-verurteilt-accg-201229090.html) | article, 59.0% | article | Generic C | With profiles disabled, the matching visible h1 in the neutral `content-wrapper` region was selected as `article-headline`. |
| 55 | [Süddeutsche Zeitung](https://www.sueddeutsche.de/projekte/artikel/muenchen/muenchen-bayern-sz-lokalkjournalismus-reportagen-2026-e152013/) | article, 5.3% | extraction timeout | **Profile attempted: `sueddeutsche-article`** | The direct project/article DOM loaded, but Distill timed out during extraction even with an extended budget; profile improvement is unverified. |
| 56 | [Neue Zürcher Zeitung](https://www.nzz.ch/schweiz/daenemark-gilt-als-musterland-fuer-eine-asylpolitik-ohne-kompromisse-was-beat-jans-von-mette-frederiksen-lernen-kann-und-was-nicht-ld.10023403) | article, 87.7% | article | Generic C | With profiles disabled, Unicode-space normalization let the matching visible h1 select the article as `article-headline`. |
| 57 | [De Standaard](https://www.standaard.be/media-en-cultuur/via-deze-knop-krijgt-u-voortaan-meer-de-standaard-nieuws-te-zien-op-google/151706718.html) | Cloudflare page | security challenge | Not available | Both captures were blocked before the article could be compared; no profile was added. |
| 58 | [NOS](https://nos.nl/artikel/2631034-invloedrijke-senator-mcconnell-na-gezondheidsproblemen-terug-in-congres) | article, 88.2% | article | Generic C | With profiles disabled, the matching visible h1 in `main` was selected as `article-headline`. |
| 59 | [Aftenposten](https://www.aftenposten.no/verden/i/JOqj67/ukraina-angrep-russisk-oljeraffineri-og-militaerbase) | article, 59.7% | article | Generic C | With profiles disabled, the matching visible h1 in `<article>` was selected as `article-headline`. |
| 60 | [Público](https://www.publico.pt/2026/09/15/local/noticia/lisboa-emel-lanca-nova-aplicacao-bicicletas-partilhadas-gira-2188107) | 403 page | security challenge | Not available | The direct article returned a 403/challenge response to the captures; no profile was added. |

The remaining European profiles are declarative JSON files in `profiles/`;
the generic matcher now recognizes a matching visible headline within common
semantic article regions, including ARIA roles, `articleBody`, and a neutral
`content-wrapper` marker, before consulting low-content JSON-LD paywall
selectors. No European site-specific condition was added to C.
