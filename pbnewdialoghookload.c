/* pbnewdialoghookload.c - loader for pbnewdialoghook.dll
 *
 * Copyright (c) 2026 Bruce Armstrong
 * Licensed under the MIT License. See the LICENSE file in this directory for the
 * full text (https://opensource.org/licenses/MIT).
 *
 * Finds the running PowerBuilder IDE frame window, loads pbnewdialoghook.dll into THIS
 * process, and asks it to install a thread-level hook on the IDE's GUI thread.
 * The OS maps pbnewdialoghook.dll into the IDE as messages flow. The hook stays active
 * while this loader is running; closing it (OK on the message box) removes it.
 *
 * Build (MinGW-w64, 32-bit):
 *   i686-w64-mingw32-gcc -O2 -o pbnewdialoghookload.exe pbnewdialoghookload.c -luser32
 */

#include <windows.h>

typedef BOOL (*InstallFn)(DWORD);
typedef void (*RemoveFn)(void);

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;

    HWND frame = FindWindowW(L"PBFRAME", NULL);
    if (!frame) {
        MessageBoxW(NULL, L"PowerBuilder IDE (PBFRAME window) not found. Is it running?",
                    L"pbnewdialoghook loader", MB_OK | MB_ICONERROR);
        return 1;
    }
    DWORD tid = GetWindowThreadProcessId(frame, NULL);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) wcscpy(slash + 1, L"pbnewdialoghook.dll"); else wcscpy(path, L"pbnewdialoghook.dll");

    HMODULE dll = LoadLibraryW(path);
    if (!dll) {
        MessageBoxW(NULL, L"Could not load pbnewdialoghook.dll (must sit next to this exe).",
                    L"pbnewdialoghook loader", MB_OK | MB_ICONERROR);
        return 1;
    }

    InstallFn Install = (InstallFn)GetProcAddress(dll, "InstallHook");
    RemoveFn  Remove  = (RemoveFn) GetProcAddress(dll, "RemoveHook");
    if (!Install || !Remove) {
        MessageBoxW(NULL, L"pbnewdialoghook.dll is missing InstallHook/RemoveHook exports.",
                    L"pbnewdialoghook loader", MB_OK | MB_ICONERROR);
        FreeLibrary(dll);
        return 1;
    }

    if (!Install(tid)) {
        MessageBoxW(NULL, L"SetWindowsHookEx failed. Is this loader the same bitness (32-bit) "
                          L"and running as the same user as the IDE?",
                    L"pbnewdialoghook loader", MB_OK | MB_ICONERROR);
        FreeLibrary(dll);
        return 1;
    }

    MessageBoxW(NULL, L"Hook installed. Open File > New in the IDE; your entries appear in the "
                      L"object list on the configured tab (default \"PB Object\").\n\n"
                      L"Click OK here to remove the hook and exit.",
                L"pbnewdialoghook loader", MB_OK | MB_ICONINFORMATION);

    Remove();
    FreeLibrary(dll);
    return 0;
}
