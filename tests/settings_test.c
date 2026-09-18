/* Settings without GLib, checked against the file GLib writes.

   The desktop shell's settings.ini predates this reader, so the claim that
   both shells read one file is only as good as this reader's agreement with
   GKeyFile. When GLib is available the test asks GKeyFile itself -- in both
   directions -- rather than trusting a description of its format. */

#define _POSIX_C_SOURCE 200809L

#include "settings.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef DISTILL_TEST_GLIB
#include <glib.h>
#endif

static char directory[] = "/tmp/distill-settings-XXXXXX";

static char *path_in(const char *name)
{
    char *path = malloc(strlen(directory) + strlen(name) + 2);
    sprintf(path, "%s/%s", directory, name);
    return path;
}

static char *read_all(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    static char buffer[8192];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[n] = '\0';
    fclose(file);
    return buffer;
}

static void test_round_trip_and_preservation(void)
{
    char *path = path_in("nested/dir/settings.ini");
    DSettings *settings = d_settings_load(path);
    assert(!d_settings_get(settings, "browser", "theme"));
    assert(d_settings_get_int(settings, "browser", "zoom", 100) == 100);
    d_settings_set(settings, "browser", "theme", "night");
    d_settings_set_int(settings, "browser", "zoom", 125);
    assert(d_settings_save(settings));
    d_settings_free(settings);

    struct stat info;
    char *dir = path_in("nested/dir");
    assert(stat(dir, &info) == 0 && (info.st_mode & 0777) == 0700);
    free(dir);

    settings = d_settings_load(path);
    assert(!strcmp(d_settings_get(settings, "browser", "theme"), "night"));
    assert(d_settings_get_int(settings, "browser", "zoom", 100) == 125);
    d_settings_free(settings);

    /* Setting one key keeps every other line, comments and foreign groups
       included, where they were. */
    FILE *file = fopen(path, "w");
    fputs("# kept\n[other]\nx=1\n\n[browser]\ntheme = paper\nstart-page=welcome\n\n[tail]\ny=2\n", file);
    fclose(file);
    settings = d_settings_load(path);
    assert(!strcmp(d_settings_get(settings, "browser", "theme"), "paper"));
    assert(!d_settings_get(settings, "other", "theme"));
    d_settings_set(settings, "browser", "theme", "warm");
    d_settings_set_int(settings, "browser", "zoom", 90);
    assert(d_settings_save(settings));
    d_settings_free(settings);
    assert(!strcmp(read_all(path),
        "# kept\n[other]\nx=1\n\n[browser]\ntheme=warm\nstart-page=welcome\nzoom=90\n\n[tail]\ny=2\n"));

    /* A number that is not one falls back rather than reading as 0. */
    settings = d_settings_load(path);
    d_settings_set(settings, "browser", "zoom", "12x");
    assert(d_settings_get_int(settings, "browser", "zoom", 100) == 100);
    d_settings_free(settings);
    free(path);
}

#ifdef DISTILL_TEST_GLIB
static const char *awkward[] = {
    "  leading spaces", "line\nbreak", "tab\there", "back\\slash", "plain",
    "https://example.com/?q=a b&c=d", "\xc3\xa5\xc3\xa4\xc3\xb6 unicode",
};

static void test_against_gkeyfile(void)
{
    char *path = path_in("glib.ini");
    size_t count = sizeof(awkward) / sizeof(awkward[0]);

    /* GKeyFile writes, we read. */
    GKeyFile *keys = g_key_file_new();
    for (size_t i = 0; i < count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "k%zu", i);
        g_key_file_set_string(keys, "browser", key, awkward[i]);
    }
    g_key_file_set_integer(keys, "browser", "zoom", 150);
    assert(g_key_file_save_to_file(keys, path, NULL));
    g_key_file_free(keys);
    DSettings *settings = d_settings_load(path);
    for (size_t i = 0; i < count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "k%zu", i);
        const char *value = d_settings_get(settings, "browser", key);
        assert(value && !strcmp(value, awkward[i]));
    }
    assert(d_settings_get_int(settings, "browser", "zoom", 0) == 150);

    /* We write, GKeyFile reads. */
    for (size_t i = 0; i < count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "w%zu", i);
        d_settings_set(settings, "written", key, awkward[i]);
    }
    assert(d_settings_save(settings));
    d_settings_free(settings);
    keys = g_key_file_new();
    assert(g_key_file_load_from_file(keys, path, G_KEY_FILE_KEEP_COMMENTS, NULL));
    for (size_t i = 0; i < count; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "w%zu", i);
        char *value = g_key_file_get_string(keys, "written", key, NULL);
        assert(value && !strcmp(value, awkward[i]));
        g_free(value);
    }
    g_key_file_free(keys);
    free(path);
    puts("settings: agrees with GKeyFile in both directions");
}
#endif

int main(void)
{
    assert(mkdtemp(directory));
    test_round_trip_and_preservation();
#ifdef DISTILL_TEST_GLIB
    test_against_gkeyfile();
#endif
    char command[128];
    snprintf(command, sizeof(command), "rm -rf %s", directory);
    assert(system(command) == 0);
    puts("settings: ok");
    return 0;
}
