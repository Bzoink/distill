#include "settings.h"

#include "cutil.h"
#include "dfs.h"

#include <errno.h>

/* The file is kept as its lines. Reading one finds it; writing one replaces
   it in place, or adds it at the end of its group. */
struct DSettings {
    char *path;
    char **lines;
    size_t count;
    size_t capacity;
    char *last_read;    /* what the last d_settings_get returned */
};

char *d_settings_default_path(void)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    if (config && *config) return d_cat2(config, "/distill/settings.ini");
    const char *home = getenv("HOME");
    return d_cat2(home && *home ? home : ".", "/.config/distill/settings.ini");
}

static void push_line(DSettings *settings, size_t at, char *line)
{
    d_vec_reserve((void **)&settings->lines, &settings->capacity, settings->count,
                  sizeof(*settings->lines), settings->count + 1);
    memmove(settings->lines + at + 1, settings->lines + at,
            (settings->count - at) * sizeof(*settings->lines));
    settings->lines[at] = line;
    ++settings->count;
}

DSettings *d_settings_load(const char *path)
{
    DSettings *settings = d_xcalloc(1, sizeof(*settings));
    settings->path = path ? d_strdup(path) : d_settings_default_path();
    size_t length = 0;
    char *data = d_read_file(settings->path, &length);
    if (!data) return settings;
    char *start = data;
    while (start < data + length) {
        char *end = memchr(start, '\n', (size_t)(data + length - start));
        size_t line_length = end ? (size_t)(end - start) : (size_t)(data + length - start);
        if (line_length && start[line_length - 1] == '\r') --line_length;
        push_line(settings, settings->count, d_strndup(start, line_length));
        if (!end) break;
        start = end + 1;
    }
    free(data);
    return settings;
}

void d_settings_free(DSettings *settings)
{
    if (!settings) return;
    for (size_t i = 0; i < settings->count; ++i) free(settings->lines[i]);
    free(settings->lines);
    free(settings->path);
    free(settings->last_read);
    free(settings);
}

static bool is_group_line(const char *line, const char *group)
{
    const char *at = line;
    while (*at == ' ' || *at == '\t') ++at;
    if (*at != '[') return false;
    if (!group) return true;
    size_t length = strlen(group);
    return !strncmp(at + 1, group, length) && at[1 + length] == ']';
}

/* The key a line assigns, if it assigns one: GKeyFile allows spaces around
   the '='. Returns the start of the raw value. */
static const char *line_value(const char *line, const char *key)
{
    const char *at = line;
    while (*at == ' ' || *at == '\t') ++at;
    if (*at == '#' || *at == ';' || *at == '[' || !*at) return NULL;
    size_t length = strlen(key);
    if (strncmp(at, key, length)) return NULL;
    at += length;
    while (*at == ' ' || *at == '\t') ++at;
    if (*at != '=') return NULL;
    ++at;
    while (*at == ' ' || *at == '\t') ++at;
    return at;
}

/* Find the line holding group.key; also reports where the group's lines end,
   which is where a new key belongs. */
static bool find_key(const DSettings *settings, const char *group, const char *key,
                     size_t *line, size_t *group_end, bool *group_found)
{
    bool in_group = false;
    *group_found = false;
    *group_end = settings->count;
    for (size_t i = 0; i < settings->count; ++i) {
        const char *text = settings->lines[i];
        if (is_group_line(text, NULL)) {
            if (in_group) { *group_end = i; in_group = false; }
            if (is_group_line(text, group)) { in_group = true; *group_found = true; *group_end = settings->count; }
            continue;
        }
        if (in_group && line_value(text, key)) { *line = i; return true; }
    }
    return false;
}

