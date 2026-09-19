#include "datadir.h"

#include "cutil.h"
#include "dfs.h"
#include "dproc.h"

#ifndef DISTILL_INSTALL_DATADIR
#define DISTILL_INSTALL_DATADIR ""
#endif

/* The candidate if it exists and fits, else NULL. Frees `base`. */
static char *candidate(char *base, const char *relative, bool *too_long)
{
    if (!base || !*base) { free(base); return NULL; }
    char *path = d_cat3(base, "/", relative);
    free(base);
    if (strlen(path) >= d_fs_path_limit()) {
        *too_long = true;
        free(path);
        return NULL;
    }
    if (d_fs_exists(path)) return path;
    free(path);
    return NULL;
}

char *d_data_path(const char *relative, char **error)
{
    if (error) *error = NULL;
    bool too_long = false;
    char *found = NULL;
    const char *configured = getenv("DISTILL_DATA_DIR");
    if (configured && *configured) found = candidate(d_strdup(configured), relative, &too_long);
    char *directory = found ? NULL : d_proc_executable_directory();
    if (!found && directory) found = candidate(d_cat2(directory, "/share/distill"), relative, &too_long);
    if (!found && directory) found = candidate(d_cat2(directory, "/../share/distill"), relative, &too_long);
    free(directory);
    if (!found) found = candidate(d_strdup(DISTILL_INSTALL_DATADIR), relative, &too_long);
    if (!found && error)
        *error = d_cat3(too_long ? "the path to Distill's data is longer than this system allows: "
                                 : "Distill's data is missing: ", relative, "");
    return found;
}
