/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A 32-bit process without user32 ends with STATUS_FATAL_USER_CALLBACK_EXCEPTION on its first win32k call
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#include <apitest.h>
#include <ndk/pstypes.h>
#include <ndk/rtlfuncs.h>

HDC NTAPI NtGdiCreateCompatibleDC(HDC hdc);

START_TEST(GdiOnlyCallback)
{
    PPEB Peb = NtCurrentPeb();
    BOOL Wow64 = FALSE;

    if (!IsWow64Process(GetCurrentProcess(), &Wow64) || !Wow64)
    {
        skip("not a 32-bit process under WOW64\n");
        return;
    }

    if (GetEnvironmentVariableW(L"WOW64CB_GDI_ONLY_CHILD", NULL, 0))
    {
        if (GetModuleHandleW(L"user32") != NULL)
            ExitProcess(3);
        if (Peb->KernelCallbackTable != NULL)
            ExitProcess(4);
        /* The first win32k call converts the thread to a GUI thread and must end the process. */
        NtGdiCreateCompatibleDC(NULL);
        ExitProcess(5);
    }

    ok(GetModuleHandleW(L"user32") == NULL, "user32 is loaded; the process is not GDI-only\n");
    ok(Peb->KernelCallbackTable == NULL, "KernelCallbackTable = %p\n", Peb->KernelCallbackTable);

    {
        WCHAR Path[MAX_PATH];
        WCHAR CmdLine[MAX_PATH + 32];
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        DWORD Code = 0;
        BOOL Ret;

        GetModuleFileNameW(NULL, Path, MAX_PATH);
        SetEnvironmentVariableW(L"WOW64CB_GDI_ONLY_CHILD", L"1");
        swprintf(CmdLine, MAX_PATH + 32, L"\"%s\" GdiOnlyCallback", Path);
        Ret = CreateProcessW(NULL, CmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        ok(Ret, "CreateProcess failed: %lu\n", GetLastError());
        if (Ret)
        {
            WaitForSingleObject(pi.hProcess, 30000);
            Ret = GetExitCodeProcess(pi.hProcess, &Code);
            ok(Ret, "GetExitCodeProcess failed: %lu\n", GetLastError());
            ok(Code == 0xC000041D, "Expected child exit code 0xc000041d, got 0x%08lx\n", Code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    ok(GetModuleHandleW(L"user32") == NULL, "user32 got loaded\n");
}
