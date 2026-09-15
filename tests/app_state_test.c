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
    assert(d_app_history_push(&history, "https://one.test", "one.png", "one.html", "warm", dispose));
    assert(d_app_history_push(&history, "https://two.test", "two.png", "two.html", "night", dispose));
    assert(d_app_history_can_back(&history));
    assert(!d_app_history_can_forward(&history));
    assert(d_app_history_back(&history));
    assert(strcmp(d_app_history_current(&history)->url, "https://one.test") == 0);
    assert(d_app_history_forward(&history));
    assert(strcmp(d_app_history_current(&history)->theme, "night") == 0);
    assert(d_app_history_back(&history));
    assert(d_app_history_push(&history, "https://three.test", "three.png", "three.html", "paper", dispose));
    assert(!d_app_history_can_forward(&history));
    assert(strcmp(d_app_history_current(&history)->url, "https://three.test") == 0);
    assert(d_app_history_replace_current(&history, "https://four.test", "four.png", "four.html", "warm", dispose));
    assert(strcmp(d_app_history_current(&history)->url, "https://four.test") == 0);
    d_app_history_clear(&history, dispose);
    puts("app-state: ok");
    return 0;
}
