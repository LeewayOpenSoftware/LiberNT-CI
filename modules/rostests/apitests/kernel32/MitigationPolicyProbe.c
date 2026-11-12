/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Record the SetProcessMitigationPolicy result of every policy class for a Windows 11 comparison
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

typedef BOOL (WINAPI *SET_POLICY)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
typedef BOOL (WINAPI *GET_POLICY)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);

static VOID
ProbeChild(SET_POLICY SetPolicy, GET_POLICY GetPolicy, ULONG Policy, ULONG Bit)
{
    DWORD Value = Bit < 32 ? (1UL << Bit) : 0;
    DWORD Current = 0;
    BOOL SetOk, GetOk;
    DWORD SetError, GetError;

    SetLastError(0xdeadbeef);
    SetOk = SetPolicy((PROCESS_MITIGATION_POLICY)Policy, &Value, sizeof(Value));
    SetError = GetLastError();
    SetLastError(0xdeadbeef);
    GetOk = GetPolicy(GetCurrentProcess(), (PROCESS_MITIGATION_POLICY)Policy, &Current, sizeof(Current));
    GetError = GetLastError();
    trace("MITIGATION policy %lu value 0x%lx: set %d error %lu; get %d error %lu value 0x%lx\n",
          Policy, Value, SetOk, SetOk ? 0 : SetError, GetOk, GetOk ? 0 : GetError, Current);
}

START_TEST(MitigationPolicyProbe)
{
    CHAR CommandLine[MAX_PATH * 2];
    STARTUPINFOA StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    SET_POLICY SetPolicy;
    GET_POLICY GetPolicy;
    ULONG Policy, Bit;
    char **argv;
    int argc;

    SetPolicy = (SET_POLICY)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessMitigationPolicy");
    GetPolicy = (GET_POLICY)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessMitigationPolicy");
    if (!SetPolicy || !GetPolicy)
    {
        skip("SetProcessMitigationPolicy is not exported\n");
        return;
    }

    argc = winetest_get_mainargs(&argv);
    if (argc >= 5 && !strcmp(argv[2], "child"))
    {
        ProbeChild(SetPolicy, GetPolicy, strtoul(argv[3], NULL, 10), strtoul(argv[4], NULL, 10));
        return;
    }

    for (Policy = 0; Policy <= 19; Policy++)
    {
        for (Bit = 0; Bit <= 32; Bit++)
        {
            if (Bit > 4 && Bit < 32)
                continue;
            sprintf(CommandLine, "\"%s\" MitigationPolicyProbe child %lu %lu", argv[0], Policy, Bit);
            if (!CreateProcessA(NULL, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
            {
                ok(0, "CreateProcessA failed: %lu\n", GetLastError());
                return;
            }
            WaitForSingleObject(ProcessInfo.hProcess, 30000);
            CloseHandle(ProcessInfo.hThread);
            CloseHandle(ProcessInfo.hProcess);
        }
    }
}
