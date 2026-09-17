#include "app_state.h"

#include "cutil.h"

#include <stdint.h>

static void page_free(DAppPage *page, DAppPageDisposer disposer)
{
    if (!page) return;
    if (disposer) disposer(page);
    free(page->url);
    free(page->template_path);
    free(page->theme);
    *page = (DAppPage){0};
}

static DAppPage page_copy(const char *url, const char *template_path,
                          const char *theme, int zoom)
{
    return (DAppPage){
        .url = d_strdup(url),
        .template_path = d_strdup(template_path),
        .theme = d_strdup(theme ? theme : "warm"),
        .zoom = zoom > 0 ? zoom : 100
    };
}

void d_app_history_init(DAppHistory *history)
{
    if (!history) return;
    *history = (DAppHistory){.current = SIZE_MAX};
}

void d_app_history_clear(DAppHistory *history, DAppPageDisposer disposer)
{
    if (!history) return;
    for (size_t i = 0; i < history->length; ++i) page_free(&history->entries[i], disposer);
    free(history->entries);
    *history = (DAppHistory){.current = SIZE_MAX};
}

static void discard_forward(DAppHistory *history, DAppPageDisposer disposer)
{
    if (!history || history->current == SIZE_MAX) return;
    size_t first = history->current + 1;
    for (size_t i = first; i < history->length; ++i) page_free(&history->entries[i], disposer);
    history->length = first;
}

bool d_app_history_push(DAppHistory *history, const char *url,
                        const char *template_path,
                        const char *theme, int zoom, DAppPageDisposer disposer)
{
    if (!history || !url || !*url) return false;
    if (history->current != SIZE_MAX) discard_forward(history, disposer);
    d_vec_reserve((void **)&history->entries, &history->capacity,
                  history->length, sizeof(*history->entries), history->length + 1);
    history->entries[history->length] = page_copy(url, template_path, theme, zoom);
    history->current = history->length++;
    return true;
}

bool d_app_history_replace_current(DAppHistory *history, const char *url,
                                   const char *template_path,
                                   const char *theme, int zoom,
                                   DAppPageDisposer disposer)
{
    if (!history || history->current == SIZE_MAX || history->current >= history->length)
        return false;
    page_free(&history->entries[history->current], disposer);
    history->entries[history->current] = page_copy(url, template_path, theme, zoom);
    return true;
}

bool d_app_history_back(DAppHistory *history)
{
    if (!d_app_history_can_back(history)) return false;
    --history->current;
    return true;
}

bool d_app_history_forward(DAppHistory *history)
{
    if (!d_app_history_can_forward(history)) return false;
    ++history->current;
    return true;
}

bool d_app_history_can_back(const DAppHistory *history)
{
    return history && history->current != SIZE_MAX && history->current > 0;
}

bool d_app_history_can_forward(const DAppHistory *history)
{
    return history && history->current != SIZE_MAX && history->current + 1 < history->length;
}

const DAppPage *d_app_history_current(const DAppHistory *history)
{
    if (!history || history->current == SIZE_MAX || history->current >= history->length)
        return NULL;
    return &history->entries[history->current];
}
