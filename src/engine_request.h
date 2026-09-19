#ifndef DISTILL_ENGINE_REQUEST_H
#define DISTILL_ENGINE_REQUEST_H

/* Writing a request file for the engine (engine_args.h), for a shell that
   runs it as a process. The request goes in a private file and the engine is
   started with `--request FILE`: nothing of it reaches argv, which the
   process table can read and which Muroxi limits to 4096 bytes in all. */

#include "cutil.h"

typedef struct {
    DBuffer text;
    bool invalid;       /* a value held a newline, which a line cannot carry */
} DRequestFile;

void d_request_file_init(DRequestFile *request);

/* One option: `name` without its dashes, and a value, or NULL for a flag. */
void d_request_file_add(DRequestFile *request, const char *name, const char *value);

/* Write it to a new private file in the temporary directory and free the
   text. The path, which the caller removes once the engine has read it, or
   NULL. */
char *d_request_file_write(DRequestFile *request);

#endif
