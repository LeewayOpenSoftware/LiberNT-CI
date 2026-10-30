/*
 * PROJECT:     LiberNT kernel32 API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     DosPathToSessionPath fails explicitly and returns no path
 */

#include "precomp.h"

typedef DWORD (WINAPI *PFN_DOSPATHTOSESSIONPATHW)(DWORD, LPWSTR, LPWSTR *);
typedef DWORD (WINAPI *PFN_DOSPATHTOSESSIONPATHA)(DWORD, LPSTR, LPSTR *);

START_TEST(DosPathToSessionPath)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    PFN_DOSPATHTOSESSIONPATHW pW = (PFN_DOSPATHTOSESSIONPATHW)GetProcAddress(Kernel32, "DosPathToSessionPathW");
    PFN_DOSPATHTOSESSIONPATHA pA = (PFN_DOSPATHTOSESSIONPATHA)GetProcAddress(Kernel32, "DosPathToSessionPathA");
    WCHAR PathW[] = L"C:\\Windows";
    CHAR PathA[] = "C:\\Windows";
    LPWSTR OutW;
    LPSTR OutA;
    DWORD Result;

    ok(pW != NULL, "DosPathToSessionPathW is not exported\n");
    ok(pA != NULL, "DosPathToSessionPathA is not exported\n");

    if (pW)
    {
        OutW = (LPWSTR)(ULONG_PTR)0xCCCCCCCC;
        SetLastError(0xDEADBEEF);
        Result = pW(0, PathW, &OutW);
        ok(Result == 0, "Result = %lu\n", Result);
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "error %lu\n", GetLastError());
        ok(OutW == NULL, "OutPath = %p, expected NULL\n", OutW);

        SetLastError(0xDEADBEEF);
        Result = pW(0, NULL, NULL);
        ok(Result == 0, "NULL arguments: Result = %lu\n", Result);
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "NULL arguments: error %lu\n", GetLastError());
    }

    if (pA)
    {
        OutA = (LPSTR)(ULONG_PTR)0xCCCCCCCC;
        SetLastError(0xDEADBEEF);
        Result = pA(1, PathA, &OutA);
        ok(Result == 0, "Result = %lu\n", Result);
        ok(GetLastError() == ERROR_CALL_NOT_IMPLEMENTED, "error %lu\n", GetLastError());
        ok(OutA == NULL, "OutPath = %p, expected NULL\n", OutA);
    }
}
