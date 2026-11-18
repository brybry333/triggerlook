/* libtriggerlook.so — LD_PRELOAD loader for the triggerlook client mod (CS:S, Linux x64).
 *
 * css-launch preloads it into everything it starts, so it has no dependencies of its own. In the game, right
 * after the engine loaded client.so, it loads triggerlook.so (next to this file) and calls TriggerLook_Init on
 * that same (main) thread. No game code is touched. (Windows: bhoptools.exe's code loads triggerlook.dll.)
 *
 * Log: ~/.cache/triggerlook/loader.log (the game's runtime container keeps ~/.cache writable).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define EXPORT __attribute__((visibility("default")))

/* loadable on the Steam runtime's glibc 2.31 when built against a newer one: the old dl* symbol versions */
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dladdr,dladdr@GLIBC_2.2.5");
__asm__(".symver dlerror,dlerror@GLIBC_2.2.5");
#define MOD_LIB "triggerlook.so"

static void *(*real_dlopen)(const char *, int);
static int g_done;      /* 1 once we loaded the mod or gave up */
static int g_busy;      /* re-entrancy guard: loading the mod calls dlopen */
static FILE *g_log;

static void logmsg(const char *fmt, ...)
{
    if (!g_log)
    {
        const char *home = getenv("HOME");
        char path[4096];
        if (!home) return;
        snprintf(path, sizeof(path), "%s/.cache", home);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/.cache/triggerlook", home);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/.cache/triggerlook/loader.log", home);
        g_log = fopen(path, "w");
        if (!g_log) return;
        setvbuf(g_log, NULL, _IOLBF, 0);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(g_log, "[%5ld.%03ld] ", (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
}

static void load_mod(void)
{
    g_done = 1;
    if (dlsym(RTLD_DEFAULT, "TriggerLook_Init")) { logmsg("already loaded (bhoptools?), leaving it"); return; }
    Dl_info me;
    char path[4096];
    if (!dladdr((void *)load_mod, &me) || !me.dli_fname) { logmsg("dladdr failed - giving up"); return; }
    snprintf(path, sizeof(path), "%s", me.dli_fname);
    char *slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "%s", MOD_LIB);
    g_busy = 1;
    /* NODELETE: never unloaded (its panels, commands and renderable live until the game exits) */
    /* GLOBAL: bhoptools.so (which can load it too) sees it is here already */
    void *lib = real_dlopen(path, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    g_busy = 0;
    if (!lib) { logmsg("can't load %s: %s", path, dlerror()); return; }
    int (*init)(void) = (int (*)(void))dlsym(lib, "TriggerLook_Init");
    logmsg("loaded %s, TriggerLook_Init %s", path, init ? (init() ? "OK" : "FAILED") : "not found");
}

EXPORT void *dlopen(const char *file, int mode)
{
    if (!real_dlopen) real_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, "dlopen");
    void *h = real_dlopen(file, mode);
    if (file && !g_busy && !g_done && h)
    {
        const char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        /* the engine loads client.so after tier0 / vstdlib / the material system are up */
        if (!strcmp(base, "client.so")) load_mod();
    }
    return h;
}

__attribute__((constructor)) static void loader_init(void)
{
    if (!strcmp(program_invocation_short_name, "cstrike_linux64")) logmsg("triggerlook loader in %s", program_invocation_short_name);
}
