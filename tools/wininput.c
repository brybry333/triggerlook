/* wininput.exe <steps file> <info file>: test helper (Wine/Proton or Windows). Waits until the info file has a line
 * "mark go" (triggerlook_ui mark go), then sends real mouse / keyboard input to the game window (SendInput), so the
 * menu is driven the way a player does it. Steps: "<seconds after the mark> <action> args", coordinates in the
 * game window's client area:  click x y | shiftclick x y | drag x1 y1 x2 y2 | key esc
 * Build: x86_64-w64-mingw32-gcc -municode -O2 -o build/wininput.exe tools/wininput.c -static -luser32 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static HWND g_game;

static BOOL CALLBACK find_game(HWND hwnd, LPARAM lp)
{
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!IsWindowVisible(hwnd)) return TRUE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t exe[MAX_PATH] = L"";
    DWORD n = MAX_PATH;
    if (p) { QueryFullProcessImageNameW(p, 0, exe, &n); CloseHandle(p); }
    if (wcsstr(exe, L"cstrike_win64.exe")) { g_game = hwnd; return FALSE; }
    return TRUE;
}

static void move_to(int x, int y)
{
    POINT pt = { x, y };
    ClientToScreen(g_game, &pt);
    SetCursorPos(pt.x, pt.y);
    INPUT in = { INPUT_MOUSE };
    in.mi.dx = (LONG)((pt.x * 65535.0) / (GetSystemMetrics(SM_CXSCREEN) - 1));
    in.mi.dy = (LONG)((pt.y * 65535.0) / (GetSystemMetrics(SM_CYSCREEN) - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(in));
}

static void button(BOOL down)
{
    INPUT in = { INPUT_MOUSE };
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

static void key(WORD vk, BOOL down)
{
    INPUT in = { INPUT_KEYBOARD };
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

static int marked(const wchar_t *info)
{
    FILE *f = _wfopen(info, L"r");
    char line[512];
    int found = 0;
    while (f && fgets(line, sizeof(line), f)) if (!strncmp(line, "mark go", 7)) found = 1;
    if (f) fclose(f);
    return found;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { fwprintf(stderr, L"usage: wininput <steps> <info file>\n"); return 2; }
    for (int i = 0; i < 1200 && !marked(argv[2]); i++) Sleep(100);
    if (!marked(argv[2])) { fwprintf(stderr, L"wininput: no mark\n"); return 1; }
    DWORD t0 = GetTickCount();
    EnumWindows(find_game, 0);
    if (!g_game) { fwprintf(stderr, L"wininput: no game window\n"); return 1; }
    FILE *f = _wfopen(argv[1], L"r");
    char line[512];
    while (f && fgets(line, sizeof(line), f)) {
        float t; char act[32]; int a = 0, b = 0, c = 0, d = 0;
        if (line[0] == '#' || sscanf(line, "%f %31s %d %d %d %d", &t, act, &a, &b, &c, &d) < 2) continue;
        while (GetTickCount() - t0 < (DWORD)(t * 1000)) Sleep(10);
        SetForegroundWindow(g_game);
        if (!strcmp(act, "click") || !strcmp(act, "shiftclick")) {
            int shift = act[0] == 's';
            move_to(a, b); Sleep(60);
            if (shift) { key(VK_LSHIFT, TRUE); Sleep(60); }
            button(TRUE); Sleep(90); button(FALSE); Sleep(60);
            if (shift) key(VK_LSHIFT, FALSE);
        } else if (!strcmp(act, "drag")) {
            move_to(a, b); Sleep(60); button(TRUE); Sleep(60);
            for (int k = 1; k <= 15; k++) { move_to(a + (c - a) * k / 15, b + (d - b) * k / 15); Sleep(25); }
            Sleep(60); button(FALSE);
        } else if (!strcmp(act, "key")) {
            key(VK_ESCAPE, TRUE); Sleep(80); key(VK_ESCAPE, FALSE);
        }
        fprintf(stderr, "wininput: %.1f %s %d %d %d %d\n", t, act, a, b, c, d);
    }
    if (f) fclose(f);
    return 0;
}
