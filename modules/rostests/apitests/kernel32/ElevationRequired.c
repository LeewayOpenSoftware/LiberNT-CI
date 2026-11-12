/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     CreateProcess and requestedExecutionLevel (ERROR_ELEVATION_REQUIRED)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"
#include <winbase.h>
#include <winuser.h>

static const char ManifestFormat[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
    "<trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security><requestedPrivileges>"
    "<requestedExecutionLevel level=\"%s\" uiAccess=\"false\"/>"
    "</requestedPrivileges></security></trustInfo></assembly>";

static BOOL
MakeImage(PCWSTR Path, PCSTR Level)
{
    WCHAR Self[MAX_PATH];
    CHAR Manifest[1024];
    HANDLE Update;

    if (!GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self)))
        return FALSE;
    if (!CopyFileW(Self, Path, FALSE))
        return FALSE;

    StringCbPrintfA(Manifest, sizeof(Manifest), ManifestFormat, Level);
    Update = BeginUpdateResourceW(Path, TRUE);
    if (!Update)
        return FALSE;
    if (!UpdateResourceW(Update, (LPCWSTR)RT_MANIFEST, MAKEINTRESOURCEW(1),
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         Manifest, (DWORD)strlen(Manifest)))
    {
        EndUpdateResourceW(Update, TRUE);
        return FALSE;
    }
    return EndUpdateResourceW(Update, FALSE);
}

static BOOL
IsElevated(HANDLE Token)
{
    TOKEN_ELEVATION Elevation;
    DWORD Length;
    HANDLE Process = NULL;
    BOOL Ret;

    if (!Token)
    {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Process))
            return TRUE;
        Token = Process;
    }
    Ret = GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Length);
    if (Process)
        CloseHandle(Process);
    return !Ret || Elevation.TokenIsElevated;
}

static DWORD
Launch(HANDLE Token, PCWSTR Path, PWSTR Environment)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR CmdLine[MAX_PATH + 2];
    DWORD Flags = CREATE_SUSPENDED | (Environment ? CREATE_UNICODE_ENVIRONMENT : 0);
    BOOL Ret;

    StringCchPrintfW(CmdLine, ARRAYSIZE(CmdLine), L"\"%s\"", Path);
    SetLastError(0xdeadbeef);
    if (Token)
        Ret = CreateProcessAsUserW(Token, Path, CmdLine, NULL, NULL, FALSE, Flags, Environment, NULL, &si, &pi);
    else
        Ret = CreateProcessW(Path, CmdLine, NULL, NULL, FALSE, Flags, Environment, NULL, &si, &pi);
    if (!Ret)
        return GetLastError();
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ERROR_SUCCESS;
}

static HANDLE
MakeNonAdminToken(void)
{
    BYTE AdminsBuffer[SECURITY_MAX_SID_SIZE];
    DWORD Size = sizeof(AdminsBuffer);
    SID_AND_ATTRIBUTES Deny;
    HANDLE Process, Restricted = NULL;

    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, AdminsBuffer, &Size))
        return NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT,
                          &Process))
        return NULL;
    Deny.Sid = AdminsBuffer;
    Deny.Attributes = 0;
    if (!CreateRestrictedToken(Process, DISABLE_MAX_PRIVILEGE, 1, &Deny, 0, NULL, 0, NULL, &Restricted))
        Restricted = NULL;
    CloseHandle(Process);
    return Restricted;
}

START_TEST(ElevationRequired)
{
    WCHAR Dir[MAX_PATH], Admin[MAX_PATH], Invoker[MAX_PATH], Highest[MAX_PATH];
    WCHAR InvokerEnv[] = L"__COMPAT_LAYER=RunAsInvoker\0SystemRoot=C:\\Windows\0";
    WCHAR PlainEnv[] = L"SystemRoot=C:\\Windows\0";
    HANDLE Restricted;
    DWORD Error;

    GetTempPathW(ARRAYSIZE(Dir), Dir);
    StringCchPrintfW(Admin, ARRAYSIZE(Admin), L"%selev_admin.exe", Dir);
    StringCchPrintfW(Invoker, ARRAYSIZE(Invoker), L"%selev_invoker.exe", Dir);
    StringCchPrintfW(Highest, ARRAYSIZE(Highest), L"%selev_highest.exe", Dir);

    if (!MakeImage(Admin, "requireAdministrator") ||
        !MakeImage(Invoker, "asInvoker") ||
        !MakeImage(Highest, "highestAvailable"))
    {
        skip("Cannot build the test images (error %lu)\n", GetLastError());
        goto Cleanup;
    }

    Error = Launch(NULL, Invoker, NULL);
    ok(Error == ERROR_SUCCESS, "asInvoker: got %lu\n", Error);
    Error = Launch(NULL, Highest, NULL);
    ok(Error == ERROR_SUCCESS, "highestAvailable: got %lu\n", Error);

    Error = Launch(NULL, Admin, NULL);
    if (IsElevated(NULL))
        ok(Error == ERROR_SUCCESS, "elevated caller: got %lu\n", Error);
    else
        ok(Error == ERROR_ELEVATION_REQUIRED, "non-elevated caller: got %lu\n", Error);

    Restricted = MakeNonAdminToken();
    if (!Restricted || IsElevated(Restricted))
    {
        skip("No non-elevated token available\n");
    }
    else
    {
        Error = Launch(Restricted, Admin, PlainEnv);
        ok(Error == ERROR_ELEVATION_REQUIRED, "requireAdministrator: got %lu\n", Error);

        Error = Launch(Restricted, Admin, InvokerEnv);
        ok(Error != ERROR_ELEVATION_REQUIRED, "RunAsInvoker: got %lu\n", Error);

        Error = Launch(Restricted, Highest, PlainEnv);
        ok(Error != ERROR_ELEVATION_REQUIRED, "highestAvailable: got %lu\n", Error);

        Error = Launch(Restricted, Invoker, PlainEnv);
        ok(Error != ERROR_ELEVATION_REQUIRED, "asInvoker: got %lu\n", Error);
    }
    if (Restricted)
        CloseHandle(Restricted);

Cleanup:
    DeleteFileW(Admin);
    DeleteFileW(Invoker);
    DeleteFileW(Highest);
}
