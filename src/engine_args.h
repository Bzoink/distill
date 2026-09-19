#ifndef DISTILL_ENGINE_ARGS_H
#define DISTILL_ENGINE_ARGS_H

/* The engine's options, from a command line or a request file.

   A request file holds the same options, one per line: the long name
   without its dashes, then its value, if it takes one, to the end of the
   line. Blank lines and lines starting with '#' are ignored.

       url https://example.com/search?q=a+long+query
       template /home/me/.cache/distill/page.html
       request-method POST
       request-data-file /home/me/.cache/distill/body

   `--request FILE` reads one, or stdin when FILE is "-". It is how a shell
   hands over a request without putting it in argv: the process table can
   read argv, and Muroxi allows 4096 bytes of it in all, which one long GET
   form query can exceed by itself. */

#include "engine.h"

#include <stdio.h>

typedef struct {
    DEngineRequest request;
    const char *markdown_from;     /* convert a reader page on disk instead */
    const char *cells_from;
    bool help;
    bool version;
    /* Storage the request points into. */
    char **owned;
    size_t owned_count, owned_capacity;
    char *request_data;            /* wiped when freed: it may hold a password */
} DEngineArgs;

/* 0 when the options make a request; 2 when they do not, with `*error`
   saying why (caller frees), or NULL when the usage text is the answer. */
int d_engine_args_parse(DEngineArgs *args, int argc, const char *const *argv, char **error);
void d_engine_args_free(DEngineArgs *args);

void d_engine_args_usage(FILE *out, const char *program);

#endif
