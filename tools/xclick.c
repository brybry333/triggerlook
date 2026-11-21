// xclick <steps> <info file>: like wininput.exe but real X11 events (XTest) on $DISPLAY (gamescope's XWayland).
// Waits for "mark go" in the info file; steps "<s after mark> click|shiftclick x y | key esc".
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int marked(const char *p) { char l[512]; int f = 0; FILE *fp = fopen(p, "r"); while (fp && fgets(l, sizeof l, fp)) if (!strncmp(l, "mark go", 7)) f = 1; if (fp) fclose(fp); return f; }
int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xclick: no display\n"); return 1; }
    for (int i = 0; i < 1200 && !marked(argv[2]); i++) usleep(100000);
    double t0 = now();
    FILE *f = fopen(argv[1], "r"); char line[256];
    KeyCode shift = XKeysymToKeycode(d, XK_Shift_L), esc = XKeysymToKeycode(d, XK_Escape);
    while (f && fgets(line, sizeof line, f)) {
        float t; char act[32], rest[200] = ""; int a = 0, b = 0, off = 0;
        if (line[0] == '#' || sscanf(line, "%f %31s %n", &t, act, &off) < 2) continue;
        sscanf(line + off, "%d %d", &a, &b);
        snprintf(rest, sizeof rest, "%s", line + off); rest[strcspn(rest, "\r\n")] = 0;
        while (now() - t0 < t) usleep(10000);
        if (!strcmp(act, "dclick")) {
            XTestFakeMotionEvent(d, -1, a, b, 0); XFlush(d); usleep(150000);
            for (int k = 0; k < 2; k++) { XTestFakeButtonEvent(d, 1, True, 0); XFlush(d); usleep(50000); XTestFakeButtonEvent(d, 1, False, 0); XFlush(d); usleep(90000); }
        } else if (!strcmp(act, "type")) {
            for (char *c = rest; *c; c++) {
                KeySym ks = (unsigned char)*c; KeyCode kc = XKeysymToKeycode(d, ks);
                int sh = XkbKeycodeToKeysym(d, kc, 0, 0) != ks;
                if (sh) { XTestFakeKeyEvent(d, shift, True, 0); XFlush(d); usleep(15000); }
                XTestFakeKeyEvent(d, kc, True, 0); XFlush(d); usleep(30000); XTestFakeKeyEvent(d, kc, False, 0); XFlush(d); usleep(30000);
                if (sh) { XTestFakeKeyEvent(d, shift, False, 0); XFlush(d); usleep(15000); }
            }
        } else if (!strcmp(act, "keyname")) {
            KeyCode kc = XKeysymToKeycode(d, XStringToKeysym(rest));
            XTestFakeKeyEvent(d, kc, True, 0); XFlush(d); usleep(80000); XTestFakeKeyEvent(d, kc, False, 0); XFlush(d);
        }
        if (!strcmp(act, "click") || !strcmp(act, "shiftclick")) {
            XTestFakeMotionEvent(d, -1, a, b, 0); XFlush(d); usleep(150000);
            if (act[0] == 's') { XTestFakeKeyEvent(d, shift, True, 0); XFlush(d); usleep(60000); }
            XTestFakeButtonEvent(d, 1, True, 0); XFlush(d); usleep(90000);
            XTestFakeButtonEvent(d, 1, False, 0); XFlush(d); usleep(60000);
            if (act[0] == 's') { XTestFakeKeyEvent(d, shift, False, 0); XFlush(d); }
        } else if (!strcmp(act, "key")) {
            XTestFakeKeyEvent(d, esc, True, 0); XFlush(d); usleep(80000); XTestFakeKeyEvent(d, esc, False, 0); XFlush(d);
        }
        fprintf(stderr, "xclick: %.1f %s %s\n", t, act, rest);
    }
    XCloseDisplay(d);
    return 0;
}
