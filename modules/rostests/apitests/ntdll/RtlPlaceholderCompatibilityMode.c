/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the process and thread placeholder compatibility modes
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include "precomp.h"

typedef CHAR (NTAPI *PFN_QUERY_PHCM)(VOID);
typedef CHAR (NTAPI *PFN_SET_PHCM)(CHAR);

static PFN_QUERY_PHCM pQueryProcess, pQueryThread;
static PFN_SET_PHCM pSetProcess, pSetThread;

typedef struct _PHCM_THREAD
{
    CHAR Initial;
    CHAR Previous;
    CHAR After;
    CHAR Invalid;
    CHAR Teb;
} PHCM_THREAD, *PPHCM_THREAD;

static
DWORD
WINAPI
PlaceholderThread(
    _In_ PVOID Parameter)
{
    PPHCM_THREAD Result = Parameter;

    Result->Initial = pQueryThread();
    Result->Previous = pSetThread(2 /* PHCM_EXPOSE_PLACEHOLDERS */);
    Result->After = pQueryThread();
    Result->Invalid = pSetThread(4);
    Result->Teb = NtCurrentTeb()->PlaceholderCompatibilityMode;
    return 0;
}

START_TEST(RtlPlaceholderCompatibilityMode)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    PHCM_THREAD Result;
    HANDLE Thread;

    pQueryProcess = (PFN_QUERY_PHCM)GetProcAddress(Ntdll, "RtlQueryProcessPlaceholderCompatibilityMode");
    pSetProcess = (PFN_SET_PHCM)GetProcAddress(Ntdll, "RtlSetProcessPlaceholderCompatibilityMode");
    pQueryThread = (PFN_QUERY_PHCM)GetProcAddress(Ntdll, "RtlQueryThreadPlaceholderCompatibilityMode");
    pSetThread = (PFN_SET_PHCM)GetProcAddress(Ntdll, "RtlSetThreadPlaceholderCompatibilityMode");
    if (!pQueryProcess || !pSetProcess || !pQueryThread || !pSetThread)
    {
        skip("placeholder compatibility mode exports are not available\n");
        return;
    }

    /* Process: disguise by default, the setter returns the previous mode */
    ok_int(pQueryProcess(), 1 /* PHCM_DISGUISE_PLACEHOLDER */);
    ok_int(pSetProcess(2), 1);
    ok_int(pQueryProcess(), 2);
    ok_int(pSetProcess(0), -1 /* PHCM_ERROR_INVALID_PARAMETER */);
    ok_int(pSetProcess(4), -1);
    ok_int(pSetProcess(-5), -1);
    ok_int(pQueryProcess(), 2);
    ok_int(pSetProcess(1), 2);
    ok_int(pQueryProcess(), 1);

    /* Thread: application default, kept in the TEB */
    ok_int(pQueryThread(), 0 /* PHCM_APPLICATION_DEFAULT */);
    ok_int(pSetThread(1), 0);
    ok_int(pQueryThread(), 1);
    ok_int(NtCurrentTeb()->PlaceholderCompatibilityMode, 1);
    ok_int(pSetThread(-1), -1);
    ok_int(pQueryThread(), 1);

    /* Another thread starts from the default and does not change this one */
    RtlZeroMemory(&Result, sizeof(Result));
    Thread = CreateThread(NULL, 0, PlaceholderThread, &Result, 0, NULL);
    ok(Thread != NULL, "CreateThread failed: %lu\n", GetLastError());
    if (Thread)
    {
        ok_int(WaitForSingleObject(Thread, 10000), WAIT_OBJECT_0);
        CloseHandle(Thread);
        ok_int(Result.Initial, 0);
        ok_int(Result.Previous, 0);
        ok_int(Result.After, 2);
        ok_int(Result.Invalid, -1);
        ok_int(Result.Teb, 2);
    }
    ok_int(pQueryThread(), 1);
    ok_int(pSetThread(0), 1);
    ok_int(pQueryProcess(), 1);
}
