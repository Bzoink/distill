#ifndef DISTILL_FORM_SUBMIT_H
#define DISTILL_FORM_SUBMIT_H

/* Turning a filled-in form into a request, for every front end.

   A form submitted from the terminal must send exactly what the same form
   submitted from the desktop sends: the same fields, the same encoding, the
   same refusal when a required field is empty. So the rules live here, once,
   and each shell only says what its controls currently hold. */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    unsigned form_index;
    const char *tag;       /* input, button, select, textarea */
    const char *type;      /* data-distill-type, else type, else "text" */
    const char *name;
    /* What the control holds now: typed text, the chosen option's value, or
       for a checkbox or radio its value attribute (sent only when checked). */
    const char *value;
    bool checked;
    bool disabled;
    bool required;
} DFormField;

typedef enum {
    D_FORM_OK,
    D_FORM_EMPTY,            /* the submitter's form has no controls */
    D_FORM_MISSING_REQUIRED, /* `missing` names the field */
    D_FORM_BLOCKED,          /* the action is not a page address */
} DFormStatus;

typedef struct {
    char *target;   /* the URL to load: the action, with the query for GET */
    char *method;   /* "GET" or "POST" */
    char *body;     /* POST data, or NULL */
    char *missing;  /* for D_FORM_MISSING_REQUIRED */
} DFormRequest;

/* Build the request `fields[submitter]` sends. The action and method are the
   form's own; method is case-insensitive and anything but post is get. */
DFormStatus d_form_build(const DFormField *fields, size_t count, size_t submitter,
                         const char *action, const char *method, DFormRequest *out);
void d_form_request_free(DFormRequest *request);

/* application/x-www-form-urlencoded, as the desktop shell has always sent it. */
char *d_form_encode(const char *text);
char *d_form_append_query(const char *action, const char *data);

/* An address the reader may load as a page: http, https, file, or one with
   no scheme at all. */
bool d_page_uri_allowed(const char *uri);
/* An address that may be handed to the system: http, https, mailto, tel. */
bool d_external_uri_allowed(const char *uri);

#endif
