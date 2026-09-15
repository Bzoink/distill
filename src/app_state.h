#ifndef DISTILL_APP_STATE_H
#define DISTILL_APP_STATE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *url;
    char *png_path;
    char *template_path;
    char *theme;
} DAppPage;

typedef struct {
    DAppPage *entries;
    size_t length;
    size_t capacity;
    size_t current;
} DAppHistory;

typedef void (*DAppPageDisposer)(DAppPage *page);

void d_app_history_init(DAppHistory *history);
void d_app_history_clear(DAppHistory *history, DAppPageDisposer disposer);
bool d_app_history_push(DAppHistory *history, const char *url,
                        const char *png_path, const char *template_path,
                        const char *theme, DAppPageDisposer disposer);
bool d_app_history_replace_current(DAppHistory *history, const char *url,
                                   const char *png_path, const char *template_path,
                                   const char *theme, DAppPageDisposer disposer);
bool d_app_history_back(DAppHistory *history);
bool d_app_history_forward(DAppHistory *history);
bool d_app_history_can_back(const DAppHistory *history);
bool d_app_history_can_forward(const DAppHistory *history);
const DAppPage *d_app_history_current(const DAppHistory *history);

#endif
