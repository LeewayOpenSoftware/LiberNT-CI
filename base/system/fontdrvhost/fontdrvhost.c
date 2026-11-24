/*
 * PROJECT:     LiberNT Usermode Font Driver Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Host process for user-mode font drivers; refuses while win32k has no UMFD channel
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>

int WINAPI
wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    /* The host defines no options */
    if (lpCmdLine && *lpCmdLine)
        return ERROR_INVALID_PARAMETER;

    /* Fonts are still parsed in the kernel: there is no channel to serve, so do not pretend to */
    OutputDebugStringW(L"fontdrvhost: win32k has no user-mode font driver channel; fonts stay on the kernel path\n");
    return ERROR_NOT_SUPPORTED;
}
