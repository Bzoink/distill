/* dfs on Linux: POSIX file calls. */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "dfs.h"

#include "cutil.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

size_t d_fs_path_limit(void)
{
    return PATH_MAX < D_FS_PATH_MAX ? PATH_MAX : D_FS_PATH_MAX;
}

bool d_fs_exists(const char *path)
{
    struct stat info;
    return path && stat(path, &info) == 0;
}

bool d_fs_readable(const char *path)
{
    return path && access(path, R_OK) == 0;
}

bool d_fs_executable(const char *path)
{
    return path && access(path, X_OK) == 0;
}

bool d_fs_is_directory(const char *path)
{
    struct stat info;
    return path && stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

char *d_fs_absolute(const char *path)
{
    return path ? realpath(path, NULL) : NULL;
}

char *d_fs_current_directory(void)
{
    char buffer[D_FS_PATH_MAX];
    return getcwd(buffer, sizeof(buffer)) ? d_strdup(buffer) : NULL;
}

bool d_fs_remove(const char *path)
{
    return path && unlink(path) == 0;
}

bool d_fs_remove_directory(const char *path)
{
    return path && rmdir(path) == 0;
}

bool d_fs_make_directories(const char *path)
{
    if (!path || !*path) return false;
    char *copy = d_strdup(path);
    for (char *at = copy + 1; *at; ++at) {
        if (*at != '/') continue;
        *at = '\0';
        if (mkdir(copy, 0700) != 0 && errno != EEXIST) { free(copy); return false; }
        *at = '/';
    }
    bool ok = mkdir(copy, 0700) == 0 || errno == EEXIST;
    free(copy);
    return ok;
}

char *d_fs_temp_directory(void)
{
    const char *directory = getenv("TMPDIR");
    return d_strdup(directory && *directory ? directory : "/tmp");
}

static char *unique_template(const char *directory, const char *prefix)
{
    char *base = directory ? d_strdup(directory) : d_fs_temp_directory();
    char *path = d_cat3(base, "/", prefix ? prefix : "distill");
    free(base);
    char *template = d_cat2(path, "-XXXXXX");
    free(path);
    return template;
}

char *d_fs_private_file(const char *directory, const char *prefix)
{
    char *path = unique_template(directory, prefix);
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    /* mkstemp's mode is 0600 already; say so rather than rely on it. */
    fchmod(fd, S_IRUSR | S_IWUSR);
    close(fd);
    return path;
}

char *d_fs_private_directory(const char *directory, const char *prefix)
{
    char *path = unique_template(directory, prefix);
    if (!mkdtemp(path)) { free(path); return NULL; }
    return path;
}

DFsPrivate d_fs_ensure_private(const char *path, unsigned *mode)
{
    struct stat info;
    if (mode) *mode = 0;
    if (stat(path, &info) == 0) {
        if (mode) *mode = (unsigned)(info.st_mode & 07777);
        if (!S_ISREG(info.st_mode)) return D_FS_PRIVATE_NOT_A_FILE;
        if (info.st_mode & (S_IRWXG | S_IRWXO)) return D_FS_PRIVATE_SHARED;
        return D_FS_PRIVATE_OK;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) return D_FS_PRIVATE_FAILED;
    close(fd);
    if (mode) *mode = S_IRUSR | S_IWUSR;
    return D_FS_PRIVATE_CREATED;
}

static bool write_all(int fd, const void *data, size_t length)
{
    const char *at = data;
    while (length) {
        ssize_t written = write(fd, at, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        at += written;
        length -= (size_t)written;
    }
    return true;
}

bool d_fs_overwrite(const char *path, const void *data, size_t length)
{
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = write_all(fd, data, length);
    ok = close(fd) == 0 && ok;
    return ok;
}

bool d_fs_replace(const char *path, const void *data, size_t length)
{
    char *temporary = d_cat2(path, ".XXXXXX");
    int fd = mkstemp(temporary);
    if (fd < 0) { free(temporary); return false; }
    bool ok = write_all(fd, data, length);
    ok = ok && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    ok = ok && rename(temporary, path) == 0;
    if (!ok) unlink(temporary);
    free(temporary);
    return ok;
}

bool d_fs_append(const char *path, const void *data, size_t length)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    /* One write: O_APPEND makes each write atomic with respect to other
       appenders, but only as a whole. */
    ssize_t written = write(fd, data, length);
    bool ok = written >= 0 && (size_t)written == length;
    ok = close(fd) == 0 && ok;
    return ok;
}

bool d_fs_list(const char *directory, DFsEach each, void *opaque)
{
    DIR *handle = opendir(directory);
    if (!handle) return false;
    struct dirent *entry;
    while ((entry = readdir(handle))) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char *path = d_cat3(directory, "/", name);
        bool is_directory = d_fs_is_directory(path);
        free(path);
        if (!each(opaque, name, is_directory)) break;
    }
    closedir(handle);
    return true;
}
