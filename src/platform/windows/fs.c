#include "dfs.h"
#include "win.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

size_t d_fs_path_limit(void)
{
    return D_FS_PATH_MAX;
}

static DWORD attributes(const char *path)
{
    wchar_t *name = d_win_wide(path);
    DWORD result = name ? GetFileAttributesW(name) : INVALID_FILE_ATTRIBUTES;
    free(name);
    return result;
}

bool d_fs_exists(const char *path)
{
    return attributes(path) != INVALID_FILE_ATTRIBUTES;
}

bool d_fs_is_directory(const char *path)
{
    DWORD value = attributes(path);
    return value != INVALID_FILE_ATTRIBUTES && (value & FILE_ATTRIBUTE_DIRECTORY);
}

bool d_fs_readable(const char *path)
{
    wchar_t *name = d_win_wide(path);
    HANDLE file = name ? CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, NULL) : INVALID_HANDLE_VALUE;
    free(name);
    return file != INVALID_HANDLE_VALUE && CloseHandle(file);
}

bool d_fs_executable(const char *path)
{
    return d_fs_readable(path) && !d_fs_is_directory(path);
}

char *d_fs_absolute(const char *path)
{
    if (!d_fs_exists(path)) return NULL;
    wchar_t *name = d_win_wide(path);
    if (!name) return NULL;
    wchar_t result[D_FS_PATH_MAX];
    DWORD count = GetFullPathNameW(name, D_FS_PATH_MAX, result, NULL);
    free(name);
    return count && count < D_FS_PATH_MAX ? d_win_path(result) : NULL;
}

char *d_fs_current_directory(void)
{
    wchar_t result[D_FS_PATH_MAX];
    DWORD count = GetCurrentDirectoryW(D_FS_PATH_MAX, result);
    return count && count < D_FS_PATH_MAX ? d_win_path(result) : NULL;
}

bool d_fs_remove(const char *path)
{
    wchar_t *name = d_win_wide(path);
    bool ok = name && DeleteFileW(name);
    free(name);
    return ok;
}

bool d_fs_remove_directory(const char *path)
{
    wchar_t *name = d_win_wide(path);
    bool ok = name && RemoveDirectoryW(name);
    free(name);
    return ok;
}

char *d_fs_temp_directory(void)
{
    const char *configured = getenv("TMPDIR");
    if (configured && *configured) return d_strdup(configured);
    wchar_t result[D_FS_PATH_MAX];
    DWORD count = GetTempPathW(D_FS_PATH_MAX, result);
    if (!count || count >= D_FS_PATH_MAX) return NULL;
    while (count > 3 && (result[count - 1] == L'\\' || result[count - 1] == L'/'))
        result[--count] = L'\0';
    return d_win_path(result);
}

static TOKEN_USER *current_user(void)
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    DWORD count = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &count);
    TOKEN_USER *user = count ? d_xcalloc(count, 1) : NULL;
    bool ok = user && GetTokenInformation(token, TokenUser, user, count, &count);
    CloseHandle(token);
    if (ok) return user;
    free(user);
    return NULL;
}

/* Private files and directories grant access to the user and Local System. */
static PSECURITY_DESCRIPTOR private_descriptor(void)
{
    TOKEN_USER *user = current_user();
    if (!user) return NULL;
    wchar_t *sid = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (ConvertSidToStringSidW(user->User.Sid, &sid)) {
        wchar_t sddl[256];
        int count = swprintf(sddl, sizeof(sddl) / sizeof(*sddl),
                             L"D:P(A;OICI;FA;;;%ls)(A;OICI;FA;;;SY)", sid);
        if (count > 0)
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                                  &descriptor, NULL);
        LocalFree(sid);
    }
    free(user);
    return descriptor;
}

