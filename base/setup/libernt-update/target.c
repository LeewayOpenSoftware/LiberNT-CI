/*
 * PROJECT:     LiberNT Update
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Target abstraction (online/image root) and the protected servicing tree
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "bootstrap.h"
#include <sddl.h>
#include <aclapi.h>

/* Same rule as libservicing CheckSecure (Pam review B1): owner SYSTEM or
 * Administrators, and a PROTECTED DACL made only of allow ACEs for SYSTEM
 * and Administrators. The SDDL below also pins the owner so the check holds
 * no matter which account creates the tree. */
static const WCHAR BsSecureSddl[] =
    L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

static BOOL
BsSidTrusted(
    _In_ PSID Sid)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    static PSID SystemSid, AdminSid;
    static BOOL Resolved;

    if (!Resolved)
    {
        if (!AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
                                      0, 0, 0, 0, 0, 0, 0, &SystemSid))
            return FALSE;
        if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                      DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                      &AdminSid))
            return FALSE;
        Resolved = TRUE;
    }
    return EqualSid(Sid, SystemSid) || EqualSid(Sid, AdminSid);
}

DWORD
BsCheckSecure(
    _In_ PCWSTR Path)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    PSID Owner = NULL;
    PACL Dacl = NULL;
    SECURITY_DESCRIPTOR_CONTROL Control = 0;
    DWORD Revision, i, Result = ERROR_ACCESS_DENIED;
    DWORD Error;

    Error = GetNamedSecurityInfoW((PWSTR)Path, SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  &Owner, NULL, &Dacl, NULL, &Sd);
    if (Error != ERROR_SUCCESS)
        return ERROR_ACCESS_DENIED; /* no ACLs (FAT) or unreadable: fail closed */
    if (!Owner || !Dacl ||
        !GetSecurityDescriptorControl(Sd, &Control, &Revision) ||
        !(Control & SE_DACL_PROTECTED))
        goto out;
    if (!BsSidTrusted(Owner) || Dacl->AceCount == 0)
        goto out;
    for (i = 0; i < Dacl->AceCount; i++)
    {
        ACE_HEADER *Ace;

        if (!GetAce(Dacl, i, (void **)&Ace))
            goto out;
        if (Ace->AceType != ACCESS_ALLOWED_ACE_TYPE)
            goto out;
        if (!BsSidTrusted((PSID)&((ACCESS_ALLOWED_ACE *)Ace)->SidStart))
            goto out;
    }
    Result = ERROR_SUCCESS;
out:
    LocalFree(Sd);
    return Result;
}

DWORD
BsCreateSecureDir(
    _In_ PCWSTR Path)
{
    PSECURITY_DESCRIPTOR Sd = NULL;
    SECURITY_ATTRIBUTES Sa;
    DWORD Error = ERROR_SUCCESS;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            BsSecureSddl, SDDL_REVISION_1, &Sd, NULL))
        return GetLastError();
    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = Sd;
    Sa.bInheritHandle = FALSE;
    if (!CreateDirectoryW(Path, &Sa) && GetLastError() != ERROR_ALREADY_EXISTS)
        Error = GetLastError();
    LocalFree(Sd);
    return Error;
}

DWORD
BsEnablePrivilege(
    _In_ PCWSTR Privilege)
{
    HANDLE Token;
    TOKEN_PRIVILEGES TokenPrivileges;
    LUID Luid;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token))
        return GetLastError();
    if (!LookupPrivilegeValueW(NULL, Privilege, &Luid))
    {
        CloseHandle(Token);
        return GetLastError();
    }
    TokenPrivileges.PrivilegeCount = 1;
    TokenPrivileges.Privileges[0].Luid = Luid;
    TokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(Token, FALSE, &TokenPrivileges,
                               sizeof(TokenPrivileges), NULL, NULL))
    {
        CloseHandle(Token);
        return GetLastError();
    }
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        CloseHandle(Token);
        return ERROR_PRIVILEGE_NOT_HELD;
    }
    CloseHandle(Token);
    return ERROR_SUCCESS;
}

