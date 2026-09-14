# Live rungs

Five real sites in ascending difficulty. **None of them is a gate.**

PLAN.md's reasoning: curl's TLS/HTTP2 fingerprint plus a missing browser API
surface gets flagged on exactly the JS-heavy sites we are targeting, *without any
JS challenge ever running*. So a live failure is not attributable, and live
results are **recorded, not asserted**. The gates live in `local/`.

Ordering here is evidence, not assertion. The `<p>` counts below were measured
with plain `curl` and a desktop UA on **2026-09-14**; re-measure when they rot.

```sh
curl -sS -L -A "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 ..." "$URL" \
  | grep -c '<p[ >]'
```

---

## 1. danluu.com — static

<https://danluu.com/diseconomies-scale/> · 402 KB · 1 `<script>`

Plain HTML, no JS needed. The reality check on fetch/parse/extract with the
engine entirely out of the picture. If this fails, nothing below it is worth
running.

## 2. Wikipedia — server-rendered, JS enhances

<https://en.wikipedia.org/wiki/Extensible_Markup_Language> · 463 KB · **72 `<p>`** · 5 `<script>`

Large DOM, heavy nav and boilerplate, content fully present without JS. That last
property is what makes it useful: any content *loss* here is an extraction bug,
not a binding one. A clean separation of the two failure modes, on a site that is
permissive to automated clients and unlikely to move.

## 3. quotes.toscrape.com/js — genuinely client-rendered

<https://quotes.toscrape.com/js/> · 5.8 KB · **0 rendered `.quote` divs in raw HTML**

The cleanest true positive available. The data sits in a `var data = [...]` array
and the DOM is built by client script, so nothing readable exists until JS runs.
Purpose-built for scraping practice, so it is friendly by design and stable.

**This is the first rung where Phase 0's whole premise is on trial.** Rungs 1 and
2 pass with no engine at all.

## 4. quotes.toscrape.com/scroll — content over the network

<https://quotes.toscrape.com/scroll> · 2.7 KB

The same content, delivered by AJAX after load. Isolates the network bridge and
the settle policy from rung 3's pure client-side rendering — rung 3 needs the JS
to run, rung 4 needs the JS *and* the fetch bridge *and* a settle policy that
waits for it.

## 5. www.dn.se — diagnostic, not a gate

<https://www.dn.se/ekonomi/lyten-lovar-revansch-men-kunderna-saknas/> · 272 KB · **3 `<p>`**

Measured, not assumed: those three paragraphs are the text-size dialog and a login
prompt. **The article body is not in the HTML at all.** Alongside them, 218
paywall markers and 31 Didomi consent references.

So a failure here has three possible causes:

1. the binding didn't run the page's JS,
2. the consent wall never cleared,
3. the content is behind the paywall and was never going to arrive.

**Being able to say which one fired is the result.** That is exactly the
conflation the corpus design exists to prevent, which is why DN is a diagnostic
rather than a gate — an un-attributable red here would poison the A/B.

Two practical notes:

- The **front page is server-rendered** (teaser headlines are in the HTML) while
  **articles are not**. Pin an article URL; the front page tests something else.
- Expect the URL to rot within days. **Re-pin it rather than debug it** — and
  re-measure the `<p>` count when you do, so the ranking stays evidence.

---

## The gap between 4 and 5

Rung 4 is a 2.7 KB practice page; rung 5 is a 272 KB newspaper behind two walls.
Nothing friendly sits in between, which is deliberate: `local/08-messy.html`
fills that gap on purpose — real-world grot (undeclared encoding, `<base href>`,
hostile schemes, dynamic `import()`, hidden boilerplate) in a fixture that still
names its own cause when it fails.
