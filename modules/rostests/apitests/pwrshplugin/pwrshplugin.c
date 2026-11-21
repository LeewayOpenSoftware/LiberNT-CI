/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for pwrshplugin.dll (WSMan plugin stubs)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>

static const CHAR * const g_exports[] =
{
    "GetCLRVersionForPSVersion",
    "PerformWSManPluginReportCompletion",
    "WSManPluginCommand",
    "WSManPluginConnect",
    "WSManPluginReceive",
    "WSManPluginReleaseCommandContext",
    "WSManPluginReleaseShellContext",
    "WSManPluginSend",
    "WSManPluginShell",
    "WSManPluginShutdown",
    "WSManPluginSignal",
    "WSManPluginStartup",
};

typedef DWORD (WINAPI *WSManPluginStartupFn)(DWORD, PCWSTR, PVOID);
typedef BOOL (WINAPI *GetCLRVersionForPSVersionFn)(PCWSTR, PWSTR, DWORD);

void func_pwrshplugin(void)
{
    HMODULE hMod;
    unsigned int i;

    hMod = LoadLibraryA("pwrshplugin.dll");
    ok(hMod != NULL, "LoadLibraryA(pwrshplugin.dll) failed: %lu\n", GetLastError());
    if (!hMod)
        return;

    for (i = 0; i < _countof(g_exports); i++)
    {
        FARPROC proc = GetProcAddress(hMod, g_exports[i]);
        ok(proc != NULL, "export %s missing\n", g_exports[i]);
    }

    {
        WSManPluginStartupFn pStartup = (WSManPluginStartupFn)(VOID*)GetProcAddress(hMod, "WSManPluginStartup");
        GetCLRVersionForPSVersionFn pGetClr = (GetCLRVersionForPSVersionFn)(VOID*)GetProcAddress(hMod, "GetCLRVersionForPSVersion");
        DWORD dwRet;
        WCHAR szBuffer[32];
        BOOL bRet;

        ok(pStartup != NULL, "WSManPluginStartup unresolved\n");
        if (pStartup)
        {
            dwRet = pStartup(0, L"apitest", NULL);
            ok(dwRet == ERROR_NOT_SUPPORTED,
               "WSManPluginStartup returned %lu, expected ERROR_NOT_SUPPORTED\n", dwRet);
        }

        ok(pGetClr != NULL, "GetCLRVersionForPSVersion unresolved\n");
        if (pGetClr)
        {
            szBuffer[0] = L'x';
            bRet = pGetClr(L"7.4", szBuffer, _countof(szBuffer));
            ok(bRet == FALSE, "GetCLRVersionForPSVersion returned %d, expected FALSE\n", bRet);
            ok(szBuffer[0] == L'\0', "GetCLRVersionForPSVersion buffer not cleared\n");
        }
    }

    FreeLibrary(hMod);
}
