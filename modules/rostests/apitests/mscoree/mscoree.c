/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fallback-path test for mscoree.dll (no real .NET CLR)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windows.h>

typedef HRESULT (WINAPI *GetCORVersionFn)(LPWSTR, DWORD, DWORD *);
typedef HRESULT (WINAPI *CorBindToRuntimeExFn)(LPWSTR, LPWSTR, DWORD, REFCLSID, REFIID, LPVOID *);

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

#ifndef CLR_E_SHIM_RUNTIME
#define CLR_E_SHIM_RUNTIME 0x80131700L
#endif

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

void func_mscoree(void)
{
    HMODULE hMod, hNtdll;
    DWORD dwVersion[4] = { 0, 0, 0, 0 };

    hMod = LoadLibraryA("mscoree.dll");
    ok(hMod != NULL, "LoadLibraryA(mscoree.dll) failed: %lu\n", GetLastError());
    if (!hMod)
        return;

    /* The shim reports a runtime version even with no CLR installed. */
    {
        GetCORVersionFn pGetCORVersion = (GetCORVersionFn)(VOID*)GetProcAddress(hMod, "GetCORVersion");
        WCHAR szVersion[64];
        DWORD dwLength = 0;
        HRESULT hr;

        ok(pGetCORVersion != NULL, "GetCORVersion unresolved\n");
        if (pGetCORVersion)
        {
            hr = pGetCORVersion(szVersion, _countof(szVersion), &dwLength);
            ok(hr == S_OK, "GetCORVersion returned hr=%lx\n", hr);
            ok(szVersion[0] == L'v', "GetCORVersion string unexpected: %s\n", wine_dbgstr_wn(szVersion, 8));
        }
    }

    /* Fallback bind: with no real CLR and no Mono the shim must fail
     * cleanly (CLR_E_SHIM_RUNTIME) or succeed if Mono is present - it
     * must never crash or return garbage. */
    {
        CorBindToRuntimeExFn pBind = (CorBindToRuntimeExFn)(VOID*)GetProcAddress(hMod, "CorBindToRuntimeEx");
        LPVOID pv = NULL;
        HRESULT hr;

        ok(pBind != NULL, "CorBindToRuntimeEx unresolved\n");
        if (pBind)
        {
            hr = pBind(NULL, NULL, 0, &IID_IUnknown, &IID_IUnknown, &pv);
            ok(hr == S_OK || hr == CLR_E_SHIM_RUNTIME,
               "CorBindToRuntimeEx returned hr=%lx\n", hr);
            ok(pv == NULL || hr == S_OK, "CorBindToRuntimeEx output state inconsistent\n");
        }
    }

    /* The shim is an OS component: version resource tracks the OS. */
    ok(GetFileVersion("mscoree.dll", dwVersion), "no version resource\n");
    if (dwVersion[0] || dwVersion[1] || dwVersion[2] || dwVersion[3])
    {
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
