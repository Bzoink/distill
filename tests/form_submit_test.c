/* The request a form sends, independent of which front end filled it in. */

#include "form_submit.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static DFormStatus build(const DFormField *fields, size_t count, size_t submitter,
                         const char *action, const char *method, DFormRequest *out)
{
    return d_form_build(fields, count, submitter, action, method, out);
}

static void test_get_and_encoding(void)
{
    DFormField fields[] = {
        {.form_index = 0, .tag = "input", .type = "search", .name = "q", .value = "a b&c/\xc3\xa5"},
        {.form_index = 0, .tag = "input", .type = "hidden", .name = "token", .value = "T"},
        {.form_index = 0, .tag = "button", .type = "submit", .name = "go", .value = "Search"},
        {.form_index = 0, .tag = "button", .type = "submit", .name = "other", .value = "No"},
        {.form_index = 0, .tag = "input", .type = "text", .name = "", .value = "unnamed"},
        {.form_index = 0, .tag = "input", .type = "text", .name = "off", .value = "x", .disabled = true},
        {.form_index = 1, .tag = "input", .type = "text", .name = "elsewhere", .value = "y"},
    };
    DFormRequest request;
    assert(build(fields, 7, 2, "https://example.com/find?lang=en#top", "get", &request) == D_FORM_OK);
    assert(!strcmp(request.method, "GET"));
    assert(!request.body);
    assert(!strcmp(request.target,
                   "https://example.com/find?lang=en&q=a+b%26c%2F%C3%A5&token=T&go=Search#top"));
    d_form_request_free(&request);

    assert(build(fields, 7, 2, "https://example.com/find", "GET", &request) == D_FORM_OK);
    assert(!strcmp(request.target, "https://example.com/find?q=a+b%26c%2F%C3%A5&token=T&go=Search"));
    d_form_request_free(&request);
}

static void test_post_and_choices(void)
{
    DFormField fields[] = {
        {.tag = "input", .type = "password", .name = "pw", .value = "s3cret"},
        {.tag = "input", .type = "checkbox", .name = "keep", .value = "on", .checked = true},
        {.tag = "input", .type = "checkbox", .name = "spam", .value = "on", .checked = false},
        {.tag = "input", .type = "radio", .name = "size", .value = "s"},
        {.tag = "input", .type = "radio", .name = "size", .value = "m", .checked = true},
        {.tag = "select", .type = "text", .name = "pick", .value = "b"},
        {.tag = "button", .type = "submit", .name = "", .value = ""},
    };
    DFormRequest request;
    assert(build(fields, 7, 6, "https://example.com/login", "Post", &request) == D_FORM_OK);
    assert(!strcmp(request.method, "POST"));
    assert(!strcmp(request.target, "https://example.com/login"));
    assert(!strcmp(request.body, "pw=s3cret&keep=on&size=m&pick=b"));
    d_form_request_free(&request);
    assert(!request.body && !request.target);
}

static void test_refusals(void)
{
    DFormField fields[] = {
        {.tag = "input", .type = "text", .name = "who", .value = "", .required = true},
        {.tag = "input", .type = "radio", .name = "r", .value = "1", .required = true},
        {.tag = "input", .type = "radio", .name = "r", .value = "2"},
        {.tag = "button", .type = "submit", .name = "go", .value = "1"},
    };
    DFormRequest request;
    assert(build(fields, 4, 3, "https://example.com/", "get", &request) == D_FORM_MISSING_REQUIRED);
    assert(!strcmp(request.missing, "who"));
    d_form_request_free(&request);

    fields[0].value = "me";
    assert(build(fields, 4, 3, "https://example.com/", "get", &request) == D_FORM_MISSING_REQUIRED);
    assert(!strcmp(request.missing, "r"));
    d_form_request_free(&request);

    /* Any radio of the name satisfies it, not only the one marked required. */
    fields[2].checked = true;
    assert(build(fields, 4, 3, "https://example.com/", "get", &request) == D_FORM_OK);
    assert(!strcmp(request.target, "https://example.com/?who=me&r=2&go=1"));
    d_form_request_free(&request);

    assert(build(fields, 4, 3, "javascript:alert(1)", "get", &request) == D_FORM_BLOCKED);
    d_form_request_free(&request);
    assert(build(fields, 4, 9, "https://example.com/", "get", &request) == D_FORM_EMPTY);
}

static void test_uri_rules(void)
{
    assert(d_page_uri_allowed("https://a/") && d_page_uri_allowed("file:///x") &&
           d_page_uri_allowed("example.com/path"));
    assert(!d_page_uri_allowed("distill-play:https://a/x.mp3") && !d_page_uri_allowed(""));
    assert(d_external_uri_allowed("mailto:a@b") && !d_external_uri_allowed("file:///etc/passwd"));
}

int main(void)
{
    test_get_and_encoding();
    test_post_and_choices();
    test_refusals();
    test_uri_rules();
    puts("form submit: ok");
    return 0;
}
