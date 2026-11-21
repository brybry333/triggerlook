// triggerlook.exe: starts 64-bit CS:S with -insecure and loads triggerlook.dll into it (the trigger menu: triggers the
// server shows drawn in your own look; see README). Based on curtimefix.exe / CS:TV's CSTV.exe launcher (same author).
//
// It only loads the DLL into a game started with -insecure: loading code into a VAC-secured game is how you get banned.
//   - CS:S not running: starts it with -insecure (Steam's launch options for CS:S are used too), then loads the DLL.
//   - CS:S already running with -insecure (e.g. started by CSTV.exe): loads the DLL into it, so both mods work.
//   - CS:S already running without -insecure: refuses.
//
// triggerlook.exe [extra game arguments]. TRIGGERLOOK_GAME_DIR=<folder> uses that game folder and skips Steam.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GAME_EXE L"cstrike_win64.exe"
#define BIG 4096

static void fail(const wchar_t *msg)
{
    MessageBoxW(NULL, msg, L"triggerlook", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static BOOL exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static BOOL game_in(const wchar_t *dir)
{
    wchar_t p[BIG];
    swprintf(p, BIG, L"%ls\\" GAME_EXE, dir);
    return exists(p);
}

static BOOL reg_str(const wchar_t *key, const wchar_t *name, wchar_t *out, DWORD chars)
{
    DWORD size = chars * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, NULL, out, &size)) return FALSE;
    for (wchar_t *c = out; *c; c++) if (*c == L'/') *c = L'\\';
    return TRUE;
}

static DWORD active_user(void)
{
    DWORD v = 0, size = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD, NULL, &v, &size);
    return v;
}

// a whole file, NUL-terminated, or NULL
static char *slurp(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s) { size_t got = fread(s, 1, (size_t)n, f); s[got] = 0; }
    fclose(f);
    return s;
}

// the next "quoted" VDF string from p into out (\\ and \" unescaped); returns the text after it, or NULL
static const char *vdf_string(const char *p, char *out, size_t size)
{
    while (*p && *p != '"') p++;
    if (!*p) return NULL;
    size_t o = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        if (o + 1 < size) out[o++] = *p;
    }
    out[o] = 0;
    return *p ? p + 1 : NULL;
}

static BOOL find_game(const wchar_t *steam, wchar_t *dir)
{
    swprintf(dir, BIG, L"%ls\\steamapps\\common\\Counter-Strike Source", steam);
    if (game_in(dir)) return TRUE;
    wchar_t vdf[BIG];
    swprintf(vdf, BIG, L"%ls\\steamapps\\libraryfolders.vdf", steam);
    char *s = slurp(vdf);
    if (!s) return FALSE;
    BOOL found = FALSE;
    for (const char *p = s; !found && (p = strstr(p, "\"path\"")); ) {
        char path[BIG];
        p = vdf_string(p + 6, path, sizeof path);
        if (!p) break;
        wchar_t lib[BIG];
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, lib, BIG)) continue;
        swprintf(dir, BIG, L"%ls\\steamapps\\common\\Counter-Strike Source", lib);
        found = game_in(dir);
    }
    free(s);
    return found;
}

// Steam's launch options for CS:S, from userdata\<id>\config\localconfig.vdf: a "240" { ... } block that
// has "LaunchOptions". Empty if there are none.
static void launch_options(const wchar_t *steam, DWORD user, wchar_t *out)
{
    out[0] = 0;
    wchar_t path[BIG];
    swprintf(path, BIG, L"%ls\\userdata\\%lu\\config\\localconfig.vdf", steam, (unsigned long)user);
    char *s = slurp(path);
    if (!s) return;
    for (const char *p = s; (p = strstr(p, "\"240\"")); p += 5) {
        const char *b = p + 5;
        while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') b++;
        if (*b != '{') continue;
        int depth = 0;
        const char *e = b;
        do { if (*e == '{') depth++; else if (*e == '}') depth--; } while (*++e && depth);
        const char *lo = strstr(b, "\"LaunchOptions\"");
        if (lo && lo < e) {
            char v[BIG];
            if (vdf_string(lo + 15, v, sizeof v)) MultiByteToWideChar(CP_UTF8, 0, v, -1, out, BIG);
            break;
        }
    }
    free(s);
}

static DWORD game_running(void)   // its process id, or 0
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !pid; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, GAME_EXE) == 0) pid = pe.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

// the running game's command line, from its PEB (works on Windows and Wine/Proton alike)
typedef LONG (WINAPI *NtQIP_t)(HANDLE, int, void *, ULONG, ULONG *);
static BOOL command_line(HANDLE process, wchar_t *out, size_t chars)
{
    struct { void *r1; void *peb; void *r2[4]; } pbi;
    NtQIP_t q = (NtQIP_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!q || q(process, 0, &pbi, sizeof pbi, NULL) != 0 || !pbi.peb) return FALSE;
    void *params = NULL;
    if (!ReadProcessMemory(process, (char *)pbi.peb + 0x20, &params, sizeof params, NULL) || !params) return FALSE;
    struct { USHORT len, max; ULONG pad; wchar_t *buf; } cl;
    if (!ReadProcessMemory(process, (char *)params + 0x70, &cl, sizeof cl, NULL) || !cl.buf) return FALSE;
    size_t n = cl.len / sizeof(wchar_t);
    if (n >= chars) n = chars - 1;
    if (!ReadProcessMemory(process, cl.buf, out, n * sizeof(wchar_t), NULL)) return FALSE;
    out[n] = 0;
    return TRUE;
}

