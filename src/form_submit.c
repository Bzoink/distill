#include "form_submit.h"

#include "cutil.h"


static bool has_scheme(const char *uri, const char *scheme)
{
    size_t length = strlen(scheme);
    return uri && d_ncasecmp(uri, scheme, length) == 0 && uri[length] == ':';
}

bool d_page_uri_allowed(const char *uri)
{
    if (!uri || !*uri) return false;
    if (has_scheme(uri, "http") || has_scheme(uri, "https") || has_scheme(uri, "file")) return true;
    return !strchr(uri, ':');
}

bool d_external_uri_allowed(const char *uri)
{
    return has_scheme(uri, "http") || has_scheme(uri, "https") ||
           has_scheme(uri, "mailto") || has_scheme(uri, "tel");
}

char *d_form_encode(const char *text)
{
    DBuffer buffer;
    dbuf_init(&buffer);
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            dbuf_append(&buffer, (const char *)p, 1);
        } else if (*p == ' ') {
            dbuf_puts(&buffer, "+");
        } else {
            dbuf_printf(&buffer, "%%%02X", *p);
        }
    }
    if (!buffer.data) dbuf_puts(&buffer, "");
    return dbuf_take(&buffer);
}

char *d_form_append_query(const char *action, const char *data)
{
    const char *hash = strchr(action, '#');
    size_t prefix_length = hash ? (size_t)(hash - action) : strlen(action);
    char *prefix = d_strndup(action, prefix_length);
    const char *separator = strchr(prefix, '?') ? (data && *data ? "&" : "") : (data && *data ? "?" : "");
    char *out = d_cat4(prefix, separator, data ? data : "", hash ? hash : "");
    free(prefix);
    return out;
}

static void append_pair(DBuffer *data, const char *name, const char *value)
{
    if (!name || !*name) return;
    if (data->length) dbuf_puts(data, "&");
    char *encoded_name = d_form_encode(name);
    char *encoded_value = d_form_encode(value ? value : "");
    dbuf_puts(data, encoded_name);
    dbuf_puts(data, "=");
    dbuf_puts(data, encoded_value);
    free(encoded_name);
    free(encoded_value);
}

static bool type_is(const DFormField *field, const char *type)
{
    return field->type && d_casecmp(field->type, type) == 0;
}

static bool is_choice(const DFormField *field)
{
    return type_is(field, "checkbox") || type_is(field, "radio");
}

static bool is_action(const DFormField *field)
{
    return type_is(field, "submit") || type_is(field, "reset") || type_is(field, "button");
}

/* A required radio is satisfied by any radio of its name in its form. */
static bool field_is_empty(const DFormField *fields, size_t count, const DFormField *field)
{
    if (type_is(field, "radio")) {
        for (size_t i = 0; i < count; ++i) {
            const DFormField *other = &fields[i];
            if (other->form_index == field->form_index && type_is(other, "radio") &&
                !strcmp(other->name ? other->name : "", field->name ? field->name : "") &&
                other->checked)
                return false;
        }
        return true;
    }
    if (is_choice(field)) return !field->checked;
    return !field->value || !*field->value;
}

static bool field_is_successful(const DFormField *field, const DFormField *submitter)
{
    if (field->disabled || !field->name || !*field->name) return false;
    if (type_is(field, "submit")) return field == submitter;
    if (type_is(field, "reset") || type_is(field, "button")) return false;
    if (is_choice(field)) return field->checked;
    return true;
}

DFormStatus d_form_build(const DFormField *fields, size_t count, size_t submitter_index,
                         const char *action, const char *method, DFormRequest *out)
{
    *out = (DFormRequest){0};
    if (!fields || submitter_index >= count) return D_FORM_EMPTY;
    const DFormField *submitter = &fields[submitter_index];
    unsigned form = submitter->form_index;
    size_t in_form = 0;
    for (size_t i = 0; i < count; ++i) in_form += fields[i].form_index == form;
    if (!in_form) return D_FORM_EMPTY;

    for (size_t i = 0; i < count; ++i) {
        const DFormField *field = &fields[i];
        if (field->form_index != form || field->disabled || !field->required || is_action(field))
            continue;
        if (field_is_empty(fields, count, field)) {
            out->missing = d_strdup(field->name && *field->name ? field->name : "field");
            return D_FORM_MISSING_REQUIRED;
        }
    }

    DBuffer data;
    dbuf_init(&data);
    for (size_t i = 0; i < count; ++i) {
        const DFormField *field = &fields[i];
        if (field->form_index != form || !field_is_successful(field, submitter)) continue;
        append_pair(&data, field->name, field->value);
    }
    if (!d_page_uri_allowed(action)) {
        dbuf_free(&data);
        return D_FORM_BLOCKED;
    }
    bool post = method && d_casecmp(method, "post") == 0;
    const char *body = data.data ? data.data : "";
    out->method = d_strdup(post ? "POST" : "GET");
    out->target = post ? d_strdup(action) : d_form_append_query(action, body);
    out->body = post ? d_strdup(body) : NULL;
    dbuf_free(&data);
    return D_FORM_OK;
}

void d_form_request_free(DFormRequest *request)
{
    if (!request) return;
    free(request->target);
    free(request->method);
    /* The body may be a password. */
    if (request->body) memset(request->body, 0, strlen(request->body));
    free(request->body);
    free(request->missing);
    *request = (DFormRequest){0};
}