bool d_fs_make_directories(const char *path)
{
    if (!path || !*path) return false;
    if (d_fs_is_directory(path)) return true;
    wchar_t *name = d_win_wide(path);
    if (!name) return false;
    for (wchar_t *p = name; *p; ++p) if (*p == L'/') *p = L'\\';
    PSECURITY_DESCRIPTOR descriptor = private_descriptor();
    SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
    /* Resolve drive and UNC roots with GetFullPathNameW, then create from the
       first component after the root. */
    wchar_t full[D_FS_PATH_MAX];
    DWORD length = GetFullPathNameW(name, D_FS_PATH_MAX, full, NULL);
    free(name);
    bool ok = descriptor && length && length < D_FS_PATH_MAX;
    if (ok) {
        wchar_t *first = full + 3;
        if (full[0] == L'\\' && full[1] == L'\\') {
            first = wcschr(full + 2, L'\\');
            if (first) first = wcschr(first + 1, L'\\');
            first = first ? first + 1 : full + length;
        }
        while (first < full + length && *first) {
            wchar_t *end = wcschr(first, L'\\');
            if (end) *end = L'\0';
            if (!CreateDirectoryW(full, &security)) {
                DWORD value = GetFileAttributesW(full);
                if (value == INVALID_FILE_ATTRIBUTES || !(value & FILE_ATTRIBUTE_DIRECTORY))
                    ok = false;
            }
            if (!end || !ok) break;
            *end = L'\\';
            first = end + 1;
        }
    }
    if (descriptor) LocalFree(descriptor);
    return ok;
}

static HANDLE create_private(const wchar_t *path, DWORD access, DWORD creation)
{
    PSECURITY_DESCRIPTOR descriptor = private_descriptor();
    if (!descriptor) return INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
    HANDLE file = CreateFileW(path, access, FILE_SHARE_READ, &security, creation,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD error = GetLastError();
    LocalFree(descriptor);
    SetLastError(error);
    return file;
}

static char *unique_path(const char *prefix)
{
    uint64_t number;
    if (BCryptGenRandom(NULL, (unsigned char *)&number, sizeof(number),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return NULL;
    char suffix[20];
    snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)number);
    return d_cat2(prefix, suffix);
}

static char *unique_private(const char *prefix, bool directory)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        char *path = unique_path(prefix);
        wchar_t *name = d_win_wide(path);
        if (!name) { free(path); return NULL; }
        bool made;
        if (directory) {
            PSECURITY_DESCRIPTOR descriptor = private_descriptor();
            SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
            made = descriptor && CreateDirectoryW(name, &security);
            DWORD error = GetLastError();
            if (descriptor) LocalFree(descriptor);
            SetLastError(error);
        } else {
            HANDLE file = create_private(name, GENERIC_WRITE, CREATE_NEW);
            made = file != INVALID_HANDLE_VALUE;
            if (made) CloseHandle(file);
        }
        DWORD error = GetLastError();
        free(name);
        if (made) return path;
        free(path);
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return NULL;
    }
    return NULL;
}

static char *private_path(const char *directory, const char *prefix, bool make_directory)
{
    char *base = directory ? d_strdup(directory) : d_fs_temp_directory();
    if (!base) return NULL;
    char *stem = d_cat3(base, "/", prefix ? prefix : "distill");
    char *path = unique_private(stem, make_directory);
    free(stem);
    free(base);
    return path;
}

char *d_fs_private_file(const char *directory, const char *prefix)
{
    return private_path(directory, prefix, false);
}

char *d_fs_private_directory(const char *directory, const char *prefix)
{
    return private_path(directory, prefix, true);
}

