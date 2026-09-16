# Site profiles

Site profiles are declarative JSON hints for pages whose main content cannot
be located by the generic extractor. The C matcher remains responsible for
loading profiles, matching URLs, selecting containers, matching headings, and
walking the selected content. Adding a site should normally only add a JSON
file here and a corpus fixture.

The profile directory is selected in this order:

1. `DISTILL_PROFILE_DIR`, when set;
2. the source tree's `profiles/` directory in a development build;
3. the installed `${CMAKE_INSTALL_FULL_DATADIR}/distill/profiles` directory.

Example:

```json
{
  "name": "example-liveblog",
  "match": {
    "hosts": ["example.org", "*.example.org"],
    "paths": ["/news/*/live"]
  },
  "main_content": {
    "containers": ["article.update", "li[data-testid=\"entry\"]"],
    "headings": ["h1", "h2", "h3"],
    "title": "og:title"
  }
}
```

`hosts` and `paths` accept exact values or `*` globs. The `containers` and
`headings` lists are alternatives, tried in order. `title` names the metadata
property used to identify the requested entry; `og:title` is the default when
it is omitted.
