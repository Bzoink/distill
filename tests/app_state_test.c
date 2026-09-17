#include "app_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void dispose(DAppPage *page)
{
    (void)page;
}

int main(void)
{
    DAppHistory history;
    d_app_history_init(&history);
    assert(!d_app_history_can_back(&history));
    assert(d_app_history_push(&history, "https://one.test", "one.html", "warm", 100, dispose));
    assert(d_app_history_push(&history, "https://two.test", "two.html", "night", 150, dispose));
    assert(d_app_history_can_back(&history));
    assert(!d_app_history_can_forward(&history));
    assert(d_app_history_back(&history));
    assert(strcmp(d_app_history_current(&history)->url, "https://one.test") == 0);
    assert(d_app_history_forward(&history));
    assert(strcmp(d_app_history_current(&history)->theme, "night") == 0);
    /* The zoom a page was laid out at travels with it, so going back does not
       claim the old template was rendered at the current size. */
    assert(d_app_history_current(&history)->zoom == 150);
    assert(d_app_history_back(&history));
    assert(d_app_history_push(&history, "https://three.test", "three.html", "paper", 100, dispose));
    assert(!d_app_history_can_forward(&history));
    assert(strcmp(d_app_history_current(&history)->url, "https://three.test") == 0);
    assert(d_app_history_replace_current(&history, "https://four.test", "four.html", "warm", 125, dispose));
    assert(strcmp(d_app_history_current(&history)->url, "https://four.test") == 0);
    assert(d_app_history_current(&history)->zoom == 125);
    d_app_history_clear(&history, dispose);
    puts("app-state: ok");
    return 0;
}
