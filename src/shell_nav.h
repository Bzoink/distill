#ifndef DISTILL_SHELL_NAV_H
#define DISTILL_SHELL_NAV_H

/* One navigation: the engine run once, as a child, to write a reader page.

   What every shell without a toolkit does around that is the same -- write
   the request to a private file rather than argv, start the engine, read its
   progress lines as they come, keep what it says on standard error for the
   message a failure shows, and tell a page from a challenge from a failure
   once it exits -- so the terminal and framebuffer shells share this, and
   wait on its handles in their own loops. */

#include "cutil.h"
#include "dproc.h"

typedef enum { D_NAV_NEW, D_NAV_RELOAD } DNavKind;

typedef struct {
    const char *engine;        /* the engine binary */
    const char *theme;
    int zoom;
    const char *session_file;  /* NULL for none */
    int width;                 /* 0 to leave the engine's default */
    bool native_forms;         /* the shell draws form controls itself */
} DNavOptions;

typedef struct {
    bool active;
    DProc *engine;
    DBuffer pending;           /* progress not yet read as whole lines */
    DBuffer diagnostics;       /* standard error, bounded */
    char *url;
    char *template_path;
    char *body_path;
    char *request_path;
    char *location;            /* where the engine says the page came from */
    int percent;
    char phase[16];
    DNavKind kind;
} DShellNav;

typedef enum {
    D_NAV_RUNNING,
    D_NAV_LOADED,              /* a reader page, at d_nav_final_url */
    D_NAV_CHALLENGE,           /* the page is a recognised bot challenge, shown as sent */
    D_NAV_FAILED,              /* d_nav_failure says why */
} DNavOutcome;

/* Start the engine on `url`. A request body (which may hold a password) goes
   to the engine in a private file. False with `*error` set (caller frees). */
bool d_nav_start(DShellNav *nav, const DNavOptions *options, const char *url,
                 const char *method, const char *body, bool https_guessed, DNavKind kind,
                 char **error);

/* The handles to wait on while it runs, up to `capacity`. */
size_t d_nav_handles(const DShellNav *nav, int *handles, size_t capacity);

/* Read what the engine has said. RUNNING until it has exited; then the
   outcome, and the caller takes the page or the reason before cleaning up. */
DNavOutcome d_nav_service(DShellNav *nav);

/* The page's own address once loaded: the engine's final location when it
   is one a page may have, else the address asked for. */
const char *d_nav_final_url(const DShellNav *nav);

/* The written reader page's path, now the caller's to remove. */
char *d_nav_take_template(DShellNav *nav);

/* Why it failed, in the engine's last words. Caller frees. */
char *d_nav_failure(const DShellNav *nav);

/* Stop a running navigation and remove what it wrote. */
void d_nav_cancel(DShellNav *nav);

/* Release everything once the outcome has been read. */
void d_nav_cleanup(DShellNav *nav);

#endif
