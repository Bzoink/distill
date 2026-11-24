#include "dproc.h"
#include "dfs.h"
#include "win.h"

#include <bcrypt.h>
#include <psapi.h>
#include <shellapi.h>

typedef struct {
    HANDLE pipe;
    OVERLAPPED read;
    unsigned char buffer[8192];
    DWORD length;
    DWORD offset;
    bool pending;
    bool eof;
    bool failed;
} PipeReader;

/* DProc and its stream IDs are owned by the shell's event-loop thread.
   Use indexes because Win32 handles are pointer-sized. */
static PipeReader *readers[64];

struct DProc {
    HANDLE process;
    int output, errors;
    bool ended;
    DProcStatus status;
};

static void issue_read(PipeReader *reader)
{
    reader->offset = reader->length = 0;
    ResetEvent(reader->read.hEvent);
    if (ReadFile(reader->pipe, reader->buffer, sizeof(reader->buffer),
                 &reader->length, &reader->read)) {
        reader->eof = reader->length == 0;
    } else {
        DWORD status = GetLastError();
        if (status == ERROR_IO_PENDING) { reader->pending = true; return; }
        reader->eof = status == ERROR_BROKEN_PIPE || status == ERROR_PIPE_NOT_CONNECTED;
        reader->failed = !reader->eof;
    }
    SetEvent(reader->read.hEvent);
}

static void finish_read(PipeReader *reader)
{
    if (!reader->pending) return;
    if (GetOverlappedResult(reader->pipe, &reader->read, &reader->length, FALSE)) {
        reader->pending = false;
        reader->eof = reader->length == 0;
    } else {
        DWORD status = GetLastError();
        if (status == ERROR_IO_INCOMPLETE) return;
        reader->pending = false;
        reader->eof = status == ERROR_BROKEN_PIPE || status == ERROR_PIPE_NOT_CONNECTED;
        reader->failed = !reader->eof;
    }
}

static void close_stream(int *slot)
{
    if (*slot < 0) return;
    PipeReader *reader = readers[*slot];
    readers[*slot] = NULL;
    *slot = -1;
    if (reader->pending) {
        DWORD unused;
        CancelIoEx(reader->pipe, &reader->read);
        GetOverlappedResult(reader->pipe, &reader->read, &unused, TRUE);
    }
    CloseHandle(reader->pipe);
    CloseHandle(reader->read.hEvent);
    free(reader);
}

