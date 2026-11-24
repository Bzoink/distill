#ifndef DISTILL_WINDOWS_WIN_H
#define DISTILL_WINDOWS_WIN_H

#include <windows.h>
#include "cutil.h"

/* The platform interfaces speak UTF-8. Only the Win32 boundary uses UTF-16. */
static inline wchar_t *d_win_wide(const char *text)
{
    if (!text) return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) return NULL;
    wchar_t *result = d_xcalloc((size_t)count, sizeof(*result));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, count))
        return result;
    free(result);
    return NULL;
}

static inline char *d_win_utf8(const wchar_t *text)
{
    if (!text) return NULL;
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    if (!count) return NULL;
    char *result = d_xcalloc((size_t)count, 1);
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result, count, NULL, NULL))
        return result;
    free(result);
    return NULL;
}

static inline char *d_win_path(wchar_t *path)
{
    for (wchar_t *p = path; *p; ++p) if (*p == L'\\') *p = L'/';
    return d_win_utf8(path);
}

static inline void d_win_error(char **error, const char *what, DWORD code)
{
    if (!error) return;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s (Windows error %lu)", what, (unsigned long)code);
    *error = d_strdup(buffer);
}

#endif
