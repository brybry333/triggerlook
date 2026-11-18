/* wininject.exe <game exe> <dll> [game arguments...]: test helper (Wine/Proton or Windows). Starts the game, waits
 * until it has loaded client.dll, loads the dll into it (LoadLibraryW on a remote thread, like CS:TV's launcher and
 * bhoptools.exe's curtimefix.dll), then waits for the game to exit.
 * Build: x86_64-w64-mingw32-gcc -municode -O2 -o build/wininject.exe tools/wininject.c -static */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

static BOOL module_loaded(DWORD pid, const wchar_t *name)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W me = { sizeof(me) };
    BOOL found = FALSE;
    for (BOOL ok = Module32FirstW(s, &me); ok && !found; ok = Module32NextW(s, &me))
        found = !_wcsicmp(me.szModule, name);
    CloseHandle(s);
    return found;
}

static BOOL inject(HANDLE process, const wchar_t *dll)
{
    SIZE_T size = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll, size, NULL)) return FALSE;
    FARPROC load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, NULL);
    if (!t) return FALSE;
    WaitForSingleObject(t, 30000);
    DWORD module = 0;
    GetExitCodeThread(t, &module);
    CloseHandle(t);
    return module != 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) { fwprintf(stderr, L"usage: wininject <game exe> <dll> [args...]\n"); return 2; }
    static wchar_t cmd[32768], dir[MAX_PATH];
    _snwprintf(cmd, 32767, L"\"%ls\"", argv[1]);
    for (int i = 3; i < argc; i++) { wcscat(cmd, L" "); wcscat(cmd, argv[i]); }
    wcsncpy(dir, argv[1], MAX_PATH - 1);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(argv[1], cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { fwprintf(stderr, L"can't start the game (%lu)\n", GetLastError()); return 1; }
    int i = 0;
    for (; i < 600 && !module_loaded(pi.dwProcessId, L"client.dll"); i++) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) { fwprintf(stderr, L"the game exited before client.dll\n"); return 1; }
    }
    fwprintf(stderr, i < 600 && inject(pi.hProcess, argv[2]) ? L"wininject: loaded %ls\n" : L"wininject: FAILED to load %ls\n", argv[2]);
    WaitForSingleObject(pi.hProcess, INFINITE);
    return 0;
}
