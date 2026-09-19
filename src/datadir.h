#ifndef DISTILL_DATADIR_H
#define DISTILL_DATADIR_H

/* Where Distill's data lives: fonts, site profiles, the welcome page.

   No binary refers to the source tree. Data is found, in order, in:
     $DISTILL_DATA_DIR                 set by whoever runs it
     <executable's directory>/share/distill      the build tree, staged by CMake
     <executable's directory>/../share/distill   an installed layout, wherever it was put
     DISTILL_INSTALL_DATADIR           the directory the build was configured to install to

   The first that has `relative` wins. A path longer than the platform
   allows is refused rather than cut short. */

/* The full path of `relative` (e.g. "profiles", "welcome.html"), or NULL
   with `*error` saying why (caller frees both). */
char *d_data_path(const char *relative, char **error);

#endif
