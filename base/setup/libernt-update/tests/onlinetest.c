/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Unprivileged test: the online target path (live system drive)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

/*
 * Regression for the S1 online run 1 failure: `bootstrap install --bundle`
 * against the LIVE system returned ERROR_INVALID_NAME at target init, and
 * every online destination validation would have failed the same way,
 * because the online root is the bare drive ("C:") while
 * GetVolumeInformationW/BsFinalPath demand ("C:\") or reject a drive path,
 * and the canonical-name guard is an --image-only concept (Pam review E1).
 *
 * UNELEVATED on the host: BsTargetInitOnline must now pass the volume check
 * and stop at the SeRestore privilege (ERROR_PRIVILEGE_NOT_HELD) instead of
 * ERROR_INVALID_NAME, and BsValidateTargetRelPath must accept the standard
 * online destinations against a synthetic online target.
 *
 * Build (from this directory, MinGW/Clang on PATH):
 *   clang --target=x86_64-w64-mingw32 -O1 -Wall -Wextra -Wno-unused-parameter -DUNICODE -D_UNICODE -I../../../system/servicing/pending onlinetest.c ../bundle.c ../multisz.c ../reg.c ../selftest.c ../service.c ../target.c ../../../system/servicing/pending/s1core.c -mconsole -o onlinetest.exe -lmsvcrt-os -lkernel32 -lntdll -ladvapi32 -lshell32 -lole32 -luuid
 *   ./onlinetest.exe
 */

#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>

#include "../bootstrap.h"
#define main bootstrap_main
#include "../main.c"
#undef main

static int Failures;

static void
Check(int Cond, const char *Msg)
{
    if (Cond)
        printf("ok:   %s\n", Msg);
    else
    {
        printf("FAIL: %s\n", Msg);
        Failures++;
    }
}

int
main(void)
{
    BS_TARGET Target;
    DWORD Error;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* 1. Online init: volume query must accept the drive root. Unprivileged
     *    it then stops at SeRestore, which is the expected wall. */
    memset(&Target, 0, sizeof(Target));
    Error = BsTargetInitOnline(&Target);
    Check(Error != ERROR_INVALID_NAME,
          "online init no longer fails with ERROR_INVALID_NAME");
    Check(Error == ERROR_PRIVILEGE_NOT_HELD || Error == ERROR_SUCCESS,
          "online init reaches the privilege gate (or succeeds when elevated)");
    if (Error == ERROR_SUCCESS)
        Check(Target.Root[1] == L':' && Target.Root[2] == 0,
              "online root is the bare drive letter");

    /* 2. Online destination validation against the live system drive. */
    memset(&Target, 0, sizeof(Target));
    Target.Root[0] = L'C';
    Target.Root[1] = L':';
    Target.Root[2] = 0;
    Target.IsImage = FALSE;

    Error = BsValidateTargetRelPath(&Target, L"Windows\\System32\\LiberNTUpdatePending.exe");
    Check(Error == ERROR_SUCCESS, "online Windows\\System32 dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Servicing\\bin\\libernt-update.exe");
    Check(Error == ERROR_SUCCESS, "online LiberNT dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"LiberNT\\Update\\libernt-update-ui.exe");
    Check(Error == ERROR_SUCCESS, "online UI exe dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\x.lnk");
    Check(Error == ERROR_SUCCESS, "online Start Menu shortcut dest accepted");
    Error = BsValidateTargetRelPath(&Target, L"Temp\\evil.exe");
    Check(Error == ERROR_BAD_PATHNAME, "online still refuses off-allowlist prefixes");
    Error = BsValidateTargetRelPath(&Target, L"..\\Windows\\System32\\evil.exe");
    Check(Error == ERROR_BAD_PATHNAME, "online still refuses traversal");

    printf("%s\n", Failures ? "ONLINE TARGET TEST FAILED" : "ONLINE TARGET TEST PASSED");
    return Failures ? 1 : 0;
}