static char *unescape(const char *raw)
{
    DBuffer out;
    dbuf_init(&out);
    for (const char *at = raw; *at; ++at) {
        if (*at != '\\' || !at[1]) { dbuf_append(&out, at, 1); continue; }
        char c = *++at;
        const char *replacement = c == 's' ? " " : c == 'n' ? "\n" : c == 't' ? "\t"
                                : c == 'r' ? "\r" : c == '\\' ? "\\" : NULL;
        if (replacement) dbuf_puts(&out, replacement);
        else { dbuf_append(&out, "\\", 1); dbuf_append(&out, &c, 1); }
    }
    /* Trailing whitespace is not part of a GKeyFile value. */
    while (out.length && (out.data[out.length - 1] == ' ' || out.data[out.length - 1] == '\t') &&
           !(out.length >= 2 && out.data[out.length - 2] == '\\'))
        --out.length;
    if (!out.data) dbuf_puts(&out, "");
    else out.data[out.length] = '\0';
    return dbuf_take(&out);
}

static char *escape(const char *value)
{
    DBuffer out;
    dbuf_init(&out);
    for (const char *at = value; *at; ++at) {
        if (*at == ' ' && at == value) dbuf_puts(&out, "\\s");
        else if (*at == '\n') dbuf_puts(&out, "\\n");
        else if (*at == '\t') dbuf_puts(&out, "\\t");
        else if (*at == '\r') dbuf_puts(&out, "\\r");
        else if (*at == '\\') dbuf_puts(&out, "\\\\");
        else dbuf_append(&out, at, 1);
    }
    if (!out.data) dbuf_puts(&out, "");
    return dbuf_take(&out);
}

const char *d_settings_get(const DSettings *settings, const char *group, const char *key)
{
    if (!settings) return NULL;
    size_t line = 0, end = 0;
    bool found_group = false;
    if (!find_key(settings, group, key, &line, &end, &found_group)) return NULL;
    DSettings *owner = (DSettings *)settings;
    free(owner->last_read);
    owner->last_read = unescape(line_value(settings->lines[line], key));
    return owner->last_read;
}

int d_settings_get_int(const DSettings *settings, const char *group, const char *key, int fallback)
{
    const char *value = d_settings_get(settings, group, key);
    if (!value || !*value) return fallback;
    char *end = NULL;
    errno = 0;
    long number = strtol(value, &end, 10);
    if (errno || !end || *end || number < -2147483647L || number > 2147483647L) return fallback;
    return (int)number;
}

void d_settings_set(DSettings *settings, const char *group, const char *key, const char *value)
{
    if (!settings || !group || !key) return;
    char *escaped = escape(value ? value : "");
    char *line = d_cat3(key, "=", escaped);
    free(escaped);
    size_t at = 0, end = 0;
    bool found_group = false;
    if (find_key(settings, group, key, &at, &end, &found_group)) {
        free(settings->lines[at]);
        settings->lines[at] = line;
        return;
    }
    if (!found_group) {
        /* GKeyFile separates groups with a blank line. */
        if (settings->count && *settings->lines[settings->count - 1])
            push_line(settings, settings->count, d_strdup(""));
        push_line(settings, settings->count, d_cat3("[", group, "]"));
        push_line(settings, settings->count, line);
        return;
    }
    /* After the group's last key, before any blank lines that end it. */
    while (end > 0 && !*settings->lines[end - 1] && !is_group_line(settings->lines[end - 1], NULL)) --end;
    push_line(settings, end, line);
}

void d_settings_set_int(DSettings *settings, const char *group, const char *key, int value)
{
    char number[32];
    snprintf(number, sizeof(number), "%d", value);
    d_settings_set(settings, group, key, number);
}

/* The directory the file goes in, made private if it is missing. */
static bool make_directories(const char *path)
{
    char *copy = d_strdup(path);
    char *slash = strrchr(copy, '/');
    if (!slash) { free(copy); return true; }
    *slash = '\0';
    bool ok = d_fs_make_directories(copy);
    free(copy);
    return ok;
}

bool d_settings_save(const DSettings *settings)
{
    if (!settings || !make_directories(settings->path)) return false;
    DBuffer out;
    dbuf_init(&out);
    for (size_t i = 0; i < settings->count; ++i) {
        dbuf_puts(&out, settings->lines[i]);
        dbuf_puts(&out, "\n");
    }
    /* Replaced whole, never rewritten in place: a crash mid-save must leave
       the old settings, not half of the new ones. */
    bool ok = d_fs_replace(settings->path, out.data ? out.data : "", out.length);
    dbuf_free(&out);
    return ok;
}
