#include "engine_request.h"

#include "dfs.h"

void d_request_file_init(DRequestFile *request)
{
    dbuf_init(&request->text);
    request->invalid = false;
}

void d_request_file_add(DRequestFile *request, const char *name, const char *value)
{
    if (value && (strchr(value, '\n') || strchr(value, '\r'))) {
        request->invalid = true;
        return;
    }
    dbuf_puts(&request->text, name);
    if (value) {
        dbuf_puts(&request->text, " ");
        dbuf_puts(&request->text, value);
    }
    dbuf_puts(&request->text, "\n");
}

char *d_request_file_write(DRequestFile *request)
{
    char *path = NULL;
    if (!request->invalid) {
        path = d_fs_private_file(NULL, "distill-request");
        if (path && !d_fs_overwrite(path, request->text.data ? request->text.data : "",
                                    request->text.length)) {
            d_fs_remove(path);
            free(path);
            path = NULL;
        }
    }
    dbuf_free(&request->text);
    return path;
}
