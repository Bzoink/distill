#ifndef DISTILL_READER_WALK_H
#define DISTILL_READER_WALK_H

/* The reader page as a stream of events.

   Every front end that is not litehtml renders the reader page by walking it:
   the Markdown export writes the events as text, the terminal writes them as
   cells. One walk means one reading of the page -- the shape the reader chose,
   its cards and their order reach every front end without any of them knowing
   what a shape is, and a change to the template reaches them all at once.

   The events are a calling convention, not a structure. Nothing is built that
   outlives the walk, so there is no third representation to keep in step with
   the extraction JSON and the page. A sink that needs two passes over
   something -- a table, measured across every row before the first is drawn --
   buffers that one block itself between its open and close events. */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    READER_BLOCK_GENERIC,   /* p, div, section, main, article, form */
    READER_BLOCK_CARD,      /* a section the template made a card: audio, video, form... */
    READER_BLOCK_HEADING,
    READER_BLOCK_LIST,
    READER_BLOCK_ITEM,
    READER_BLOCK_QUOTE,
    READER_BLOCK_PRE,
} ReaderBlockKind;

typedef struct {
    ReaderBlockKind kind;
    const char *tag;
    const char *id;          /* "" when absent; a section's id is its jump target */
    const char *class_name;  /* "" when absent */
    /* For READER_BLOCK_CARD: "audio", "video", "table", "form", "results",
       "gallery". Named here once so no sink has to parse class lists. */
    const char *card;
    int level;               /* heading level, 1..6 */
    bool ordered;            /* list */
} ReaderBlock;

enum {
    READER_STYLE_STRONG = 1u << 0,
    READER_STYLE_EM     = 1u << 1,
    READER_STYLE_CODE   = 1u << 2,
};

typedef struct {
    const char *href;   /* as the template wrote it, internal schemes included */
    size_t index;       /* document order among links, from 0: a focus target */
    bool button;        /* the template drew it as an action */
} ReaderLink;

typedef struct {
    const char *src;
    const char *alt;
} ReaderImage;

/* The fields the desktop shell reads out of a laid-out form, filled the same
   way (see collect_form_boxes in litehtml_probe.cpp): whatever submits from a
   terminal submits the same request. */
typedef struct {
    bool in_form;
    unsigned form_index;       /* document order among forms, from 0 */
    const char *action;
    const char *method;        /* "get" unless the form says otherwise */
    const char *tag;           /* input, button, select, textarea */
    const char *type_attr;     /* the type attribute exactly as written */
    const char *type;          /* data-distill-type, else type, else "text" */
    const char *id;
    const char *name;
    const char *value;         /* a textarea's text; a select's chosen value */
    const char *value_attr;    /* the value attribute exactly as written */
    const char *placeholder;
    const char *caption;       /* a button's text */
    /* Each choice is value, label, selectedness, separated by ASCII unit and
       record separators -- the same encoding DFormBox carries. */
    const char *options;
    bool checked;
    bool disabled;
    bool required;
    size_t index;              /* document order among controls, from 0 */
} ReaderControl;

typedef struct {
    const char *title;
    const char *dir;    /* "rtl", "ltr" or "" */
    const char *lang;
} ReaderPage;

typedef struct {
    void *opaque;
    void (*page)(void *opaque, const ReaderPage *page);
    void (*block_open)(void *opaque, const ReaderBlock *block);
    void (*block_close)(void *opaque, const ReaderBlock *block);
    /* Text exactly as the page holds it. Collapsing whitespace is the sink's
       business: inside a PRE block it must not happen. */
    void (*text)(void *opaque, const char *text, size_t length);
    void (*style_open)(void *opaque, unsigned style);
    void (*style_close)(void *opaque, unsigned style);
    void (*link_open)(void *opaque, const ReaderLink *link);
    void (*link_close)(void *opaque, const ReaderLink *link);
    void (*line_break)(void *opaque);
    void (*rule)(void *opaque);
    void (*image)(void *opaque, const ReaderImage *image);
    void (*control)(void *opaque, const ReaderControl *control);
    /* A table arrives as open, its rows in order, close. Cell text is already
       whitespace-collapsed; `header` marks th cells. */
    void (*table_open)(void *opaque);
    void (*table_row)(void *opaque, const char *const *cells, const bool *header,
                      size_t count);
    void (*table_close)(void *opaque);
    /* A section the shell can jump to begins here. */
    void (*anchor)(void *opaque, const char *section_id);
} ReaderSink;

/* Walk a reader page. Any callback may be NULL. False when the page could
   not be parsed at all. */
bool reader_walk(const char *html, const ReaderSink *sink);

/* The reader page as Markdown: what the reader shows, as a document. The
   source address is recorded at the end when `url` is given. */
char *reader_markdown(const char *html, const char *url);

#endif
