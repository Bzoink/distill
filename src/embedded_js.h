#ifndef DISTILL_EMBEDDED_JS_H
#define DISTILL_EMBEDDED_JS_H

/* The JavaScript the engine runs itself, compiled to QuickJS bytecode at
   build time by tools/distill_jsc.c: the linkedom DOM (a module) and
   Readability with its parser (global scripts). Nothing is read from disk. */

#include <stddef.h>

extern const unsigned char distill_js_linkedom[];
extern const size_t distill_js_linkedom_size;
extern const unsigned char distill_js_jsdomparser[];
extern const size_t distill_js_jsdomparser_size;
extern const unsigned char distill_js_readability[];
extern const size_t distill_js_readability_size;

#endif
