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
    "WSManPluginStartup",
    "WSManPluginShutdown",
    "WSManPluginShell",
    "WSManPluginCommand",
    "WSManPluginSend",
    "WSManPluginReceive",
    "WSManPluginSignal",
    "WSManPluginConnect",
    "WSManPluginReleaseShellContext",
    "WSManPluginReleaseCommandContext",
};

/* The two undocumented Windows exports must stay absent. */
static const CHAR * const g_notExported[] =
{
    "GetCLRVersionForPSVersion",
    "PerformWSManPluginReportCompletion",
};

typedef DWORD (WINAPI *WSManPluginStartupFn)(DWORD, PCWSTR, PVOID, PVOID *);
typedef LONG (WINAPI *RtlGetVersionFn)(PVOID);

typedef struct _TEST_OSVERSIONINFOW
{
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} TEST_OSVERSIONINFOW;

static BOOL
GetFileVersion(
    _In_ LPCSTR FileName,
    _Out_ DWORD *pVersion)
{
    DWORD dwHandle, dwSize;
    LPBYTE pData;
    BOOL bRet = FALSE;

    dwSize = GetFileVersionInfoSizeA(FileName, &dwHandle);
    if (!dwSize)
        return FALSE;

    pData = HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!pData)
        return FALSE;

    if (GetFileVersionInfoA(FileName, 0, dwSize, pData))
    {
        VS_FIXEDFILEINFO *pFfi;
        UINT uLen;
        if (VerQueryValueA(pData, "\\", (PVOID *)&pFfi, &uLen) && pFfi)
        {
            pVersion[0] = HIWORD(pFfi->dwFileVersionMS);
            pVersion[1] = LOWORD(pFfi->dwFileVersionMS);
            pVersion[2] = HIWORD(pFfi->dwFileVersionLS);
            pVersion[3] = LOWORD(pFfi->dwFileVersionLS);
            bRet = TRUE;
        }
    }

    HeapFree(GetProcessHeap(), 0, pData);
    return bRet;
}

void func_pwrshplugin(void)
{
    HMODULE hMod, hNtdll;
    unsigned int i;
    DWORD dwVersion[4];
    static const DWORD dwMinVersion[4] = { 6, 3, 9600, 16383 };

    hMod = LoadLibraryA("pwrshplugin.dll");
    ok(hMod != NULL, "LoadLibraryA(pwrshplugin.dll) failed: %lu\n", GetLastError());
    if (!hMod)
        return;

    for (i = 0; i < _countof(g_exports); i++)
    {
        FARPROC proc = GetProcAddress(hMod, g_exports[i]);
        ok(proc != NULL, "export %s missing\n", g_exports[i]);
    }

    for (i = 0; i < _countof(g_notExported); i++)
    {
        FARPROC proc = GetProcAddress(hMod, g_notExported[i]);
        ok(proc == NULL, "undocumented export %s unexpectedly present\n", g_notExported[i]);
    }

    {
        WSManPluginStartupFn pStartup = (WSManPluginStartupFn)(VOID*)GetProcAddress(hMod, "WSManPluginStartup");
        PVOID pContext = (PVOID)(ULONG_PTR)0x1;
        DWORD dwRet;

        ok(pStartup != NULL, "WSManPluginStartup unresolved\n");
        if (pStartup)
        {
            dwRet = pStartup(0, L"apitest", NULL, &pContext);
            ok(dwRet == ERROR_NOT_SUPPORTED,
               "WSManPluginStartup returned %lu, expected ERROR_NOT_SUPPORTED\n", dwRet);
            ok(pContext == NULL, "WSManPluginStartup did not clear *pluginContext\n");
        }
    }

    ok(GetFileVersion("pwrshplugin.dll", dwVersion), "no version resource\n");
    if (dwVersion[0] || dwVersion[1] || dwVersion[2] || dwVersion[3])
    {
        ok(memcmp(dwVersion, dwMinVersion, sizeof(dwVersion)) >= 0,
           "FileVersion %lu.%lu.%lu.%lu below 6.3.9600.16383\n",
           dwVersion[0], dwVersion[1], dwVersion[2], dwVersion[3]);

        hNtdll = GetModuleHandleA("ntdll.dll");
        if (hNtdll)
        {
            RtlGetVersionFn pRtlGetVersion = (RtlGetVersionFn)(VOID*)GetProcAddress(hNtdll, "RtlGetVersion");
            if (pRtlGetVersion)
            {
                TEST_OSVERSIONINFOW osvi;
                osvi.dwOSVersionInfoSize = sizeof(osvi);
                if (pRtlGetVersion(&osvi) == 0)
                {
                    ok(dwVersion[0] == osvi.dwMajorVersion &&
                       dwVersion[1] == osvi.dwMinorVersion &&
                       dwVersion[2] == osvi.dwBuildNumber,
                       "FileVersion %lu.%lu.%lu.%lu != OS version %lu.%lu.%lu\n",
                       dwVersion[0], dwVersion[1], dwVersion[2], dwVersion[3],
                       osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
                }
            }
        }
    }

    FreeLibrary(hMod);
}
