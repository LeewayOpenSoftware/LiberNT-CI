/*
 * PROJECT:     LiberNT logon image check library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Structural verification of a logon binary (see wlimgcheck.h).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>
#include <winnt.h>
#include <winreg.h>
#include <aclapi.h>
#include <sddl.h>
#include <strsafe.h>

#include "wlimgcheck.h"

#if defined(_M_AMD64)
#define WLIMG_NATIVE_MACHINE IMAGE_FILE_MACHINE_AMD64
#define WLIMG_NATIVE_MAGIC   IMAGE_NT_OPTIONAL_HDR64_MAGIC
#elif defined(_M_IX86)
#define WLIMG_NATIVE_MACHINE IMAGE_FILE_MACHINE_I386
#define WLIMG_NATIVE_MAGIC   IMAGE_NT_OPTIONAL_HDR32_MAGIC
#elif defined(_M_ARM64)
#define WLIMG_NATIVE_MACHINE IMAGE_FILE_MACHINE_ARM64
#define WLIMG_NATIVE_MAGIC   IMAGE_NT_OPTIONAL_HDR64_MAGIC
#else
#error Unsupported architecture for wlimgcheck
#endif

/* Access that lets a trustee change or replace the file. */
#define WLIMG_FILE_WRITE_MASK \
    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | \
     DELETE | WRITE_DAC | WRITE_OWNER)

/* Access that lets a trustee add, remove or replace entries in the directory. */
#define WLIMG_DIR_WRITE_MASK \
    (FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER)

#define WLIMG_MAX_LFANEW (4 * 1024 * 1024)

static const WCHAR TrustedInstallerSid[] =
    L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

static BOOL
WlImgpIsAbsoluteWithoutDotDot(
    _In_ PCWSTR Path)
{
    PCWSTR p;

    if (Path == NULL || Path[0] == UNICODE_NULL)
        return FALSE;

    /* Accept "X:\..." and "\\?\X:\..."; reject anything relative. */
    if (Path[0] == L'\\' && Path[1] == L'\\' && Path[2] == L'?' && Path[3] == L'\\')
        Path += 4;
    if (!((Path[0] >= L'A' && Path[0] <= L'Z') || (Path[0] >= L'a' && Path[0] <= L'z')) ||
        Path[1] != L':' || Path[2] != L'\\')
    {
        return FALSE;
    }

    for (p = Path; *p != UNICODE_NULL; p++)
    {
        if (p[0] == L'\\' && p[1] == L'.' && p[2] == L'.' && (p[3] == L'\\' || p[3] == UNICODE_NULL))
            return FALSE;
        if (*p == L'/')
            return FALSE;
    }
    return TRUE;
}

static BOOL
WlImgpIsDirectlyInSystem32(
    _In_ PCWSTR Path)
{
    WCHAR SystemDir[MAX_PATH];
    UINT Len;
    PCWSTR Rest;

    Len = GetSystemDirectoryW(SystemDir, ARRAYSIZE(SystemDir));
    if (Len == 0 || Len >= ARRAYSIZE(SystemDir))
        return FALSE;

    if (Path[0] == L'\\' && Path[1] == L'\\' && Path[2] == L'?' && Path[3] == L'\\')
        Path += 4;

    if (_wcsnicmp(Path, SystemDir, Len) != 0 || Path[Len] != L'\\')
        return FALSE;

    Rest = Path + Len + 1;
    if (*Rest == UNICODE_NULL || wcschr(Rest, L'\\') != NULL)
        return FALSE;

    return TRUE;
}

static BOOL
WlImgpReadAt(
    _In_ HANDLE File,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Size) PVOID Buffer,
    _In_ DWORD Size)
{
    OVERLAPPED Ov;
    DWORD Read;

    ZeroMemory(&Ov, sizeof(Ov));
    Ov.Offset = (DWORD)Offset;
    Ov.OffsetHigh = (DWORD)(Offset >> 32);

    if (!ReadFile(File, Buffer, Size, &Read, &Ov))
        return FALSE;
    return Read == Size;
}

