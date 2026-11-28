/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A statically imported DLL's process-attach DllMain runs under the loader lock
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

START_TEST(LdrInitLoaderLock)
{
    WCHAR Child[MAX_PATH];
    STARTUPINFOW StartupInfo = { sizeof(StartupInfo) };
    PROCESS_INFORMATION ProcessInfo;
    DWORD Length, ExitCode = 0;
    PWSTR Slash;

    /* ldrlock_child.exe and ldrlock_dll.dll ship next to the test executable */
    Length = GetModuleFileNameW(NULL, Child, ARRAYSIZE(Child));
    Slash = wcsrchr(Child, L'\\');
    if (!Length || Length >= ARRAYSIZE(Child) || !Slash ||
        (SIZE_T)(Slash + 1 - Child) + wcslen(L"ldrlock_child.exe") >= ARRAYSIZE(Child))
    {
        skip("cannot build the child path\n");
        return;
    }
    wcscpy(Slash + 1, L"ldrlock_child.exe");
    if (GetFileAttributesW(Child) == INVALID_FILE_ATTRIBUTES)
    {
        skip("ldrlock_child.exe is not present: %lu\n", GetLastError());
        return;
    }

    if (!CreateProcessW(Child, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        ok(0, "CreateProcessW(ldrlock_child.exe) failed with %lu\n", GetLastError());
        return;
    }
    ok(WaitForSingleObject(ProcessInfo.hProcess, 30000) == WAIT_OBJECT_0, "the child did not exit\n");
    ok(GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode), "GetExitCodeProcess failed with %lu\n", GetLastError());
    ok(ExitCode == 1, "loader lock state at process attach is %lu, expected 1 (held)\n", ExitCode);

    if (ExitCode == STILL_ACTIVE)
        TerminateProcess(ProcessInfo.hProcess, 1);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}