DFsPrivate d_fs_ensure_private(const char *path, unsigned *mode)
{
    if (mode) *mode = 0;
    wchar_t *name = d_win_wide(path);
    if (!name) return D_FS_PRIVATE_FAILED;
    HANDLE file = CreateFileW(name, READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT |
                              FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
        file = create_private(name, GENERIC_WRITE, CREATE_NEW);
        free(name);
        if (file == INVALID_HANDLE_VALUE) return D_FS_PRIVATE_FAILED;
        CloseHandle(file);
        return D_FS_PRIVATE_CREATED;
    }
    free(name);
    if (file == INVALID_HANDLE_VALUE) return D_FS_PRIVATE_FAILED;
    BY_HANDLE_FILE_INFORMATION info;
    DFsPrivate answer = D_FS_PRIVATE_FAILED;
    if (GetFileInformationByHandle(file, &info)) {
        if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            answer = D_FS_PRIVATE_NOT_A_FILE;
        } else {
            PACL acl = NULL;
            PSID owner = NULL;
            PSECURITY_DESCRIPTOR descriptor = NULL;
            TOKEN_USER *user = current_user();
            DWORD result = GetSecurityInfo(file, SE_FILE_OBJECT,
                                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                           &owner, NULL, &acl, NULL, &descriptor);
            if (result == ERROR_SUCCESS && user) {
                answer = acl && owner && EqualSid(owner, user->User.Sid)
                       ? D_FS_PRIVATE_OK : D_FS_PRIVATE_SHARED;
                for (DWORD i = 0; answer == D_FS_PRIVATE_OK && i < acl->AceCount; ++i) {
                    ACE_HEADER *header = NULL;
                    if (!GetAce(acl, i, (void **)&header)) { answer = D_FS_PRIVATE_FAILED; break; }
                    if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
                    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                        PSID who = &((ACCESS_ALLOWED_ACE *)header)->SidStart;
                        if (EqualSid(who, user->User.Sid) || IsWellKnownSid(who, WinLocalSystemSid))
                            continue;
                    }
                    answer = D_FS_PRIVATE_SHARED;
                }
            }
            if (descriptor) LocalFree(descriptor);
            free(user);
        }
    }
    CloseHandle(file);
    return answer;
}

static bool write_all(HANDLE file, const void *data, size_t length)
{
    const unsigned char *at = data;
    while (length) {
        DWORD done;
        DWORD batch = length > 1048576 ? 1048576 : (DWORD)length;
        if (!WriteFile(file, at, batch, &done, NULL) || !done) return false;
        at += done;
        length -= done;
    }
    return true;
}

bool d_fs_overwrite(const char *path, const void *data, size_t length)
{
    wchar_t *name = d_win_wide(path);
    if (!name) return false;
    HANDLE file = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ, NULL, TRUNCATE_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    free(name);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = write_all(file, data, length);
    return CloseHandle(file) && ok;
}

bool d_fs_replace(const char *path, const void *data, size_t length)
{
    wchar_t *name = d_win_wide(path);
    if (!name) return false;
    char *temporary = unique_private(path, false);
    wchar_t *stage = d_win_wide(temporary);
    bool ok = false;
    if (stage) {
        HANDLE file = CreateFileW(stage, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            ok = write_all(file, data, length) && FlushFileBuffers(file);
            ok = CloseHandle(file) && ok;
            if (ok) ok = MoveFileExW(stage, name, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        if (!ok) DeleteFileW(stage);
    }
    free(temporary);
    free(stage);
    free(name);
    return ok;
}

bool d_fs_append(const char *path, const void *data, size_t length)
{
    if (length > UINT32_MAX) return false;
    wchar_t *name = d_win_wide(path);
    if (!name) return false;
    HANDLE file = CreateFileW(name, FILE_APPEND_DATA, FILE_SHARE_WRITE | FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
        file = create_private(name, FILE_APPEND_DATA, CREATE_NEW);
        if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS)
            file = CreateFileW(name, FILE_APPEND_DATA, FILE_SHARE_WRITE | FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    free(name);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD done;
    bool ok = WriteFile(file, data, (DWORD)length, &done, NULL) && done == length;
    return CloseHandle(file) && ok;
}

bool d_fs_list(const char *directory, DFsEach each, void *opaque)
{
    if (!d_fs_is_directory(directory)) return false;
    char *pattern = d_cat2(directory, "/*");
    wchar_t *name = d_win_wide(pattern);
    free(pattern);
    if (!name) return false;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(name, &entry);
    DWORD error = GetLastError();
    free(name);
    if (find == INVALID_HANDLE_VALUE) return error == ERROR_FILE_NOT_FOUND;
    bool ok = true;
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        char *utf8 = d_win_utf8(entry.cFileName);
        if (!utf8) { ok = false; break; }
        bool more = each(opaque, utf8, (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        free(utf8);
        if (!more) { FindClose(find); return true; }
    } while (FindNextFileW(find, &entry));
    ok = ok && GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(find);
    return ok;
}

FILE *d_win_fopen(const char *path, const char *mode)
{
    wchar_t *name = d_win_wide(path);
    wchar_t *options = d_win_wide(mode);
    FILE *file = name && options ? _wfopen(name, options) : NULL;
    free(name);
    free(options);
    return file;
}