static WLIMG_STATUS
WlImgpVerifyPe(
    _In_ HANDLE File)
{
    IMAGE_DOS_HEADER Dos;
    IMAGE_NT_HEADERS Nt;
    LARGE_INTEGER FileSize;

    if (!GetFileSizeEx(File, &FileSize))
        return WlImgInternalError;

    if (!WlImgpReadAt(File, 0, &Dos, sizeof(Dos)))
        return WlImgNotPe;
    if (Dos.e_magic != IMAGE_DOS_SIGNATURE)
        return WlImgNotPe;
    if (Dos.e_lfanew <= 0 || Dos.e_lfanew > WLIMG_MAX_LFANEW ||
        (ULONGLONG)Dos.e_lfanew + sizeof(Nt) > (ULONGLONG)FileSize.QuadPart)
    {
        return WlImgNotPe;
    }

    if (!WlImgpReadAt(File, (ULONGLONG)Dos.e_lfanew, &Nt, sizeof(Nt)))
        return WlImgNotPe;
    if (Nt.Signature != IMAGE_NT_SIGNATURE)
        return WlImgNotPe;

    if (Nt.FileHeader.Machine != WLIMG_NATIVE_MACHINE ||
        Nt.OptionalHeader.Magic != WLIMG_NATIVE_MAGIC)
    {
        return WlImgWrongMachine;
    }

    if (!(Nt.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        (Nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
        Nt.OptionalHeader.Subsystem != IMAGE_SUBSYSTEM_WINDOWS_GUI)
    {
        return WlImgNotGuiExe;
    }

    return WlImgOk;
}

static BOOL
WlImgpBuildAllowedSids(
    _Out_ PSID *System,
    _Out_ PSID *Admins,
    _Out_ PSID *TrustedInstaller)
{
    DWORD Size;

    *System = NULL;
    *Admins = NULL;
    *TrustedInstaller = NULL;

    Size = SECURITY_MAX_SID_SIZE;
    *System = LocalAlloc(LMEM_FIXED, Size);
    if (*System == NULL || !CreateWellKnownSid(WinLocalSystemSid, NULL, *System, &Size))
        goto Fail;

    Size = SECURITY_MAX_SID_SIZE;
    *Admins = LocalAlloc(LMEM_FIXED, Size);
    if (*Admins == NULL || !CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, *Admins, &Size))
        goto Fail;

    if (!ConvertStringSidToSidW(TrustedInstallerSid, TrustedInstaller))
        goto Fail;

    return TRUE;

Fail:
    if (*System) LocalFree(*System);
    if (*Admins) LocalFree(*Admins);
    if (*TrustedInstaller) LocalFree(*TrustedInstaller);
    *System = *Admins = *TrustedInstaller = NULL;
    return FALSE;
}

/* TRUE when some trustee outside SYSTEM/Administrators/TrustedInstaller gets WriteMask bits. */
static BOOL
WlImgpDaclGrantsWriteToOthers(
    _In_opt_ PACL Dacl,
    _In_ ACCESS_MASK WriteMask)
{
    GENERIC_MAPPING Mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    PSID System, Admins, TrustedInstaller;
    BOOL Result = TRUE;
    DWORD i;

    /* A NULL DACL grants everyone full access. */
    if (Dacl == NULL)
        return TRUE;

    if (!WlImgpBuildAllowedSids(&System, &Admins, &TrustedInstaller))
        return TRUE;

    Result = FALSE;
    for (i = 0; i < Dacl->AceCount; i++)
    {
        PACE_HEADER Header;
        PACCESS_ALLOWED_ACE Ace;
        ACCESS_MASK Mask;
        PSID Sid;

        if (!GetAce(Dacl, i, (PVOID *)&Header))
        {
            Result = TRUE;
            break;
        }

        /* Inherit-only entries do not apply to this object itself. */
        if (Header->AceFlags & INHERIT_ONLY_ACE)
            continue;

        if (Header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;

        Ace = (PACCESS_ALLOWED_ACE)Header;
        Mask = Ace->Mask;
        MapGenericMask(&Mask, &Mapping);
        if ((Mask & WriteMask) == 0)
            continue;

        Sid = (PSID)&Ace->SidStart;
        if (EqualSid(Sid, System) || EqualSid(Sid, Admins) || EqualSid(Sid, TrustedInstaller))
            continue;

        Result = TRUE;
        break;
    }

    LocalFree(System);
    LocalFree(Admins);
    LocalFree(TrustedInstaller);
    return Result;
}

static WLIMG_STATUS
WlImgpVerifyObjectDacl(
    _In_ HANDLE Object,
    _In_ ACCESS_MASK WriteMask,
    _In_ WLIMG_STATUS WritableStatus)
{
    PACL Dacl = NULL;
    PSECURITY_DESCRIPTOR Sd = NULL;
    WLIMG_STATUS Status;

    if (GetSecurityInfo(Object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                        NULL, NULL, &Dacl, NULL, &Sd) != ERROR_SUCCESS)
    {
        return WlImgInternalError;
    }

    Status = WlImgpDaclGrantsWriteToOthers(Dacl, WriteMask) ? WritableStatus : WlImgOk;

    LocalFree(Sd);
    return Status;
}

static WLIMG_STATUS
WlImgpVerifyDirectory(
    _In_ PCWSTR ImagePath)
{
    WCHAR Dir[MAX_PATH];
    PWSTR LastSlash;
    HANDLE DirHandle;
    WLIMG_STATUS Status;

    if (FAILED(StringCchCopyW(Dir, ARRAYSIZE(Dir), ImagePath)))
        return WlImgBadPath;

    LastSlash = wcsrchr(Dir, L'\\');
    if (LastSlash == NULL || LastSlash == Dir)
        return WlImgBadPath;
    /* Keep the root backslash ("C:\"), drop it elsewhere. */
    if (LastSlash[-1] == L':')
        LastSlash[1] = UNICODE_NULL;
    else
        LastSlash[0] = UNICODE_NULL;

    DirHandle = CreateFileW(Dir, READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (DirHandle == INVALID_HANDLE_VALUE)
        return WlImgOpenFailed;

    Status = WlImgpVerifyObjectDacl(DirHandle, WLIMG_DIR_WRITE_MASK, WlImgDirectoryWritable);
    CloseHandle(DirHandle);
    return Status;
}

WLIMG_STATUS
NTAPI
WlImgOpenVerified(
    _In_ PCWSTR ImagePath,
    _In_ DWORD Flags,
    _Out_ PHANDLE ImageHandle)
{
    HANDLE File;
    BY_HANDLE_FILE_INFORMATION Info;
    WLIMG_STATUS Status;

    *ImageHandle = INVALID_HANDLE_VALUE;

    if (!WlImgpIsAbsoluteWithoutDotDot(ImagePath))
        return WlImgBadPath;
    if ((Flags & WLIMG_REQUIRE_SYSTEM32) && !WlImgpIsDirectlyInSystem32(ImagePath))
        return WlImgBadPath;

    /* No write or delete sharing: nothing can replace or rename the file while
     * the handle is open, and the caller holds it until CreateProcess returns. */
    File = CreateFileW(ImagePath, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return WlImgOpenFailed;

    if (!GetFileInformationByHandle(File, &Info))
    {
        Status = WlImgInternalError;
        goto Fail;
    }
    if (Info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
    {
        Status = WlImgBadPath;
        goto Fail;
    }

    Status = WlImgpVerifyPe(File);
    if (Status != WlImgOk)
        goto Fail;

    Status = WlImgpVerifyObjectDacl(File, WLIMG_FILE_WRITE_MASK, WlImgFileWritable);
    if (Status != WlImgOk)
        goto Fail;

    if (Flags & WLIMG_CHECK_DIRECTORY)
    {
        Status = WlImgpVerifyDirectory(ImagePath);
        if (Status != WlImgOk)
            goto Fail;
    }

    *ImageHandle = File;
    return WlImgOk;

Fail:
    CloseHandle(File);
    return Status;
}

PCWSTR
NTAPI
WlImgStatusName(
    _In_ WLIMG_STATUS Status)
{
    switch (Status)
    {
        case WlImgOk:                return L"ok";
        case WlImgBadPath:           return L"bad-path";
        case WlImgOpenFailed:        return L"open-failed";
        case WlImgNotPe:             return L"not-pe";
        case WlImgWrongMachine:      return L"wrong-machine";
        case WlImgNotGuiExe:         return L"not-gui-exe";
        case WlImgFileWritable:      return L"file-writable";
        case WlImgDirectoryWritable: return L"directory-writable";
        default:                     return L"internal-error";
    }
}