DWORD
BsTargetInitOnline(
    _Out_ BS_TARGET *Target)
{
    WCHAR WindowsDir[BS_MAX_PATH_BUF];
    WCHAR FsName[32];
    DWORD Error;

    memset(Target, 0, sizeof(*Target));
    Target->IsImage = FALSE;
    Target->VolumeLock = NULL;

    if (!GetWindowsDirectoryW(WindowsDir, RTL_NUMBER_OF(WindowsDir)))
        return GetLastError();
    if (WindowsDir[1] != L':')
        return ERROR_BAD_ENVIRONMENT;
    Target->Root[0] = WindowsDir[0];
    Target->Root[1] = L':';
    Target->Root[2] = 0;

    if (!GetVolumeInformationW(Target->Root, NULL, 0, NULL, NULL, NULL,
                               FsName, RTL_NUMBER_OF(FsName)))
        return GetLastError();
    if (wcscmp(FsName, L"NTFS") != 0)
        return ERROR_NOT_SUPPORTED; /* DACL protection requires NTFS */

    Error = BsEnablePrivilege(SE_RESTORE_NAME);
    if (Error != ERROR_SUCCESS)
        return Error;
    return ERROR_SUCCESS;
}

DWORD
BsTargetInitImage(
    _Out_ BS_TARGET *Target,
    _In_ PCWSTR ImageRoot)
{
    WCHAR Probe[BS_MAX_PATH_BUF];
    DWORD Attr;

    memset(Target, 0, sizeof(*Target));
    Target->IsImage = TRUE;
    Target->VolumeLock = NULL;
    if (wcslen(ImageRoot) >= RTL_NUMBER_OF(Target->Root))
        return ERROR_FILENAME_EXCED_RANGE;
    wcscpy(Target->Root, ImageRoot);
    if (Target->Root[wcslen(Target->Root) - 1] == L'\\')
        Target->Root[wcslen(Target->Root) - 1] = 0;

    if (swprintf(Probe, RTL_NUMBER_OF(Probe),
                 L"%s\\Windows\\System32\\config\\SYSTEM", Target->Root) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    Attr = GetFileAttributesW(Probe);
    if (Attr == INVALID_FILE_ATTRIBUTES || (Attr & FILE_ATTRIBUTE_DIRECTORY))
        return ERROR_DIRECTORY;
    return ERROR_SUCCESS;
}

VOID
BsTargetFree(
    _Inout_ BS_TARGET *Target)
{
    if (Target->VolumeLock && Target->VolumeLock != INVALID_HANDLE_VALUE)
    {
        DWORD Bytes;

        DeviceIoControl(Target->VolumeLock, FSCTL_UNLOCK_VOLUME,
                        NULL, 0, NULL, 0, &Bytes, NULL);
        CloseHandle(Target->VolumeLock);
        Target->VolumeLock = NULL;
    }
}

DWORD
BsBuildTargetPath(
    _Out_ PWSTR Buffer,
    _In_ size_t Cch,
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Relative)
{
    INT Len;

    Len = swprintf(Buffer, Cch, L"%s\\%s", Target->Root, Relative);
    if (Len < 0 || (size_t)Len >= Cch)
        return ERROR_FILENAME_EXCED_RANGE;
    return ERROR_SUCCESS;
}

DWORD
BsEnsureSecureTree(
    _In_ const BS_TARGET *Target)
{
    static const PCWSTR Dirs[] = {
        L"LiberNT",
        BS_SERVICING_DIR,
        BS_STORE_DIR,
        BS_JOURNAL_DIR,
        BS_PENDING_DIR,
        BS_BACKUP_DIR,
        BS_BIN_DIR,
        BS_LOGS_DIR,
        BS_META_DIR,
    };
    ULONG i;
    WCHAR Path[BS_MAX_PATH_BUF];
    DWORD Error;

    for (i = 0; i < RTL_NUMBER_OF(Dirs); i++)
    {
        Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, Dirs[i]);
        if (Error != ERROR_SUCCESS)
            return Error;
        if (GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES)
        {
            /* Existing tree: it must already satisfy the protected-DACL rule. */
            Error = BsCheckSecure(Path);
            if (Error != ERROR_SUCCESS)
                return Error;
            continue;
        }
        Error = BsCreateSecureDir(Path);
        if (Error != ERROR_SUCCESS)
            return Error;
    }
    return ERROR_SUCCESS;
}

