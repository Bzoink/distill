#include "win.h"

#include <locale.h>
#include <shellapi.h>

int distill_main(int argc, char **argv);

static int run(int argc, wchar_t **wide_argv)
{
    setlocale(LC_CTYPE, ".UTF-8");
    SetConsoleOutputCP(CP_UTF8);
    /* The shared shell uses HOME to find per-user settings and Downloads. */
    if (!getenv("HOME")) {
        const wchar_t *home = _wgetenv(L"USERPROFILE");
        if (home) {
            char *path = d_win_utf8(home);
            if (path) _putenv_s("HOME", path);
            free(path);
        }
    }
    char **argv = d_xcalloc((size_t)argc + 1, sizeof(*argv));
    int result = 2;
    for (int i = 0; i < argc; ++i) {
        argv[i] = d_win_utf8(wide_argv[i]);
        if (!argv[i]) {
            fputs("distill: invalid command-line Unicode\n", stderr);
            goto done;
        }
    }
    result = distill_main(argc, argv);
done:
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    return result;
}

#ifdef DISTILL_WINDOW_ENTRY
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, wchar_t *command, int show)
{
    (void)instance;
    (void)previous;
    (void)command;
    (void)show;
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    int result = run(argc, argv);
    LocalFree(argv);
    return result;
}
#else
int wmain(int argc, wchar_t **argv)
{
    return run(argc, argv);
}
#endif
