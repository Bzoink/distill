#ifndef DISTILL_PLATFORM_DFS_H
#define DISTILL_PLATFORM_DFS_H

/* The file system, for the core. Whole-file reads and plain writes are
   standard C (d_read_file in cutil.h, fopen); what is here is what standard C
   cannot say: permissions, directories, private temporary files and
   replacing a file safely.

   Paths are UTF-8 strings with '/' separators. Every function that returns a
   string returns one the caller frees. */

#include <stddef.h>

/* The longest path a fixed buffer here needs to hold. A platform may refuse
   shorter ones -- Muroxi's limit is 256 bytes -- and then says so. */
#define D_FS_PATH_MAX 4096

/* The longest path, in bytes with its terminator, this platform accepts. */
size_t d_fs_path_limit(void);

bool d_fs_exists(const char *path);
bool d_fs_readable(const char *path);
bool d_fs_executable(const char *path);
bool d_fs_is_directory(const char *path);

/* The absolute form of an existing path, or NULL. */
char *d_fs_absolute(const char *path);

/* The directory relative paths are resolved against, or NULL. */
char *d_fs_current_directory(void);

/* Remove a file, or an empty directory. False if it was not removed. */
bool d_fs_remove(const char *path);
bool d_fs_remove_directory(const char *path);

/* Make a directory and any missing parents, private to this user. */
bool d_fs_make_directories(const char *path);

/* Where temporary files go: the platform's temporary directory. */
char *d_fs_temp_directory(void);

/* A new, empty file only this user can read, named `prefix` plus a unique
   suffix inside `directory` (the temporary directory when NULL). NULL when
   it cannot be made. */
char *d_fs_private_file(const char *directory, const char *prefix);

/* The same, as a directory. */
char *d_fs_private_directory(const char *directory, const char *prefix);

/* A file that is to hold secrets -- a session's cookies -- must be private.
   Checks an existing file, or creates an empty private one. */
typedef enum {
    D_FS_PRIVATE_OK,          /* exists, a regular file, private */
    D_FS_PRIVATE_CREATED,     /* did not exist; created empty and private */
    D_FS_PRIVATE_NOT_A_FILE,  /* exists and is not a regular file */
    D_FS_PRIVATE_SHARED,      /* exists and others can read or write it */
    D_FS_PRIVATE_FAILED,      /* could not be checked or created */
} DFsPrivate;

/* `mode` receives the file's permission bits where they are known, for the
   message that tells the user what to change. */
DFsPrivate d_fs_ensure_private(const char *path, unsigned *mode);

/* Write `length` bytes to an existing file, replacing its contents. */
bool d_fs_overwrite(const char *path, const void *data, size_t length);

/* Replace `path` with `data` so a reader sees the old contents or the new,
   never a mixture: written beside it, flushed to disk, then renamed over it.
   The new file is private. */
bool d_fs_replace(const char *path, const void *data, size_t length);

/* Append in one write, so processes sharing the file do not interleave
   inside it. Creates the file, private, if it is missing. */
bool d_fs_append(const char *path, const void *data, size_t length);

/* Call `each` for every entry of `directory` except "." and "..", in the
   order the platform lists them, until it returns false. `is_directory`
   follows symbolic links. False if the directory could not be read. */
typedef bool (*DFsEach)(void *opaque, const char *name, bool is_directory);
bool d_fs_list(const char *directory, DFsEach each, void *opaque);

#endif
