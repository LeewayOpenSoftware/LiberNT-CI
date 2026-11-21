/*
 * PROJECT:     Orosi Firewall settings
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The elevated side of the Orosi Firewall page
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 *
 * The Control Panel page starts this program with "runas" when its caller is not elevated, so
 * the elevation prompt names "Orosi Firewall settings" rather than a host program. Its manifest
 * asks for an administrator. It takes one of three words (customize, restore, allowedapps) and
 * runs that action of the page in shell32; nothing else crosses from the caller.
 */

#include <windef.h>
#include <winbase.h>

typedef VOID (WINAPI *FW_RUN)(HWND, HINSTANCE, LPCWSTR, int);

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    HMODULE shell32 = LoadLibraryExW(L"shell32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    FW_RUN run;

    if (!shell32)
        shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32)
        return 1;
    run = (FW_RUN)GetProcAddress(shell32, "OrosiFirewall_RunDLLW");
    if (run)
        run(NULL, hInstance, lpCmdLine ? lpCmdLine : L"", nCmdShow);
    FreeLibrary(shell32);
    return run ? 0 : 1;
}
