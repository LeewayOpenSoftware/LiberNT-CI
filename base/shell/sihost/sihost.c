/*
 * PROJECT:     LiberNT Shell Infrastructure Host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shell infrastructure host; refuses while CoreUI/CoreMessaging do not exist
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

    /* Nothing to host yet: do not stay resident as if serving */
    OutputDebugStringW(L"sihost: CoreUI/CoreMessaging infrastructure is not available\n");
    return ERROR_NOT_SUPPORTED;
}
