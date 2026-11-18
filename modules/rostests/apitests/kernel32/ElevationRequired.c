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
MakeUnmanifestedImage(PCWSTR Path)
{
    WCHAR Self[MAX_PATH];

    if (!GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self)))
        return FALSE;
    return CopyFileW(Self, Path, FALSE);
}

static BOOL
MakeNoRunLevelImage(PCWSTR Path)
{
    WCHAR Self[MAX_PATH];
    CHAR Manifest[1024];
    HANDLE Update;

    if (!GetModuleFileNameW(NULL, Self, ARRAYSIZE(Self)))
        return FALSE;
    if (!CopyFileW(Self, Path, FALSE))
        return FALSE;

    StringCbCopyA(Manifest, sizeof(Manifest),
                  "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                  "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
                  "<assemblyIdentity name=\"NoRunLevel\" version=\"1.0.0.0\" "
                  "processorArchitecture=\"*\" type=\"win32\"/></assembly>");
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

static BOOL
Is32BitX86Image(PCWSTR Path)
{
    HANDLE File;
    IMAGE_DOS_HEADER DosHeader;
    IMAGE_FILE_HEADER FileHeader;
    DWORD Signature, Read;
    DWORD Machine = 0;

    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    if (ReadFile(File, &DosHeader, sizeof(DosHeader), &Read, NULL) &&
        Read == sizeof(DosHeader) && DosHeader.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(File, DosHeader.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(File, &Signature, sizeof(Signature), &Read, NULL) &&
        Read == sizeof(Signature) && Signature == IMAGE_NT_SIGNATURE &&
        ReadFile(File, &FileHeader, sizeof(FileHeader), &Read, NULL) &&
        Read == sizeof(FileHeader))
        Machine = FileHeader.Machine;

    CloseHandle(File);
    return Machine == IMAGE_FILE_MACHINE_I386;
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

static BOOL
AreInstallerDetectionPoliciesEnabled(void)
{
    HKEY PolicyKey;
    DWORD EnableLUA = 0;
    DWORD EnableInstaller = 0;
    DWORD Size;
    LONG Result;

    Result = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                          0,
                          KEY_READ,
                          &PolicyKey);
    if (Result != ERROR_SUCCESS)
        return FALSE;

    Size = sizeof(EnableLUA);
    RegQueryValueExW(PolicyKey, L"EnableLUA", NULL, NULL, (LPBYTE)&EnableLUA, &Size);

    Size = sizeof(EnableInstaller);
    Result = RegQueryValueExW(PolicyKey, L"EnableInstallerDetection", NULL, NULL,
                              (LPBYTE)&EnableInstaller, &Size);
    RegCloseKey(PolicyKey);

    return EnableLUA == 1 && Result == ERROR_SUCCESS && EnableInstaller == 1;
}

START_TEST(InstallerDetection)
{
    WCHAR Dir[MAX_PATH], SetupExe[MAX_PATH], SetupInvoker[MAX_PATH], SetupNoLevel[MAX_PATH];
    WCHAR PlainEnv[] = L"SystemRoot=C:\\Windows\0";
    HANDLE Restricted;
    DWORD Error;

    /* kernel32_apitest.exe has no manifest (version.rc and UI language .rc only),
       so the copied image is truly unmanifested */
    if (!AreInstallerDetectionPoliciesEnabled())
    {
        skip("EnableLUA or EnableInstallerDetection not both set to 1\n");
        return;
    }

    GetTempPathW(ARRAYSIZE(Dir), Dir);
    StringCchPrintfW(SetupExe, ARRAYSIZE(SetupExe), L"%ssetup_d1_test.exe", Dir);
    StringCchPrintfW(SetupInvoker, ARRAYSIZE(SetupInvoker), L"%ssetup_d1_asInvoker.exe", Dir);
    StringCchPrintfW(SetupNoLevel, ARRAYSIZE(SetupNoLevel), L"%ssetup_d1_noLevel.exe", Dir);

    if (!MakeUnmanifestedImage(SetupExe) ||
        !MakeImage(SetupInvoker, "asInvoker") ||
        !MakeNoRunLevelImage(SetupNoLevel))
    {
        skip("Cannot build the test images (error %lu)\n", GetLastError());
        goto Cleanup;
    }

    Restricted = MakeNonAdminToken();
    if (!Restricted || IsElevated(Restricted))
    {
        skip("No non-elevated token available\n");
    }
    else
    {
        Error = Launch(Restricted, SetupExe, PlainEnv);
        if (Is32BitX86Image(SetupExe))
            ok(Error == ERROR_ELEVATION_REQUIRED, "unmanifested setup.exe: got %lu (expected %lu)\n",
               Error, (DWORD)ERROR_ELEVATION_REQUIRED);
        else
            ok(Error != ERROR_ELEVATION_REQUIRED,
               "non-x86-32 target: installer detection does not apply, got %lu\n", Error);

        Error = Launch(Restricted, SetupInvoker, PlainEnv);
        ok(Error != ERROR_ELEVATION_REQUIRED,
           "manifested asInvoker setup.exe: got %lu (should not be elevation required)\n", Error);

        Error = Launch(Restricted, SetupNoLevel, PlainEnv);
        if (Is32BitX86Image(SetupNoLevel))
            ok(Error == ERROR_ELEVATION_REQUIRED,
               "manifest without requestedExecutionLevel setup.exe: got %lu (expected %lu)\n",
               Error, (DWORD)ERROR_ELEVATION_REQUIRED);
        else
            ok(Error != ERROR_ELEVATION_REQUIRED,
               "non-x86-32 target: installer detection does not apply, got %lu\n", Error);
    }
    if (Restricted)
        CloseHandle(Restricted);

Cleanup:
    DeleteFileW(SetupExe);
    DeleteFileW(SetupInvoker);
    DeleteFileW(SetupNoLevel);
}