static BOOL has_insecure(const wchar_t *cmd)   // the engine matches its flags without case
{
    for (const wchar_t *p = cmd; *p; p++) {
        if (_wcsnicmp(p, L"-insecure", 9)) continue;
        wchar_t before = p == cmd ? L' ' : p[-1], after = p[9];
        // (not next to a quote: inside quotes it's part of another argument, the game stays secured)
        if ((before == L' ' || before == L'\t') && (after == 0 || after == L' ' || after == L'\t')) return TRUE;
    }
    return FALSE;
}

static BOOL module_loaded(DWORD pid, const wchar_t *name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    BOOL found = FALSE;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = _wcsicmp(me.szModule, name) == 0;
    CloseHandle(snap);
    return found;
}

static BOOL inject(HANDLE process, const wchar_t *dll)
{
    SIZE_T size = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return FALSE;
    BOOL ok = FALSE;
    if (WriteProcessMemory(process, remote, dll, size, NULL)) {
        // kernel32 is at the same address in every process
        FARPROC load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE t = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, NULL);
        if (t) {
            DWORD code = 0;
            if (WaitForSingleObject(t, 30000) == WAIT_OBJECT_0 && GetExitCodeThread(t, &code)) ok = code != 0;
            CloseHandle(t);
        }
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    (void)inst; (void)prev; (void)show;
    wchar_t here[BIG], dll[BIG], game[BIG], exe[BIG], steam[BIG], options[BIG] = L"";
    DWORD n = GetModuleFileNameW(NULL, here, BIG);
    if (!n || n >= BIG) fail(L"Couldn't find where triggerlook.exe is.");
    *wcsrchr(here, L'\\') = 0;
    swprintf(dll, BIG, L"%ls\\triggerlook.dll", here);
    if (!exists(dll)) fail(L"triggerlook.dll isn't next to triggerlook.exe. Keep the files from the zip together.");
    DWORD pid = game_running();
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, pid);
        static wchar_t running[BIG * 3];
        if (!h || !command_line(h, running, BIG * 3)) fail(L"Counter-Strike: Source is already running and its command line can't be read. Close it, then start triggerlook.exe.");
        if (!has_insecure(running)) fail(L"Counter-Strike: Source is already running without -insecure. Close it, then start triggerlook.exe (it starts the game with -insecure).");
        if (module_loaded(pid, L"triggerlook.dll")) fail(L"triggerlook is already loaded in the running game.");
        if (!inject(h, dll)) fail(L"Couldn't load triggerlook.dll into the game. Your antivirus may be blocking it.");
        CloseHandle(h);
        return 0;
    }

    if (GetEnvironmentVariableW(L"TRIGGERLOOK_GAME_DIR", game, BIG)) {
        if (!game_in(game)) fail(L"TRIGGERLOOK_GAME_DIR doesn't have " GAME_EXE L" in it.");
    } else {
        if (!reg_str(L"Software\\Valve\\Steam", L"SteamPath", steam, BIG)) fail(L"Steam doesn't seem to be installed.");
        if (!find_game(steam, game)) fail(L"Couldn't find Counter-Strike: Source in your Steam libraries.");
        if (!active_user()) {
            ShellExecuteW(NULL, L"open", L"steam://open/main", NULL, NULL, SW_SHOWNORMAL);
            for (int i = 0; i < 120 && !active_user(); i++) Sleep(1000);
            if (!active_user()) fail(L"Steam isn't logged in. Log in, then start triggerlook.exe.");
        }
        launch_options(steam, active_user(), options);
    }

    // Started outside Steam, the game can restart itself through Steam, which drops this command line
    SetEnvironmentVariableW(L"SteamAppId", L"240");
    SetEnvironmentVariableW(L"SteamGameId", L"240");
    swprintf(exe, BIG, L"%ls\\" GAME_EXE, game);
    static wchar_t cmd[BIG * 3];   // the engine skips argv[0] when it looks for flags, so the exe comes first
    swprintf(cmd, BIG * 3, L"\"%ls\" -insecure -novid %ls %ls", exe, options, args ? args : L"");

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, game, &si, &pi)) fail(L"Couldn't start the game.");
    CloseHandle(pi.hThread);

    for (int i = 0; !module_loaded(pi.dwProcessId, L"client.dll"); i++) {
        if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0)
            fail(L"The game closed or restarted through Steam, so triggerlook wasn't loaded. Start Steam first, then triggerlook.exe.");
        if (i > 600) fail(L"The game took too long to start, so triggerlook wasn't loaded.");
    }
    if (!inject(pi.hProcess, dll)) fail(L"Couldn't load triggerlook.dll into the game. Your antivirus may be blocking it.");
    // stay until the game closes: under Proton, the game goes down with the program that started it
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    return 0;
}