static bool open_stream(int *slot, HANDLE *writer, SECURITY_ATTRIBUTES *inherited)
{
    int index = 0;
    while (index < (int)(sizeof(readers) / sizeof(*readers)) && readers[index]) ++index;
    if (index == (int)(sizeof(readers) / sizeof(*readers))) {
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return false;
    }
    uint64_t random;
    if (BCryptGenRandom(NULL, (unsigned char *)&random, sizeof(random),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        SetLastError(ERROR_GEN_FAILURE);
        return false;
    }
    wchar_t name[100];
    swprintf(name, sizeof(name) / sizeof(*name), L"\\\\.\\pipe\\distill-%lu-%016llx",
             (unsigned long)GetCurrentProcessId(), (unsigned long long)random);
    HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED |
                                 FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 8192, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE write_end = event ? CreateFileW(name, GENERIC_WRITE, 0, inherited, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    if (write_end == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (event) CloseHandle(event);
        CloseHandle(pipe);
        SetLastError(code);
        return false;
    }
    PipeReader *reader = d_xcalloc(1, sizeof(*reader));
    reader->pipe = pipe;
    reader->read.hEvent = event;
    readers[index] = reader;
    *slot = index;
    *writer = write_end;
    return true;
}

/* Quote every argument for the Windows C runtime. Backslashes immediately
   before quotes or a closing delimiter must be doubled. */
static void append_argument(DBuffer *line, const char *argument)
{
    dbuf_puts(line, "\"");
    for (const char *p = argument; ; ) {
        size_t slashes = 0;
        while (*p == '\\') { ++p; ++slashes; }
        if (!*p || *p == '"') slashes *= 2;
        while (slashes--) dbuf_puts(line, "\\");
        if (!*p) break;
        if (*p == '"') dbuf_puts(line, "\\");
        dbuf_append(line, p++, 1);
    }
    dbuf_puts(line, "\"");
}

DProc *d_proc_spawn(const char *const *argv, DProcStream output, DProcStream errors,
                    char **error)
{
    if (error) *error = NULL;
    DBuffer command;
    dbuf_init(&command);
    for (size_t i = 0; argv[i]; ++i) {
        if (i) dbuf_puts(&command, " ");
        append_argument(&command, argv[i]);
    }
    wchar_t *command_line = d_win_wide(command.data);
    dbuf_free(&command);
    if (!command_line) {
        if (error) *error = d_strdup("invalid process arguments");
        return NULL;
    }

    int out_slot = -1, err_slot = -1;
    HANDLE out = NULL, err = NULL;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE null_device = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    PROCESS_INFORMATION info;
    bool started = null_device != INVALID_HANDLE_VALUE &&
                   (output != D_PROC_PIPE || open_stream(&out_slot, &out, &inherit)) &&
                   (errors != D_PROC_PIPE || open_stream(&err_slot, &err, &inherit));
    if (started) {
        STARTUPINFOW startup = {0};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = null_device;
        startup.hStdOutput = out ? out : null_device;
        startup.hStdError = err ? err : errors == D_PROC_WITH_OUTPUT && out ? out : null_device;
        started = CreateProcessW(NULL, command_line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                 NULL, NULL, &startup, &info);
    }
    DWORD status = GetLastError();
    if (null_device != INVALID_HANDLE_VALUE) CloseHandle(null_device);
    if (out) CloseHandle(out);
    if (err) CloseHandle(err);
    free(command_line);
    if (!started) {
        close_stream(&out_slot);
        close_stream(&err_slot);
        d_win_error(error, argv[0], status);
        return NULL;
    }
    CloseHandle(info.hThread);
    DProc *proc = d_xcalloc(1, sizeof(*proc));
    proc->process = info.hProcess;
    proc->output = out_slot;
    proc->errors = err_slot;
    if (out_slot >= 0) issue_read(readers[out_slot]);
    if (err_slot >= 0) issue_read(readers[err_slot]);
    return proc;
}

int d_proc_output(const DProc *proc) { return proc ? proc->output : -1; }
int d_proc_errors(const DProc *proc) { return proc ? proc->errors : -1; }
void d_proc_close_output(DProc *proc) { if (proc) close_stream(&proc->output); }
void d_proc_close_errors(DProc *proc) { if (proc) close_stream(&proc->errors); }

static void record_exit(DProc *proc)
{
    DWORD code;
    bool exited = GetExitCodeProcess(proc->process, &code) && code < 0xc0000000u;
    proc->status = (DProcStatus){.exited = exited, .code = exited ? (int)code : -1};
    proc->ended = true;
}

bool d_proc_poll_exit(DProc *proc, DProcStatus *status)
{
    if (!proc) return false;
    if (!proc->ended) {
        if (WaitForSingleObject(proc->process, 0) != WAIT_OBJECT_0) return false;
        record_exit(proc);
    }
    if (status) *status = proc->status;
    return true;
}

DProcStatus d_proc_wait(DProc *proc)
{
    if (!proc->ended) {
        WaitForSingleObject(proc->process, INFINITE);
        record_exit(proc);
    }
    return proc->status;
}

void d_proc_terminate(DProc *proc)
{
    if (!proc || d_proc_poll_exit(proc, NULL)) return;
    TerminateProcess(proc->process, 0xc000013au);
    d_proc_wait(proc);
}

void d_proc_free(DProc *proc)
{
    if (!proc) return;
    d_proc_close_output(proc);
    d_proc_close_errors(proc);
    if (proc->process) CloseHandle(proc->process);
    free(proc);
}

DProc *d_proc_open_with_system(const char *uri)
{
    wchar_t *target = d_win_wide(uri);
    if (!target) return NULL;
    SHELLEXECUTEINFOW open = {.cbSize = sizeof(open), .fMask = SEE_MASK_NOCLOSEPROCESS |
                             SEE_MASK_FLAG_NO_UI, .lpVerb = L"open", .lpFile = target,
                             .nShow = SW_SHOWNORMAL};
    bool ok = ShellExecuteExW(&open);
    free(target);
    if (!ok) return NULL;
    DProc *proc = d_xcalloc(1, sizeof(*proc));
    proc->output = proc->errors = -1;
    proc->process = open.hProcess;
    proc->ended = !open.hProcess;
    if (proc->ended) proc->status = (DProcStatus){.exited = true, .code = 0};
    return proc;
}

static PipeReader *find_reader(int slot)
{
    return slot >= 0 && slot < (int)(sizeof(readers) / sizeof(*readers)) ? readers[slot] : NULL;
}

long d_handle_read(int slot, void *buffer, size_t size)
{
    PipeReader *reader = find_reader(slot);
    if (!reader) return D_READ_FAILED;
    finish_read(reader);
    if (reader->pending) return D_READ_EMPTY;
    if (reader->failed) return D_READ_FAILED;
    if (reader->eof) return 0;
    size_t available = reader->length - reader->offset;
    if (size > available) size = available;
    memcpy(buffer, reader->buffer + reader->offset, size);
    reader->offset += (DWORD)size;
    if (reader->offset == reader->length) issue_read(reader);
    return (long)size;
}

int d_poll(DPollItem *items, size_t count, int timeout_ms)
{
    HANDLE handles[MAXIMUM_WAIT_OBJECTS - 1];
    DWORD used = 0;
    if (count > MAXIMUM_WAIT_OBJECTS - 1) return -1;
    for (size_t i = 0; i < count; ++i) {
        items[i].ready = false;
        PipeReader *reader = find_reader(items[i].handle);
        if (!reader) continue;
        DWORD j = 0;
        while (j < used && handles[j] != reader->read.hEvent) ++j;
        if (j == used) handles[used++] = reader->read.hEvent;
    }
    DWORD wait = MsgWaitForMultipleObjectsEx(used, handles, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms,
                                             QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_FAILED) return -1;
    if (wait == WAIT_TIMEOUT) return 0;
    int ready = 0;
    for (size_t i = 0; i < count; ++i) {
        PipeReader *reader = find_reader(items[i].handle);
        items[i].ready = reader && WaitForSingleObject(reader->read.hEvent, 0) == WAIT_OBJECT_0;
        ready += items[i].ready;
    }
    return ready;
}

size_t d_proc_on_fatal_signal(void (*restore)(void))
{
    (void)restore;
    return 0;
}

uintptr_t d_win_stack_top(void)
{
    return (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
}

size_t d_proc_stack_limit(void)
{
    /* StackBase is the top of the stack, and VirtualQuery gives the lower
       edge of its reservation even before the guard page grows it. */
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery(&region, &region, sizeof(region))) return 0;
    return d_win_stack_top() - (uintptr_t)region.AllocationBase;
}

char *d_proc_executable_directory(void)
{
    wchar_t path[D_FS_PATH_MAX];
    DWORD count = GetModuleFileNameW(NULL, path, D_FS_PATH_MAX);
    if (!count || count >= D_FS_PATH_MAX) return NULL;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return NULL;
    *slash = L'\0';
    return d_win_path(path);
}

bool d_proc_set_environment(const char *name, const char *value)
{
    wchar_t *key = d_win_wide(name);
    wchar_t *text = d_win_wide(value);
    bool ok = key && text && _wputenv_s(key, text) == 0;
    free(key);
    free(text);
    return ok;
}

long d_proc_id(void) { return (long)GetCurrentProcessId(); }

long d_proc_peak_rss_kib(void)
{
    PROCESS_MEMORY_COUNTERS memory;
    return GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))
         ? (long)(memory.PeakWorkingSetSize / 1024) : -1;
}
