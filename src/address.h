#ifndef DISTILL_ADDRESS_H
#define DISTILL_ADDRESS_H

/* What the reader typed into the address line, turned into somewhere to go.

   Shared by every front end for the same reason the form rules are: "dn.se"
   must navigate and "kingfisher hunting" must search whichever shell it was
   typed into. */

#include <stdbool.h>

/* The search engine the reader is pointed at. DuckDuckGo's HTML endpoint is
   chosen on measured behaviour rather than preference: it is server-rendered,
   needs no JavaScript, and wraps each result in a "result" class, which is the
   signal the extractor's search-results shape keys on. Measured against the
   same query, it produced 27 structured result records where Mojeek, Brave,
   Ecosia, Startpage and SearX produced none, and Marginalia rate-limited the
   request outright. Override with DISTILL_SEARCH_URL; the encoded query is
   appended to whatever it is set to. */
#define DISTILL_DEFAULT_SEARCH_URL "https://html.duckduckgo.com/html/?q="

/* Does this token name a place, or describe one? A scheme, a path, or a
   dotted host is somewhere to go; a bare word or anything containing a space
   is something to look for. */
bool d_address_looks_like_url(const char *text);

/* The URL to fetch for what was typed, or NULL for nothing. When the scheme
   is a guess rather than something the reader wrote, *https_guessed says so:
   the engine is then allowed to correct it, and only then. Caller frees. */
char *d_address_resolve(const char *raw, bool *https_guessed);

/* DuckDuckGo hands out result links as /l/?uddg=<target>; the destination,
   or NULL when this is not such a link. Caller frees. */
char *d_address_unwrap_redirect(const char *uri);

/* %XX decoding; NULL for a malformed escape or an encoded NUL. */
char *d_uri_unescape(const char *text);

#endif