DWORD
BsCopyFileToTarget(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR Source,
    _In_ PCWSTR RelativeDest,
    _Out_opt_ char *Sha256Out)
{
    WCHAR Dest[BS_MAX_PATH_BUF];
    HANDLE In, Out;
    uint8_t Buffer[64 * 1024];
    S1_SHA256_CTX Ctx;
    DWORD Read, Written;
    DWORD Error;

    Error = BsBuildTargetPath(Dest, RTL_NUMBER_OF(Dest), Target, RelativeDest);
    if (Error != ERROR_SUCCESS)
        return Error;

    In = CreateFileW(Source, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (In == INVALID_HANDLE_VALUE)
        return GetLastError();
    Out = CreateFileW(Dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
    if (Out == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        CloseHandle(In);
        return Error;
    }
    S1Sha256Init(&Ctx);
    for (;;)
    {
        if (!ReadFile(In, Buffer, sizeof(Buffer), &Read, NULL))
        {
            Error = GetLastError();
            break;
        }
        if (Read == 0)
        {
            Error = ERROR_SUCCESS;
            break;
        }
        S1Sha256Update(&Ctx, Buffer, Read);
        if (!WriteFile(Out, Buffer, Read, &Written, NULL) || Written != Read)
        {
            Error = GetLastError();
            break;
        }
    }
    if (Error == ERROR_SUCCESS && !FlushFileBuffers(Out))
        Error = GetLastError();
    CloseHandle(Out);
    CloseHandle(In);
    if (Error == ERROR_SUCCESS)
    {
        uint8_t Digest[S1_SHA256_SIZE];

        S1Sha256Final(&Ctx, Digest);
        if (Sha256Out)
            S1HexEncode(Digest, S1_SHA256_SIZE, Sha256Out);
    }
    return Error;
}

DWORD
BsHashTargetFile(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath,
    _Out_ char *HexOut)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    HANDLE In;
    uint8_t Buffer[64 * 1024];
    S1_SHA256_CTX Ctx;
    uint8_t Digest[S1_SHA256_SIZE];
    DWORD Read;
    DWORD Error = ERROR_SUCCESS;

    Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, RelativePath);
    if (Error != ERROR_SUCCESS)
        return Error;
    In = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (In == INVALID_HANDLE_VALUE)
        return GetLastError();
    S1Sha256Init(&Ctx);
    for (;;)
    {
        if (!ReadFile(In, Buffer, sizeof(Buffer), &Read, NULL))
        {
            Error = GetLastError();
            break;
        }
        if (Read == 0)
            break;
        S1Sha256Update(&Ctx, Buffer, Read);
    }
    CloseHandle(In);
    if (Error == ERROR_SUCCESS)
    {
        S1Sha256Final(&Ctx, Digest);
        S1HexEncode(Digest, S1_SHA256_SIZE, HexOut);
    }
    return Error;
}

DWORD
BsDeleteTargetFile(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath)
{
    WCHAR Path[BS_MAX_PATH_BUF];
    DWORD Error;

    Error = BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, RelativePath);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (!DeleteFileW(Path))
        return GetLastError();
    return ERROR_SUCCESS;
}

BOOL
BsTargetFileExists(
    _In_ const BS_TARGET *Target,
    _In_ PCWSTR RelativePath)
{
    WCHAR Path[BS_MAX_PATH_BUF];

    if (BsBuildTargetPath(Path, RTL_NUMBER_OF(Path), Target, RelativePath) != ERROR_SUCCESS)
        return FALSE;
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}
