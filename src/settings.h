#ifndef DISTILL_SETTINGS_H
#define DISTILL_SETTINGS_H

/* Browser preferences in ~/.config/distill/settings.ini, readable and
   writable without GLib.

   The desktop shell wrote this file through GKeyFile before any other front
   end existed, so this reads what GKeyFile writes -- groups, key=value lines,
   comments, and its escapes (\s \n \t \r \\) -- and writes the same. Every
   front end reads one file, and a preference set in one is the preference in
   the other. Setting a key rewrites that one line and keeps every other line
   of the file as it was, comments included. */

#include <stdbool.h>

typedef struct DSettings DSettings;

/* Load `path`, or the default location when NULL. A missing file is an empty
   settings object, not an error. */
DSettings *d_settings_load(const char *path);
void d_settings_free(DSettings *settings);

/* NULL when absent. The string belongs to the settings object and stays
   valid until the next d_settings_get on it. */
const char *d_settings_get(const DSettings *settings, const char *group, const char *key);
int d_settings_get_int(const DSettings *settings, const char *group, const char *key,
                       int fallback);

void d_settings_set(DSettings *settings, const char *group, const char *key, const char *value);
void d_settings_set_int(DSettings *settings, const char *group, const char *key, int value);

/* Written whole through a temporary file and a rename, as GKeyFile does, so
   a crash mid-write cannot leave half a file. Creates the directory 0700. */
bool d_settings_save(const DSettings *settings);

/* $XDG_CONFIG_HOME/distill/settings.ini, else ~/.config/distill/settings.ini.
   Caller frees. */
char *d_settings_default_path(void);

#endif
