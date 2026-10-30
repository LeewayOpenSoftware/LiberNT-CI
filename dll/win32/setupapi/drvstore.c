/*
 * PROJECT:     LiberNT Setup API
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver store staging for published OEM driver packages
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "setupapi_private.h"

/* LiberNT-private driver store enumeration contracts. These entry points
 * have no public Windows prototype (the SDK does not declare them); they are
 * exported under Ln-prefixed names so no colliding Windows name is given a
 * body with an assumed signature. */
typedef BOOL (CALLBACK *INFENUMPROCA)(PCSTR PublishedInfName, PVOID Context);
typedef BOOL (CALLBACK *INFENUMPROCW)(PCWSTR PublishedInfName, PVOID Context);

BOOL
WINAPI
LnSetupEnumPublishedInfW(
    IN INFENUMPROCW Callback,
    IN PVOID Context OPTIONAL,
    IN DWORD Flags);

BOOL
WINAPI
LnSetupEnumPublishedInfA(
    IN INFENUMPROCA Callback,
    IN PVOID Context OPTIONAL,
    IN DWORD Flags);

HRESULT
WINAPI
LnDriverStoreEnumDriverPackageW(
    IN PCWSTR EnumFilter OPTIONAL,
    IN PVOID Reserved1,
    IN WORD Architecture,
    OUT PWSTR ReturnPath,
    IN OUT PDWORD ReturnLength,
    IN OUT PDWORD Index);

static BOOL
IsPackageDirectoryName(
    IN PCWSTR PackageName,
    OUT PWSTR InfBaseName OPTIONAL,
    IN DWORD InfBaseNameSize);

static VOID
RemovePublishedInfEntries(
    IN PCWSTR StoreInfFileName,
    IN BOOL DeleteFiles);

static const WCHAR RepositoryPath[] = L"\\System32\\DriverStore\\FileRepository";
static const WCHAR VersionSection[] = L"Version";
static const WCHAR CatalogFile[] = L"CatalogFile";

static PCWSTR
GetStoreArchitecture(VOID)
{
    SYSTEM_INFO SystemInfo;

    GetNativeSystemInfo(&SystemInfo);
    switch (SystemInfo.wProcessorArchitecture)
    {
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
        case PROCESSOR_ARCHITECTURE_AMD64: return L"amd64";
        case PROCESSOR_ARCHITECTURE_ARM:   return L"arm";
        case PROCESSOR_ARCHITECTURE_ARM64: return L"arm64";
        case PROCESSOR_ARCHITECTURE_IA64:  return L"ia64";
        case PROCESSOR_ARCHITECTURE_RISCV64: return L"riscv64";
        case PROCESSOR_ARCHITECTURE_PPC:   return L"ppc";
        default:                           return L"unknown";
    }
}

static BOOL
GetRepositoryRoot(
    OUT PWSTR Buffer,
    IN DWORD BufferSize)
{
    UINT Length = GetWindowsDirectoryW(Buffer, BufferSize);

    if (Length == 0 || Length + ARRAY_SIZE(RepositoryPath) > BufferSize)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    lstrcatW(Buffer, RepositoryPath);
    return TRUE;
}

static BOOL
HashFileContents(
    IN PCWSTR FileName,
    OUT PULONGLONG Hash)
{
    BYTE Buffer[4096];
    ULONGLONG Value = 0xcbf29ce484222325ULL;
    DWORD Read, i;
    HANDLE hFile;
    BOOL Result = TRUE;

    hFile = CreateFileW(FileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    for (;;)
    {
        if (!ReadFile(hFile, Buffer, sizeof(Buffer), &Read, NULL))
        {
            Result = FALSE;
            break;
        }
        if (Read == 0)
            break;

        for (i = 0; i < Read; i++)
        {
            Value ^= Buffer[i];
            Value *= 0x100000001b3ULL;
        }
    }

    CloseHandle(hFile);
    *Hash = Value;
    return Result;
}

static BOOL
FilesAreIdentical(
    IN PCWSTR FirstName,
    IN PCWSTR SecondName)
{
    BYTE First[4096], Second[4096];
    DWORD FirstRead, SecondRead;
    HANDLE hFirst, hSecond;
    BOOL Result = FALSE;

    hFirst = CreateFileW(FirstName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, 0, NULL);
    if (hFirst == INVALID_HANDLE_VALUE)
        return FALSE;

    hSecond = CreateFileW(SecondName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, OPEN_EXISTING, 0, NULL);
    if (hSecond == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFirst);
        return FALSE;
    }

    for (;;)
    {
        if (!ReadFile(hFirst, First, sizeof(First), &FirstRead, NULL) ||
            !ReadFile(hSecond, Second, sizeof(Second), &SecondRead, NULL) ||
            FirstRead != SecondRead)
        {
            break;
        }
        if (FirstRead == 0)
        {
            Result = TRUE;
            break;
        }
        if (memcmp(First, Second, FirstRead) != 0)
            break;
    }

    CloseHandle(hSecond);
    CloseHandle(hFirst);
    return Result;
}

static BOOL
CreateDirectoryPath(
    IN PCWSTR Path)
{
    WCHAR Buffer[MAX_PATH];
    PWSTR Cursor;
    DWORD Attributes;

    if (lstrlenW(Path) >= MAX_PATH)
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    lstrcpyW(Buffer, Path);

    for (Cursor = Buffer + 3; ; Cursor++)
    {
        WCHAR Saved = *Cursor;

        if (Saved != L'\\' && Saved != UNICODE_NULL)
            continue;

        *Cursor = UNICODE_NULL;
        if (!CreateDirectoryW(Buffer, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
        if (Saved == UNICODE_NULL)
            break;
        *Cursor = Saved;
    }

    Attributes = GetFileAttributesW(Buffer);
    if (Attributes == INVALID_FILE_ATTRIBUTES || !(Attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

static BOOL
CombinePath(
    OUT PWSTR Buffer,
    IN PCWSTR Root,
    IN PCWSTR Relative)
{
    DWORD RootLength = lstrlenW(Root);

    while (*Relative == L'\\')
        Relative++;

    if (RootLength + 1 + lstrlenW(Relative) >= MAX_PATH)
        return FALSE;

    lstrcpyW(Buffer, Root);
    if (RootLength && Buffer[RootLength - 1] != L'\\' && *Relative)
        lstrcatW(Buffer, L"\\");
    lstrcatW(Buffer, Relative);
    return TRUE;
}

static BOOL
IsSafeRelativePath(
    IN PCWSTR Relative)
{
    PCWSTR Cursor = Relative;

    if (wcschr(Relative, L':'))
        return FALSE;

    while (*Cursor)
    {
        PCWSTR End = wcschr(Cursor, L'\\');
        SIZE_T Length = End ? (SIZE_T)(End - Cursor) : wcslen(Cursor);

        if (Length == 2 && Cursor[0] == L'.' && Cursor[1] == L'.')
            return FALSE;
        if (!End)
            break;
        Cursor = End + 1;
    }
    return TRUE;
}

static BOOL
GetCompressedSourceName(
    IN PCWSTR Source,
    OUT PWSTR Compressed,
    IN DWORD CompressedSize)
{
    PWSTR Name, Extension;
    SIZE_T Length = wcslen(Source);

    if (Length + 3 > CompressedSize)
        return FALSE;

    wcscpy(Compressed, Source);
    Name = wcsrchr(Compressed, L'\\');
    Name = Name ? Name + 1 : Compressed;
    Extension = wcschr(Name, L'.');
    if (!Extension)
        wcscat(Compressed, L"._");
    else if (wcslen(Extension + 1) < 3)
        wcscat(Compressed, L"_");
    else
        Compressed[Length - 1] = L'_';

    return TRUE;
}

typedef struct _STAGE_NAME_LIST
{
    PWSTR *Names;
    DWORD Count;
    DWORD Capacity;
} STAGE_NAME_LIST, *PSTAGE_NAME_LIST;

typedef struct _STAGE_PACKAGE_CONTEXT
{
    PCWSTR SourceRoot;
    PCWSTR StoreRoot;
    STAGE_NAME_LIST Sections;
    STAGE_NAME_LIST Files;
    /* Verification pass (LnSetupVerifyDriverPackage): walk the same file set
     * and check each staged member against the package catalog instead of
     * copying it. */
    BOOL VerifyOnly;
    HCATINFO Catalog;
    DWORD SigState;
    WCHAR MismatchFile[MAX_PATH];
} STAGE_PACKAGE_CONTEXT, *PSTAGE_PACKAGE_CONTEXT;

static BOOL
MarkNameSeen(
    IN OUT PSTAGE_NAME_LIST List,
    IN PCWSTR Name)
{
    PWSTR Copy;
    DWORD i;

    for (i = 0; i < List->Count; i++)
    {
        if (!_wcsicmp(List->Names[i], Name))
            return FALSE;
    }

    if (List->Count == List->Capacity)
    {
        DWORD Capacity = List->Capacity ? List->Capacity * 2 : 128;
        PWSTR *Names = List->Names ? MyRealloc(List->Names, Capacity * sizeof(PWSTR))
                                   : MyMalloc(Capacity * sizeof(PWSTR));

        if (!Names)
            return TRUE;
        List->Names = Names;
        List->Capacity = Capacity;
    }

    Copy = MyMalloc((lstrlenW(Name) + 1) * sizeof(WCHAR));
    if (!Copy)
        return TRUE;
    lstrcpyW(Copy, Name);
    List->Names[List->Count++] = Copy;
    return TRUE;
}

static VOID
FreeNameList(
    IN OUT PSTAGE_NAME_LIST List)
{
    DWORD i;

    for (i = 0; i < List->Count; i++)
        MyFree(List->Names[i]);
    MyFree(List->Names);
    List->Names = NULL;
    List->Count = List->Capacity = 0;
}

typedef struct _STAGE_CABINET_CONTEXT
{
    PCWSTR FileName;
    PCWSTR Target;
} STAGE_CABINET_CONTEXT, *PSTAGE_CABINET_CONTEXT;

static UINT CALLBACK
StageCabinetCallback(
    IN PVOID Context,
    IN UINT Notification,
    IN UINT_PTR Param1,
    IN UINT_PTR Param2)
{
    PSTAGE_CABINET_CONTEXT Stage = Context;

    switch (Notification)
    {
        case SPFILENOTIFY_FILEINCABINET:
        {
            PFILE_IN_CABINET_INFO_W Info = (PFILE_IN_CABINET_INFO_W)Param1;
            PCWSTR Name = wcsrchr(Info->NameInCabinet, L'\\');

            Name = Name ? Name + 1 : Info->NameInCabinet;
            if (_wcsicmp(Name, Stage->FileName) != 0)
                return FILEOP_SKIP;

            lstrcpynW(Info->FullTargetName, Stage->Target, ARRAY_SIZE(Info->FullTargetName));
            return FILEOP_DOIT;
        }

        case SPFILENOTIFY_FILEEXTRACTED:
            return ((PFILEPATHS_W)Param1)->Win32Error;

        case SPFILENOTIFY_NEEDNEWCABINET:
            lstrcpyW((PWSTR)Param2, ((PCABINET_INFO_W)Param1)->CabinetPath);
            return NO_ERROR;

        default:
            return NO_ERROR;
    }
}

static VOID
StagePackageFile(
    IN PCWSTR SourceRoot,
    IN PCWSTR StoreRoot,
    IN PCWSTR Relative,
    IN PCWSTR Cabinet OPTIONAL)
{
    WCHAR Source[MAX_PATH], Target[MAX_PATH], Compressed[MAX_PATH];
    STAGE_CABINET_CONTEXT Stage;
    PWSTR Last;
    DWORD Error;

    if (!IsSafeRelativePath(Relative) ||
        !CombinePath(Source, SourceRoot, Relative) ||
        !CombinePath(Target, StoreRoot, Relative))
    {
        return;
    }

    Compressed[0] = UNICODE_NULL;
    if (GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES)
    {
        if (!GetCompressedSourceName(Source, Compressed, ARRAY_SIZE(Compressed)) ||
            GetFileAttributesW(Compressed) == INVALID_FILE_ATTRIBUTES)
        {
            Compressed[0] = UNICODE_NULL;
            if (!Cabinet || GetFileAttributesW(Cabinet) == INVALID_FILE_ATTRIBUTES)
                return;
        }
        else
        {
            Cabinet = NULL;
        }
    }
    else
    {
        Cabinet = NULL;
    }

    Last = wcsrchr(Target, L'\\');
    if (Last)
    {
        *Last = UNICODE_NULL;
        if (!CreateDirectoryPath(Target))
            return;
        *Last = L'\\';
    }

    if (Cabinet)
    {
        Stage.FileName = wcsrchr(Relative, L'\\');
        Stage.FileName = Stage.FileName ? Stage.FileName + 1 : Relative;
        Stage.Target = Target;
        if (!SetupIterateCabinetW(Cabinet, 0, StageCabinetCallback, &Stage))
            TRACE("Extracting %s from %s failed with error %lu\n", debugstr_w(Relative), debugstr_w(Cabinet), GetLastError());
    }
    else if (Compressed[0])
    {
        Error = SetupDecompressOrCopyFileW(Compressed, Target, NULL);
        if (Error != ERROR_SUCCESS)
            TRACE("Expanding %s failed with error %lu\n", debugstr_w(Compressed), Error);
    }
    else if (!CopyFileW(Source, Target, FALSE))
    {
        TRACE("Staging %s failed with error %lu\n", debugstr_w(Source), GetLastError());
    }
}

static VOID
VerifyPackageMember(
    IN OUT PSTAGE_PACKAGE_CONTEXT Package,
    IN PCWSTR Relative,
    IN BOOL FromCabinet);

static VOID
StageCopyFile(
    IN HINF hInf,
    IN PCWSTR FileName,
    IN OUT PSTAGE_PACKAGE_CONTEXT Package)
{
    WCHAR SubDir[MAX_PATH], DiskPath[MAX_PATH], Tag[MAX_PATH], Relative[MAX_PATH], Cabinet[MAX_PATH];
    PCWSTR CabinetPath = NULL;
    SIZE_T Length;
    UINT DiskId;

    if (!MarkNameSeen(&Package->Files, FileName))
        return;

    SubDir[0] = UNICODE_NULL;
    if (!SetupGetSourceFileLocationW(hInf, NULL, FileName, &DiskId, SubDir, ARRAY_SIZE(SubDir), NULL))
        return;

    DiskPath[0] = UNICODE_NULL;
    SetupGetSourceInfoW(hInf, DiskId, SRCINFO_PATH, DiskPath, ARRAY_SIZE(DiskPath), NULL);
    Tag[0] = UNICODE_NULL;
    SetupGetSourceInfoW(hInf, DiskId, SRCINFO_TAGFILE, Tag, ARRAY_SIZE(Tag), NULL);

    if (!CombinePath(Relative, DiskPath, SubDir) ||
        lstrlenW(Relative) + 1 + lstrlenW(FileName) >= MAX_PATH)
    {
        return;
    }
    if (Relative[0] && Relative[lstrlenW(Relative) - 1] != L'\\')
        lstrcatW(Relative, L"\\");
    lstrcatW(Relative, FileName);

    Length = wcslen(Tag);
    if (Length > 4 && _wcsicmp(Tag + Length - 4, L".cab") == 0 &&
        IsSafeRelativePath(DiskPath) && IsSafeRelativePath(Tag) &&
        CombinePath(Cabinet, Package->SourceRoot, DiskPath) &&
        lstrlenW(Cabinet) + 1 + Length < MAX_PATH)
    {
        if (Cabinet[0] && Cabinet[lstrlenW(Cabinet) - 1] != L'\\')
            lstrcatW(Cabinet, L"\\");
        lstrcatW(Cabinet, Tag);
        CabinetPath = Cabinet;
    }

    if (Package->VerifyOnly)
    {
        VerifyPackageMember(Package, Relative, CabinetPath != NULL);
        return;
    }

    StagePackageFile(Package->SourceRoot, Package->StoreRoot, Relative, CabinetPath);
}

static const struct
{
    PCSTR Oid;
    PCWSTR Algorithm;
} CatalogHashAlgorithms[] =
{
    { szOID_OIWSEC_sha1, L"SHA1" },
    { szOID_RSA_MD5, L"MD5" },
    { szOID_NIST_sha256, L"SHA256" },
    { szOID_NIST_sha384, L"SHA384" },
    { szOID_NIST_sha512, L"SHA512" },
};

static BOOL
VerifyFileHashWithAlgorithm(
    IN HANDLE hFile,
    IN CRYPTCATMEMBER *Member)
{
    HCATADMIN Admin = NULL;
    DWORD HashSize = 0;
    PBYTE Hash = NULL;
    PCWSTR Algorithm = NULL;
    UINT i;
    BOOL Found = FALSE;

    if (!Member->pIndirectData)
        return FALSE;

    for (i = 0; i < ARRAY_SIZE(CatalogHashAlgorithms); i++)
    {
        if (Member->pIndirectData->DigestAlgorithm.pszObjId &&
            strcmp(Member->pIndirectData->DigestAlgorithm.pszObjId,
                   CatalogHashAlgorithms[i].Oid) == 0)
        {
            Algorithm = CatalogHashAlgorithms[i].Algorithm;
            break;
        }
    }
    if (!Algorithm)
        return FALSE;

    if (!CryptCATAdminAcquireContext2(&Admin, NULL, Algorithm, NULL, 0))
        return FALSE;

    if (CryptCATAdminCalcHashFromFileHandle2(Admin, hFile, &HashSize, NULL, 0) && HashSize)
    {
        Hash = MyMalloc(HashSize);
        if (Hash &&
            CryptCATAdminCalcHashFromFileHandle2(Admin, hFile, &HashSize, Hash, 0) &&
            Member->pIndirectData->Digest.cbData == HashSize &&
            RtlEqualMemory(Member->pIndirectData->Digest.pbData, Hash, HashSize))
        {
            Found = TRUE;
        }
    }

    if (Hash)
        MyFree(Hash);
    CryptCATAdminReleaseContext(Admin, 0);
    return Found;
}

static VOID
VerifyPackageMember(
    IN OUT PSTAGE_PACKAGE_CONTEXT Package,
    IN PCWSTR Relative,
    IN BOOL FromCabinet)
{
    WCHAR FullPath[MAX_PATH];
    HANDLE hFile;
    CRYPTCATMEMBER *Member;
    BOOL Found = FALSE;

    /* Cabinet members are verified with the cabinet as a whole; the per-file
     * catalog check applies to plain files only. */
    if (FromCabinet || Package->SigState == PACKAGE_SIG_CATALOG_INVALID)
        return;

    if (!CombinePath(FullPath, Package->SourceRoot, Relative))
        return;

    hFile = CreateFileW(FullPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        Package->SigState = PACKAGE_SIG_HASH_MISMATCH;
        if (!Package->MismatchFile[0])
            lstrcpynW(Package->MismatchFile, Relative, MAX_PATH);
        return;
    }

    Member = NULL;
    while ((Member = CryptCATEnumerateMember(Package->Catalog, Member)))
    {
        if (VerifyFileHashWithAlgorithm(hFile, Member))
        {
            Found = TRUE;
            break;
        }
    }

    CloseHandle(hFile);

    if (!Found)
    {
        Package->SigState = PACKAGE_SIG_HASH_MISMATCH;
        if (!Package->MismatchFile[0])
            lstrcpynW(Package->MismatchFile, Relative, MAX_PATH);
    }
}

static VOID
StageCopyFilesSection(
    IN HINF hInf,
    IN PCWSTR Section,
    IN OUT PSTAGE_PACKAGE_CONTEXT Package)
{
    WCHAR Target[MAX_PATH], Source[MAX_PATH];
    INFCONTEXT Context;

    if (Section[0] == L'@')
    {
        StageCopyFile(hInf, Section + 1, Package);
        return;
    }

    if (!MarkNameSeen(&Package->Sections, Section) ||
        !SetupFindFirstLineW(hInf, Section, NULL, &Context))
        return;

    do
    {
        if (!SetupGetStringFieldW(&Context, 1, Target, ARRAY_SIZE(Target), NULL) || !Target[0])
            continue;
        if (!SetupGetStringFieldW(&Context, 2, Source, ARRAY_SIZE(Source), NULL) || !Source[0])
            lstrcpyW(Source, Target);

        StageCopyFile(hInf, Source, Package);
    } while (SetupFindNextLine(&Context, &Context));
}

static VOID
StageCopyFiles(
    IN HINF hInf,
    IN OUT PSTAGE_PACKAGE_CONTEXT Package)
{
    WCHAR Section[MAX_INF_SECTION_NAME_LENGTH + 1], Files[MAX_INF_SECTION_NAME_LENGTH + 1];
    DWORD Index, Field, FieldCount;
    INFCONTEXT Context;
    BOOL Found;

    for (Index = 0; SetupEnumInfSectionsW(hInf, Index, Section, ARRAY_SIZE(Section), NULL); Index++)
    {
        for (Found = SetupFindFirstLineW(hInf, Section, L"CopyFiles", &Context);
             Found;
             Found = SetupFindNextMatchLineW(&Context, L"CopyFiles", &Context))
        {
            FieldCount = SetupGetFieldCount(&Context);
            for (Field = 1; Field <= FieldCount; Field++)
            {
                if (SetupGetStringFieldW(&Context, Field, Files, ARRAY_SIZE(Files), NULL) && Files[0])
                    StageCopyFilesSection(hInf, Files, Package);
            }
        }
    }
}

static VOID
StageCatalogFiles(
    IN HINF hInf,
    IN PCWSTR SourceRoot,
    IN PCWSTR StoreRoot)
{
    WCHAR Key[LINE_LEN], Value[MAX_PATH];
    INFCONTEXT Context;

    if (!SetupFindFirstLineW(hInf, VersionSection, NULL, &Context))
        return;

    do
    {
        if (!SetupGetStringFieldW(&Context, 0, Key, ARRAY_SIZE(Key), NULL) ||
            _wcsnicmp(Key, CatalogFile, ARRAY_SIZE(CatalogFile) - 1) != 0)
        {
            continue;
        }

        if (SetupGetStringFieldW(&Context, 1, Value, ARRAY_SIZE(Value), NULL) && Value[0])
            StagePackageFile(SourceRoot, StoreRoot, Value, NULL);
    } while (SetupFindNextLine(&Context, &Context));
}

BOOL
SETUPAPI_GetDriverPackageId(
    IN PCWSTR InfFileName,
    IN PCWSTR InfBaseName,
    OUT PWSTR PackageId,
    IN DWORD PackageIdSize)
{
    ULONGLONG Hash;

    if (!HashFileContents(InfFileName, &Hash))
        return FALSE;

    if ((DWORD)lstrlenW(InfBaseName) + 32 >= PackageIdSize)
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }

    swprintf(PackageId, PackageIdSize, L"%s_%s_%016I64x", InfBaseName, GetStoreArchitecture(), Hash);
    CharLowerW(PackageId);
    return TRUE;
}

static BOOL
BuildStoreDirectory(
    IN PCWSTR InfFileName,
    IN PCWSTR InfBaseName,
    OUT PWSTR StoreDirectory)
{
    WCHAR Root[MAX_PATH], Name[MAX_PATH];

    if (!GetRepositoryRoot(Root, ARRAY_SIZE(Root)) ||
        !SETUPAPI_GetDriverPackageId(InfFileName, InfBaseName, Name, ARRAY_SIZE(Name)))
    {
        return FALSE;
    }

    if (!CombinePath(StoreDirectory, Root, Name))
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    return TRUE;
}

static BOOL
IsSystemInfDirectory(
    IN PCWSTR Directory)
{
    WCHAR InfDirectory[MAX_PATH];
    SIZE_T Length;
    UINT Result;

    Result = GetSystemWindowsDirectoryW(InfDirectory, ARRAY_SIZE(InfDirectory));
    if (Result == 0 || Result + 5 >= ARRAY_SIZE(InfDirectory))
        return FALSE;
    lstrcatW(InfDirectory, L"\\inf");

    Length = lstrlenW(InfDirectory);
    return _wcsnicmp(Directory, InfDirectory, Length) == 0 &&
           (Directory[Length] == UNICODE_NULL ||
            (Directory[Length] == L'\\' && Directory[Length + 1] == UNICODE_NULL));
}

BOOL
SETUPAPI_StageDriverPackage(
    IN PCWSTR SourceInfFileName)
{
    WCHAR Source[MAX_PATH], SourceRoot[MAX_PATH], Root[MAX_PATH], Store[MAX_PATH], Target[MAX_PATH];
    STAGE_PACKAGE_CONTEXT Package;
    PWSTR BaseName;
    HINF hInf;
    DWORD Length;

    Length = GetFullPathNameW(SourceInfFileName, ARRAY_SIZE(Source), Source, &BaseName);
    if (Length == 0 || Length >= ARRAY_SIZE(Source) || !BaseName)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    lstrcpynW(SourceRoot, Source, (INT)(BaseName - Source) + 1);

    if (!GetRepositoryRoot(Root, ARRAY_SIZE(Root)))
        return FALSE;
    if (_wcsnicmp(SourceRoot, Root, lstrlenW(Root)) == 0 || IsSystemInfDirectory(SourceRoot))
        return TRUE;

    if (!BuildStoreDirectory(Source, BaseName, Store) ||
        !CombinePath(Target, Store, BaseName))
    {
        return FALSE;
    }

    if (GetFileAttributesW(Target) != INVALID_FILE_ATTRIBUTES && FilesAreIdentical(Source, Target))
        return TRUE;

    if (!CreateDirectoryPath(Store))
        return FALSE;

    hInf = SetupOpenInfFileW(Source, NULL, INF_STYLE_WIN4, NULL);
    if (hInf != INVALID_HANDLE_VALUE)
    {
        StageCatalogFiles(hInf, SourceRoot, Store);

        ZeroMemory(&Package, sizeof(Package));
        Package.SourceRoot = SourceRoot;
        Package.StoreRoot = Store;
        StageCopyFiles(hInf, &Package);
        FreeNameList(&Package.Sections);
        FreeNameList(&Package.Files);

        SetupCloseInfFile(hInf);
    }

    if (!CopyFileW(Source, Target, FALSE))
        return FALSE;

    if (SetupapiGetVerifyPolicy() != 0)
    {
        DWORD SigState = PACKAGE_SIG_UNSIGNED;
        WCHAR Mismatch[MAX_PATH] = { 0 };
        DWORD Error;

        Error = LnSetupVerifyDriverPackage(Target, &SigState, Mismatch, ARRAY_SIZE(Mismatch));
        if (Error == ERROR_SUCCESS)
        {
            TRACE("staged package %s signature state %lu\n", debugstr_w(Target), SigState);

            if (SetupapiGetVerifyPolicy() == 2 &&
                (SigState == PACKAGE_SIG_HASH_MISMATCH || SigState == PACKAGE_SIG_CATALOG_INVALID))
            {
                SetLastError(ERROR_INVALID_IMAGE_HASH);
                return FALSE;
            }
        }
    }

    return TRUE;
}

/* Política de verificación de firma de paquetes (M6).
 * HKLM\SYSTEM\CurrentControlSet\Control\DrvInst\VerifyPackageSignature:
 * 0 = off, 1 = audit (default, verifica y registra pero no bloquea),
 * 2 = enforce (rechaza paquetes con hash que no coincide o catálogo inválido).
 * Los paquetes sin catálogo siguen permitidos en ambos modos. */
DWORD
SetupapiGetVerifyPolicy(VOID)
{
    HKEY Key;
    DWORD Value = 1, Size = sizeof(Value), Type;
    LONG Error;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\DrvInst",
                          0, KEY_QUERY_VALUE, &Key);
    if (Error != ERROR_SUCCESS)
        return 1;

    Error = RegQueryValueExW(Key, L"VerifyPackageSignature", NULL, &Type,
                             (PBYTE)&Value, &Size);
    RegCloseKey(Key);

    if (Error != ERROR_SUCCESS || Type != REG_DWORD || Value > 2)
        return 1;
    return Value;
}

/* Verifica el paquete de drivers al que pertenece InfPath: cada archivo
 * referenciado por el INF debe aparecer en el catálogo [Version] CatalogFile
 * con el hash calculado por CryptCATAdminCalcHashFromFileHandle.
 * Devuelve ERROR_SUCCESS y el estado; el estado es un dato (Unsigned no es
 * error). Reusable por LiberNT Update. */
static HCATINFO
OpenInfCatalog(
    IN HINF hInf,
    IN PCWSTR SourceRoot,
    OUT PWSTR CatalogPath,
    IN DWORD CatalogPathCch,
    OUT PDWORD Error);

DWORD
WINAPI
LnSetupVerifyDriverPackage(
    IN PCWSTR InfPath,
    OUT PDWORD SigState,
    OUT PWSTR MismatchFile OPTIONAL,
    IN DWORD MismatchFileCch OPTIONAL)
{
    WCHAR Source[MAX_PATH], SourceRoot[MAX_PATH], CatalogPath[MAX_PATH];
    STAGE_PACKAGE_CONTEXT Package;
    PWSTR BaseName;
    HINF hInf;
    HCATINFO Catalog;
    DWORD Length, Error;

    if (!InfPath || !SigState)
        return ERROR_INVALID_PARAMETER;
    *SigState = PACKAGE_SIG_UNSIGNED;
    if (MismatchFile && MismatchFileCch)
        MismatchFile[0] = UNICODE_NULL;

    Length = GetFullPathNameW(InfPath, ARRAY_SIZE(Source), Source, &BaseName);
    if (Length == 0 || Length >= ARRAY_SIZE(Source) || !BaseName)
        return ERROR_INVALID_PARAMETER;

    lstrcpynW(SourceRoot, Source, (INT)(BaseName - Source) + 1);

    hInf = SetupOpenInfFileW(Source, NULL, INF_STYLE_WIN4, NULL);
    if (hInf == INVALID_HANDLE_VALUE)
        return GetLastError();

    Catalog = OpenInfCatalog(hInf, SourceRoot, CatalogPath, ARRAY_SIZE(CatalogPath), &Error);
    if (!Catalog)
    {
        if (Error != ERROR_SUCCESS)
        {
            SetupCloseInfFile(hInf);
            return Error;
        }
        /* Paquete sin catálogo: estado Unsigned, no es un error. */
        SetupCloseInfFile(hInf);
        return ERROR_SUCCESS;
    }
    if (Catalog == INVALID_HANDLE_VALUE)
    {
        *SigState = PACKAGE_SIG_CATALOG_INVALID;
        SetupCloseInfFile(hInf);
        return ERROR_SUCCESS;
    }

    ZeroMemory(&Package, sizeof(Package));
    Package.SourceRoot = SourceRoot;
    Package.StoreRoot = SourceRoot;
    Package.VerifyOnly = TRUE;
    Package.Catalog = Catalog;
    Package.SigState = PACKAGE_SIG_SIGNED_VALID;

    StageCopyFiles(hInf, &Package);
    FreeNameList(&Package.Sections);
    FreeNameList(&Package.Files);

    *SigState = Package.SigState;
    if (MismatchFile && MismatchFileCch && Package.MismatchFile[0])
        lstrcpynW(MismatchFile, Package.MismatchFile, MismatchFileCch);

    CryptCATClose(Catalog);
    SetupCloseInfFile(hInf);
    return ERROR_SUCCESS;
}

/* Localiza y abre el catálogo que el INF declara en [Version] CatalogFile.
 * Devuelve NULL sin error cuando el INF no declara catálogo (Unsigned);
 * INVALID_HANDLE_VALUE cuando el catálogo existe pero no se pudo abrir. */
static HCATINFO
OpenInfCatalog(
    IN HINF hInf,
    IN PCWSTR SourceRoot,
    OUT PWSTR CatalogPath,
    IN DWORD CatalogPathCch,
    OUT PDWORD Error)
{
    INFCONTEXT Context;

    if (!SetupFindFirstLineW(hInf, L"Version", NULL, &Context))
    {
        *Error = ERROR_SUCCESS;
        return NULL;
    }

    do
    {
        WCHAR Key[LINE_LEN];
        WCHAR Value[MAX_PATH];

        if (!SetupGetStringFieldW(&Context, 0, Key, ARRAY_SIZE(Key), NULL) ||
            _wcsnicmp(Key, L"CatalogFile", 11) != 0)
            continue;
        if (!SetupGetStringFieldW(&Context, 1, Value, ARRAY_SIZE(Value), NULL) || !Value[0])
            continue;
        if (!CombinePath(CatalogPath, SourceRoot, Value))
        {
            *Error = ERROR_FILENAME_EXCED_RANGE;
            return NULL;
        }
        *Error = ERROR_SUCCESS;
        return CryptCATOpen(CatalogPath, CRYPTCAT_OPEN_EXISTING, 0, 0, 0);
    } while (SetupFindNextLine(&Context, &Context));

    *Error = ERROR_SUCCESS;
    return NULL;
}

/* Extrae el nombre para mostrar del firmante del catálogo (best effort; no
 * evalúa ninguna cadena de confianza). Devuelve FALSE si no pudo leerlo. */
static BOOL
GetCatalogSigner(
    IN PCWSTR CatalogPath,
    OUT PWSTR Signer,
    IN DWORD SignerCch,
    OUT PWSTR SignerVersion,
    IN DWORD SignerVersionCch)
{
    HCERTSTORE Store = NULL;
    HCRYPTMSG Msg = NULL;
    PCCERT_CONTEXT Cert = NULL;
    CMSG_SIGNER_INFO *SignerInfo = NULL;
    CERT_INFO CertInfo;
    DWORD ContentType, FormatType, EncodingType, Size;
    BOOL Result = FALSE;

    Signer[0] = UNICODE_NULL;
    SignerVersion[0] = UNICODE_NULL;

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, (void *)CatalogPath,
                          CERT_QUERY_CONTENT_FLAG_ALL,
                          CERT_QUERY_FORMAT_FLAG_ALL, 0,
                          &EncodingType, &ContentType, &FormatType, &Store, &Msg, NULL))
        return FALSE;

    Size = 0;
    if (!CryptMsgGetParam(Msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &Size))
        goto done;
    SignerInfo = MyMalloc(Size);
    if (!SignerInfo || !CryptMsgGetParam(Msg, CMSG_SIGNER_INFO_PARAM, 0, SignerInfo, &Size))
        goto done;

    CertInfo.Issuer = SignerInfo->Issuer;
    CertInfo.SerialNumber = SignerInfo->SerialNumber;
    Cert = CertFindCertificateInStore(Store,
                                      X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                      0, CERT_FIND_SUBJECT_CERT, &CertInfo, NULL);
    if (!Cert)
        goto done;

    Result = (CertGetNameStringW(Cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
                                 Signer, SignerCch) != 0);

done:
    if (Cert)
        CertFreeCertificateContext(Cert);
    if (SignerInfo)
        MyFree(SignerInfo);
    if (Msg)
        CryptMsgClose(Msg);
    if (Store)
        CertCloseStore(Store, 0);
    return Result;
}

BOOL WINAPI
SetupVerifyInfFileW(
    IN PCWSTR InfName,
    IN PSP_ALTPLATFORM_INFO AltPlatformInfo,
    _Inout_ PSP_INF_SIGNER_INFO_INF_W InfSignerInfo)
{
    WCHAR Source[MAX_PATH], SourceRoot[MAX_PATH], CatalogPath[MAX_PATH];
    CRYPTCATMEMBER *Member;
    SYSTEM_INFO SystemInfo;
    HCATINFO Catalog;
    HANDLE hFile;
    PWSTR BaseName, CatBase;
    DWORD Length, Error;
    BOOL Found = FALSE;

    if (!InfName || !InfName[0] || !InfSignerInfo ||
        InfSignerInfo->cbSize != sizeof(*InfSignerInfo))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (AltPlatformInfo)
    {
        if (AltPlatformInfo->cbSize != sizeof(*AltPlatformInfo))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        /* Solo la plataforma nativa está soportada hoy. */
        GetNativeSystemInfo(&SystemInfo);
        if (AltPlatformInfo->ProcessorArchitecture != SystemInfo.wProcessorArchitecture)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    InfSignerInfo->CatalogFile[0] = UNICODE_NULL;
    InfSignerInfo->DigitalSigner[0] = UNICODE_NULL;
    InfSignerInfo->DigitalSignerVersion[0] = UNICODE_NULL;

    Length = GetFullPathNameW(InfName, ARRAY_SIZE(Source), Source, &BaseName);
    if (Length == 0 || Length >= ARRAY_SIZE(Source) || !BaseName || BaseName == Source)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    lstrcpynW(SourceRoot, Source, (INT)(BaseName - Source) + 1);

    {
        HINF hInf = SetupOpenInfFileW(Source, NULL, INF_STYLE_WIN4, NULL);
        if (hInf == INVALID_HANDLE_VALUE)
            return FALSE;
        Catalog = OpenInfCatalog(hInf, SourceRoot, CatalogPath, ARRAY_SIZE(CatalogPath), &Error);
        SetupCloseInfFile(hInf);
    }
    if (!Catalog)
    {
        if (Error != ERROR_SUCCESS)
        {
            SetLastError(Error);
            return FALSE;
        }
        /* Paquete sin catálogo: no firmado. */
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    if (Catalog == INVALID_HANDLE_VALUE)
    {
        /* Catálogo ilegible. */
        SetLastError(ERROR_INVALID_IMAGE_HASH);
        return FALSE;
    }

    hFile = CreateFileW(Source, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        CryptCATClose(Catalog);
        return FALSE;
    }

    Member = NULL;
    while ((Member = CryptCATEnumerateMember(Catalog, Member)))
    {
        if (VerifyFileHashWithAlgorithm(hFile, Member))
        {
            Found = TRUE;
            break;
        }
    }
    CloseHandle(hFile);
    CryptCATClose(Catalog);

    if (!Found)
    {
        SetLastError(ERROR_INVALID_IMAGE_HASH);
        return FALSE;
    }

    GetCatalogSigner(CatalogPath, InfSignerInfo->DigitalSigner, MAX_PATH,
                     InfSignerInfo->DigitalSignerVersion, MAX_PATH);

    CatBase = wcsrchr(CatalogPath, L'\\');
    lstrcpynW(InfSignerInfo->CatalogFile,
              CatBase ? CatBase + 1 : CatalogPath, MAX_PATH);
    return TRUE;
}

BOOL WINAPI
SetupVerifyInfFileA(
    IN PCSTR InfName,
    IN PSP_ALTPLATFORM_INFO AltPlatformInfo,
    _Inout_ PSP_INF_SIGNER_INFO_INF_A InfSignerInfo)
{
    SP_INF_SIGNER_INFO_INF_W InfoW;
    WCHAR InfNameW[MAX_PATH];
    BOOL Ret;

    if (!InfName || !InfName[0] || !InfSignerInfo ||
        InfSignerInfo->cbSize != sizeof(*InfSignerInfo))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!MultiByteToWideChar(CP_ACP, 0, InfName, -1, InfNameW, ARRAY_SIZE(InfNameW)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    InfoW.cbSize = sizeof(InfoW);
    Ret = SetupVerifyInfFileW(InfNameW, AltPlatformInfo, &InfoW);
    if (Ret)
    {
        WideCharToMultiByte(CP_ACP, 0, InfoW.CatalogFile, -1,
                            InfSignerInfo->CatalogFile, MAX_PATH, NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, InfoW.DigitalSigner, -1,
                            InfSignerInfo->DigitalSigner, MAX_PATH, NULL, NULL);
        WideCharToMultiByte(CP_ACP, 0, InfoW.DigitalSignerVersion, -1,
                            InfSignerInfo->DigitalSignerVersion, MAX_PATH, NULL, NULL);
    }
    return Ret;
}

static BOOL
GetPublishedInfPath(
    IN PCWSTR FileName,
    OUT PWSTR Buffer)
{
    UINT Length;

    if (wcschr(FileName, L'\\') || wcschr(FileName, L'/'))
    {
        Length = GetFullPathNameW(FileName, MAX_PATH, Buffer, NULL);
        return Length != 0 && Length < MAX_PATH;
    }

    Length = GetSystemWindowsDirectoryW(Buffer, MAX_PATH);
    if (Length == 0 || Length + 5 + lstrlenW(FileName) >= MAX_PATH)
        return FALSE;

    lstrcatW(Buffer, L"\\inf\\");
    lstrcatW(Buffer, FileName);
    return TRUE;
}

static BOOL
FindStoreInf(
    IN PCWSTR Published,
    IN PCWSTR RequiredName,
    OUT PWSTR StoreInfFileName)
{
    WCHAR Root[MAX_PATH], Pattern[MAX_PATH], Candidate[MAX_PATH], Suffix[64];
    WIN32_FIND_DATAW FindData;
    ULONGLONG Hash;
    HANDLE hFind;
    BOOL Found = FALSE;

    if (!GetRepositoryRoot(Root, ARRAY_SIZE(Root)))
        return FALSE;

    if (_wcsnicmp(Published, Root, lstrlenW(Root)) == 0)
    {
        WCHAR Package[MAX_PATH];
        PCWSTR Relative = Published + lstrlenW(Root);
        PCWSTR Rest;
        SIZE_T PackageLength;

        /* Accept only the store layout Root\<package>\<inf>: a sibling such
         * as Root'X\a.inf' or a file directly in Root must not resolve */
        if (Relative[0] != L'\\')
            return FALSE;
        Rest = wcschr(Relative + 1, L'\\');
        if (!Rest || wcschr(Rest + 1, L'\\') != NULL)
            return FALSE;
        PackageLength = Rest - (Relative + 1);
        if (PackageLength == 0 || PackageLength >= ARRAY_SIZE(Package))
            return FALSE;
        lstrcpynW(Package, Relative + 1, (INT)PackageLength + 1);
        if (!IsPackageDirectoryName(Package, NULL, 0))
            return FALSE;
        if (GetFileAttributesW(Published) == INVALID_FILE_ATTRIBUTES)
            return FALSE;
        lstrcpyW(StoreInfFileName, Published);
        return TRUE;
    }

    if (!HashFileContents(Published, &Hash))
        return FALSE;

    swprintf(Suffix, ARRAY_SIZE(Suffix), L"_%s_%016I64x", GetStoreArchitecture(), Hash);
    if (lstrlenW(Root) + 2 + lstrlenW(Suffix) >= MAX_PATH)
        return FALSE;

    lstrcpyW(Pattern, Root);
    lstrcatW(Pattern, L"\\*");
    lstrcatW(Pattern, Suffix);

    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return FALSE;

    do
    {
        WCHAR InfName[MAX_PATH];
        DWORD NameLength = lstrlenW(FindData.cFileName);
        DWORD SuffixLength = lstrlenW(Suffix);

        if (!(FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || NameLength <= SuffixLength)
            continue;

        lstrcpynW(InfName, FindData.cFileName, NameLength - SuffixLength + 1);
        if (RequiredName && _wcsicmp(InfName, RequiredName))
            continue;
        if (lstrlenW(Root) + 2 + NameLength + lstrlenW(InfName) >= MAX_PATH)
            continue;

        swprintf(Candidate, ARRAY_SIZE(Candidate), L"%s\\%s\\%s", Root, FindData.cFileName, InfName);
        if (FilesAreIdentical(Published, Candidate))
        {
            lstrcpyW(StoreInfFileName, Candidate);
            Found = TRUE;
        }
    } while (!Found && FindNextFileW(hFind, &FindData));

    FindClose(hFind);
    return Found;
}

BOOL
SETUPAPI_FindDriverStoreInf(
    IN PCWSTR FileName,
    OUT PWSTR StoreInfFileName)
{
    WCHAR Published[MAX_PATH];

    if (!GetPublishedInfPath(FileName, Published))
        return FALSE;
    return FindStoreInf(Published, NULL, StoreInfFileName);
}

PCWSTR
SETUPAPI_GetInfSourceDirectory(
    IN const struct InfFileDetails *InfFileDetails,
    OUT PWSTR Buffer)
{
    WCHAR Published[MAX_PATH];
    PWSTR Last;

    if (!InfFileDetails->DirectoryName ||
        _wcsnicmp(InfFileDetails->FileName, L"oem", 3) != 0 ||
        !IsSystemInfDirectory(InfFileDetails->DirectoryName) ||
        !CombinePath(Published, InfFileDetails->DirectoryName, InfFileDetails->FileName) ||
        !SETUPAPI_FindDriverStoreInf(Published, Buffer))
    {
        return InfFileDetails->DirectoryName;
    }

    Last = wcsrchr(Buffer, L'\\');
    if (!Last)
        return InfFileDetails->DirectoryName;

    *Last = UNICODE_NULL;
    return Buffer;
}

VOID
SETUPAPI_RecordPublishedDriverPackage(
    IN PCWSTR PublishedInfFileName,
    IN PCWSTR SourceInfFileName)
{
    WCHAR Source[MAX_PATH];
    PCWSTR PublishedName;
    PWSTR BaseName;
    DWORD Length;

    PublishedName = wcsrchr(PublishedInfFileName, L'\\');
    PublishedName = PublishedName ? PublishedName + 1 : PublishedInfFileName;

    Length = GetFullPathNameW(SourceInfFileName, ARRAY_SIZE(Source), Source, &BaseName);
    if (Length == 0 || Length >= ARRAY_SIZE(Source) || !BaseName || BaseName == Source)
        return;

    BaseName[-1] = UNICODE_NULL;
    SETUPAPI_RecordDriverDatabasePackage(PublishedInfFileName, PublishedName, BaseName, Source);
}

BOOL
SETUPAPI_GetOriginalInfName(
    IN PCWSTR PublishedInfFileName,
    OUT PWSTR OriginalName,
    IN DWORD OriginalNameSize)
{
    WCHAR Directory[MAX_PATH], StoreInf[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    PWSTR Last;

    if (lstrlenW(PublishedInfFileName) >= ARRAY_SIZE(Directory))
        return FALSE;
    lstrcpyW(Directory, PublishedInfFileName);
    Last = wcsrchr(Directory, L'\\');
    if (!Last)
        return FALSE;
    *Last = UNICODE_NULL;
    if (!IsSystemInfDirectory(Directory) ||
        !FindStoreInf(PublishedInfFileName, NULL, StoreInf))
    {
        return FALSE;
    }

    hFind = FindFirstFileW(StoreInf, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return FALSE;
    FindClose(hFind);

    if ((DWORD)lstrlenW(FindData.cFileName) >= OriginalNameSize)
        return FALSE;
    lstrcpyW(OriginalName, FindData.cFileName);
    return TRUE;
}

VOID
SETUPAPI_RecordInstalledDriverPackage(
    IN const struct InfFileDetails *InfFileDetails)
{
    WCHAR Published[MAX_PATH], Path[MAX_PATH + 64];
    HKEY Key;

    if (!InfFileDetails->DirectoryName ||
        _wcsnicmp(InfFileDetails->FileName, L"oem", 3) == 0 ||
        !IsSystemInfDirectory(InfFileDetails->DirectoryName) ||
        !CombinePath(Published, InfFileDetails->DirectoryName, InfFileDetails->FileName))
    {
        return;
    }

    swprintf(Path, ARRAY_SIZE(Path), L"SYSTEM\\DriverDatabase\\DriverInfFiles\\%s", InfFileDetails->FileName);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_QUERY_VALUE, &Key) == ERROR_SUCCESS)
    {
        RegCloseKey(Key);
        return;
    }

    SETUPAPI_RecordDriverDatabasePackage(Published, InfFileDetails->FileName, InfFileDetails->FileName, NULL);
}

static BOOL
DeleteDirectoryTree(
    IN PCWSTR Directory)
{
    WCHAR Path[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;

    if (!CombinePath(Path, Directory, L"*"))
        return FALSE;

    hFind = FindFirstFileW(Path, &FindData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(FindData.cFileName, L".") || !wcscmp(FindData.cFileName, L".."))
                continue;
            if (!CombinePath(Path, Directory, FindData.cFileName))
                continue;

            if (FindData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                /* Delete the link itself; never follow it into a target */
                SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(Path);
            }
            else if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                DeleteDirectoryTree(Path);
            }
            else
            {
                SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(Path);
            }
        } while (FindNextFileW(hFind, &FindData));
        FindClose(hFind);
    }

    return RemoveDirectoryW(Directory);
}

BOOL
SETUPAPI_DeleteDriverStorePackage(
    IN PCWSTR PublishedInfFileName)
{
    WCHAR StoreInf[MAX_PATH];
    PWSTR Last;

    if (!SETUPAPI_FindDriverStoreInf(PublishedInfFileName, StoreInf))
        return TRUE;

    RemovePublishedInfEntries(StoreInf, FALSE);

    Last = wcsrchr(StoreInf, L'\\');
    if (!Last)
        return TRUE;

    *Last = UNICODE_NULL;
    return DeleteDirectoryTree(StoreInf);
}

static HRESULT
CheckStoreRequest(
    IN WORD Architecture,
    IN PDWORD ReturnLength)
{
    SYSTEM_INFO SystemInfo;

    if (*ReturnLength < MAX_PATH)
        return E_INVALIDARG;

    GetSystemInfo(&SystemInfo);
    if (Architecture != SystemInfo.wProcessorArchitecture)
        return E_INVALIDARG;

    return S_OK;
}

static HRESULT
ReturnStorePath(
    IN PCWSTR Path,
    OUT PWSTR ReturnPath,
    IN OUT PDWORD ReturnLength)
{
    DWORD Length = lstrlenW(Path) + 1;

    if (Length > *ReturnLength)
    {
        *ReturnLength = Length;
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    lstrcpyW(ReturnPath, Path);
    *ReturnLength = Length;
    return S_OK;
}

static BOOL
FindSourceStoreInf(
    IN PCWSTR InfPath,
    OUT PWSTR Source,
    OUT PWSTR StoreInfFileName)
{
    PWSTR BaseName;
    DWORD Length;

    if (wcschr(InfPath, L'"'))
        return FALSE;

    Length = GetFullPathNameW(InfPath, MAX_PATH, Source, &BaseName);
    if (Length == 0 || Length >= MAX_PATH || !BaseName)
        return FALSE;

    if (BaseName != Source && BaseName[-1] == L'\\' &&
        BaseName[0] == L'.' && BaseName[1] == L'.' &&
        (BaseName[2] == UNICODE_NULL || BaseName[2] == L'\\'))
    {
        return FALSE;
    }

    return FindStoreInf(Source, BaseName, StoreInfFileName);
}

/* Drops the DriverDatabase index entries of every published INF whose
 * content matches the store package, and optionally the published files */
static VOID
RemovePublishedInfEntries(
    IN PCWSTR StoreInfFileName,
    IN BOOL DeleteFiles)
{
    WCHAR InfDirectory[MAX_PATH], Pattern[MAX_PATH], Published[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    PWSTR Extension;
    UINT Length;

    Length = GetSystemWindowsDirectoryW(InfDirectory, ARRAY_SIZE(InfDirectory));
    if (Length == 0 || Length + 5 >= ARRAY_SIZE(InfDirectory))
        return;
    lstrcatW(InfDirectory, L"\\inf");

    if (!CombinePath(Pattern, InfDirectory, L"oem*.inf"))
        return;

    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if ((FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !CombinePath(Published, InfDirectory, FindData.cFileName) ||
            !FilesAreIdentical(Published, StoreInfFileName))
        {
            continue;
        }

        SETUPAPI_DeleteDriverDatabasePackage(FindData.cFileName);

        if (DeleteFiles)
        {
            DeleteFileW(Published);
            Extension = wcsrchr(Published, L'.');
            if (Extension)
            {
                lstrcpyW(Extension, L".pnf");
                DeleteFileW(Published);
            }
        }
    } while (FindNextFileW(hFind, &FindData));

    FindClose(hFind);
}

HRESULT
WINAPI
DriverStoreFindDriverPackageW(
    IN PCWSTR InfPath,
    IN PVOID Reserved1,
    IN PVOID Reserved2,
    IN WORD Architecture,
    IN PVOID Reserved3,
    OUT PWSTR ReturnPath,
    IN OUT PDWORD ReturnLength)
{
    WCHAR Source[MAX_PATH], StoreInf[MAX_PATH];
    HRESULT hr;

    hr = CheckStoreRequest(Architecture, ReturnLength);
    if (FAILED(hr))
        return hr;

    Source[0] = UNICODE_NULL;
    if (FindSourceStoreInf(InfPath, Source, StoreInf))
        return ReturnStorePath(StoreInf, ReturnPath, ReturnLength);

    *ReturnPath = UNICODE_NULL;
    *ReturnLength = 0;
    if (GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT
WINAPI
DriverStoreAddDriverPackageW(
    IN PCWSTR InfPath,
    IN PVOID Reserved1,
    IN PVOID Reserved2,
    IN WORD Architecture,
    OUT PWSTR ReturnPath,
    IN OUT PDWORD ReturnLength)
{
    WCHAR Source[MAX_PATH], StoreInf[MAX_PATH];
    HRESULT hr;

    hr = CheckStoreRequest(Architecture, ReturnLength);
    if (FAILED(hr))
        return hr;

    if (!FindSourceStoreInf(InfPath, Source, StoreInf))
    {
        if (GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        if (!SetupCopyOEMInfW(Source, NULL, SPOST_NONE, 0, NULL, 0, NULL, NULL))
            return HRESULT_FROM_WIN32(GetLastError());
        if (!FindSourceStoreInf(Source, Source, StoreInf))
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    return ReturnStorePath(StoreInf, ReturnPath, ReturnLength);
}

HRESULT
WINAPI
DriverStoreDeleteDriverPackageW(
    IN PCWSTR InfPath,
    IN PVOID Reserved1,
    IN PVOID Reserved2)
{
    WCHAR Source[MAX_PATH], StoreInf[MAX_PATH];
    PWSTR Last;

    if (!FindSourceStoreInf(InfPath, Source, StoreInf))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    RemovePublishedInfEntries(StoreInf, TRUE);

    Last = wcsrchr(StoreInf, L'\\');
    if (!Last)
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    *Last = UNICODE_NULL;
    if (!DeleteDirectoryTree(StoreInf))
        return HRESULT_FROM_WIN32(GetLastError());

    return S_OK;
}

static PWSTR
StoreAnsiToUnicode(
    IN PCSTR String)
{
    PWSTR Result;
    INT Length;

    Length = MultiByteToWideChar(CP_ACP, 0, String, -1, NULL, 0);
    if (Length == 0)
        return NULL;

    Result = MyMalloc(Length * sizeof(WCHAR));
    if (Result)
        MultiByteToWideChar(CP_ACP, 0, String, -1, Result, Length);
    return Result;
}

static HRESULT
ReturnStorePathA(
    IN HRESULT hr,
    IN PCWSTR PathW,
    OUT PSTR ReturnPath,
    IN OUT PDWORD ReturnLength)
{
    INT Length;

    if (FAILED(hr))
    {
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        {
            *ReturnPath = ANSI_NULL;
            *ReturnLength = 0;
        }
        return hr;
    }

    Length = WideCharToMultiByte(CP_ACP, 0, PathW, -1, NULL, 0, NULL, NULL);
    if ((DWORD)Length > *ReturnLength)
    {
        *ReturnLength = Length;
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    WideCharToMultiByte(CP_ACP, 0, PathW, -1, ReturnPath, Length, NULL, NULL);
    *ReturnLength = Length;
    return S_OK;
}

HRESULT
WINAPI
DriverStoreFindDriverPackageA(
    IN PCSTR InfPath,
    IN PVOID Reserved1,
    IN PVOID Reserved2,
    IN WORD Architecture,
    IN PVOID Reserved3,
    OUT PSTR ReturnPath,
    IN OUT PDWORD ReturnLength)
{
    WCHAR PathW[MAX_PATH];
    DWORD LengthW = ARRAY_SIZE(PathW);
    PWSTR InfPathW;
    HRESULT hr;

    if (*ReturnLength < MAX_PATH)
        return E_INVALIDARG;

    InfPathW = StoreAnsiToUnicode(InfPath);
    if (!InfPathW)
        return E_OUTOFMEMORY;

    hr = DriverStoreFindDriverPackageW(InfPathW, Reserved1, Reserved2, Architecture, Reserved3, PathW, &LengthW);
    MyFree(InfPathW);
    return ReturnStorePathA(hr, PathW, ReturnPath, ReturnLength);
}

HRESULT
WINAPI
DriverStoreAddDriverPackageA(
    IN PCSTR InfPath,
    IN PVOID Reserved1,
    IN PVOID Reserved2,
    IN WORD Architecture,
    OUT PSTR ReturnPath,
    IN OUT PDWORD ReturnLength)
{
    WCHAR PathW[MAX_PATH];
    DWORD LengthW = ARRAY_SIZE(PathW);
    PWSTR InfPathW;
    HRESULT hr;

    if (*ReturnLength < MAX_PATH)
        return E_INVALIDARG;

    InfPathW = StoreAnsiToUnicode(InfPath);
    if (!InfPathW)
        return E_OUTOFMEMORY;

    hr = DriverStoreAddDriverPackageW(InfPathW, Reserved1, Reserved2, Architecture, PathW, &LengthW);
    MyFree(InfPathW);
    return ReturnStorePathA(hr, PathW, ReturnPath, ReturnLength);
}

HRESULT
WINAPI
DriverStoreDeleteDriverPackageA(
    IN PCSTR InfPath,
    IN PVOID Reserved1,
    IN PVOID Reserved2)
{
    PWSTR InfPathW;
    HRESULT hr;

    InfPathW = StoreAnsiToUnicode(InfPath);
    if (!InfPathW)
        return E_OUTOFMEMORY;

    hr = DriverStoreDeleteDriverPackageW(InfPathW, Reserved1, Reserved2);
    MyFree(InfPathW);
    return hr;
}

/* PUBLISHED NAME LOOKUP AND ENUMERATION ************************************/

/* Fills InfDirectory with the full path of the published INF directory. */
static BOOL
GetPublishedInfDirectory(
    OUT PWSTR InfDirectory,
    IN DWORD InfDirectorySize)
{
    UINT Length = GetSystemWindowsDirectoryW(InfDirectory, InfDirectorySize);

    if (Length == 0 || Length + 5 >= InfDirectorySize)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    lstrcatW(InfDirectory, L"\\inf");
    return TRUE;
}

BOOL
WINAPI
SetupGetInfPublishedNameW(
    IN PCWSTR DriverStoreLocation,
    OUT PWSTR ReturnBuffer,
    IN DWORD ReturnBufferSize,
    OUT PDWORD RequiredSize OPTIONAL)
{
    WCHAR StorePath[MAX_PATH], InfDirectory[MAX_PATH], Pattern[MAX_PATH], Candidate[MAX_PATH];
    PWSTR BaseName;
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    DWORD NameLength, Error;
    UINT Length;
    BOOL Found = FALSE;

    if (RequiredSize)
        *RequiredSize = 0;

    if (!DriverStoreLocation || !ReturnBuffer || ReturnBufferSize == 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Length = GetFullPathNameW(DriverStoreLocation, ARRAY_SIZE(StorePath), StorePath, NULL);
    if (Length == 0 || Length >= ARRAY_SIZE(StorePath))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!GetPublishedInfDirectory(InfDirectory, ARRAY_SIZE(InfDirectory)))
        return FALSE;

    BaseName = wcsrchr(StorePath, L'\\');

    /* An oem*.inf that already lives in the published directory is its own
     * published name; inbox INFs are not published */
    if (BaseName && IsSystemInfDirectory(StorePath) &&
        _wcsnicmp(BaseName + 1, L"oem", 3) == 0)
    {
        if (GetFileAttributesW(StorePath) == INVALID_FILE_ATTRIBUTES)
        {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return FALSE;
        }
        Found = TRUE;
        BaseName++;
        lstrcpynW(Candidate, BaseName, ARRAY_SIZE(Candidate));
    }

    if (!Found)
    {
        if (lstrlenW(InfDirectory) + 10 >= ARRAY_SIZE(Pattern))
        {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return FALSE;
        }
        lstrcpyW(Pattern, InfDirectory);
        lstrcatW(Pattern, L"\\oem*.inf");

        hFind = FindFirstFileW(Pattern, &FindData);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            Error = GetLastError();
            SetLastError(Error == ERROR_FILE_NOT_FOUND ? ERROR_FILE_NOT_FOUND : Error);
            return FALSE;
        }

        do
        {
            if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (lstrlenW(InfDirectory) + 1 + lstrlenW(FindData.cFileName) >= ARRAY_SIZE(Candidate))
                continue;
            swprintf(Candidate, ARRAY_SIZE(Candidate), L"%s\\%s", InfDirectory, FindData.cFileName);
            if (FilesAreIdentical(StorePath, Candidate))
            {
                lstrcpynW(Candidate, FindData.cFileName, ARRAY_SIZE(Candidate));
                Found = TRUE;
            }
        } while (!Found && FindNextFileW(hFind, &FindData));

        FindClose(hFind);
    }

    if (!Found)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    NameLength = lstrlenW(Candidate) + 1;
    if (RequiredSize)
        *RequiredSize = NameLength;
    if (ReturnBufferSize < NameLength)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    lstrcpyW(ReturnBuffer, Candidate);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL
WINAPI
SetupGetInfPublishedNameA(
    IN PCSTR DriverStoreLocation,
    OUT PSTR ReturnBuffer,
    IN DWORD ReturnBufferSize,
    OUT PDWORD RequiredSize OPTIONAL)
{
    WCHAR LocationW[MAX_PATH];
    WCHAR PublishedW[MAX_PATH];
    DWORD RequiredW = 0, Error;
    PWSTR Location = NULL;
    INT Length;
    BOOL Ret;

    if (RequiredSize)
        *RequiredSize = 0;
    if (!DriverStoreLocation || !ReturnBuffer || ReturnBufferSize == 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Length = MultiByteToWideChar(CP_ACP, 0, DriverStoreLocation, -1, NULL, 0);
    if (Length == 0 || Length > (INT)ARRAY_SIZE(LocationW))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MultiByteToWideChar(CP_ACP, 0, DriverStoreLocation, -1, LocationW, ARRAY_SIZE(LocationW));
    Location = LocationW;

    Ret = SetupGetInfPublishedNameW(Location, PublishedW, ARRAY_SIZE(PublishedW), &RequiredW);
    if (!Ret)
    {
        Error = GetLastError();
        if (RequiredSize)
            *RequiredSize = RequiredW;
        SetLastError(Error);
        return FALSE;
    }

    Length = WideCharToMultiByte(CP_ACP, 0, PublishedW, -1, NULL, 0, NULL, NULL);
    if (RequiredSize)
        *RequiredSize = (DWORD)Length;
    if ((DWORD)Length > ReturnBufferSize)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    WideCharToMultiByte(CP_ACP, 0, PublishedW, -1, ReturnBuffer, ReturnBufferSize, NULL, NULL);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

static int __cdecl
ComparePublishedNames(
    IN const void *First,
    IN const void *Second)
{
    return _wcsicmp(*(PCWSTR const *)First, *(PCWSTR const *)Second);
}

/* LiberNT-private assumed contract: no public prototype exists; the shape
 * follows the setupapi enumeration family and only public documentation
 * was used. */
BOOL
WINAPI
LnSetupEnumPublishedInfW(
    IN INFENUMPROCW Callback,
    IN PVOID Context OPTIONAL,
    IN DWORD Flags)
{
    WCHAR InfDirectory[MAX_PATH], Pattern[MAX_PATH], Candidate[MAX_PATH];
    PWSTR *Names = NULL;
    DWORD Count = 0, Capacity = 0, i;
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    BOOL Ret = TRUE;

    if (!Callback)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Flags != 0)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }

    if (!GetPublishedInfDirectory(InfDirectory, ARRAY_SIZE(InfDirectory)))
        return FALSE;

    lstrcpyW(Pattern, InfDirectory);
    lstrcatW(Pattern, L"\\oem*.inf");

    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        return FALSE;
    }

    do
    {
        PWSTR *NewNames;
        PWSTR Name;

        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (Count == Capacity)
        {
            Capacity = Capacity ? Capacity * 2 : 16;
            NewNames = Names ? MyRealloc(Names, Capacity * sizeof(PWSTR))
                             : MyMalloc(Capacity * sizeof(PWSTR));
            if (!NewNames)
            {
                Ret = FALSE;
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                break;
            }
            Names = NewNames;
        }

        Name = MyMalloc((lstrlenW(FindData.cFileName) + 1) * sizeof(WCHAR));
        if (!Name)
        {
            Ret = FALSE;
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            break;
        }
        lstrcpyW(Name, FindData.cFileName);
        Names[Count++] = Name;
    } while (FindNextFileW(hFind, &FindData));

    FindClose(hFind);

    if (Ret && Count > 1)
        qsort(Names, Count, sizeof(PWSTR), ComparePublishedNames);

    if (Ret)
    {
        for (i = 0; i < Count; i++)
        {
            lstrcpynW(Candidate, Names[i], ARRAY_SIZE(Candidate));
            if (!Callback(Candidate, Context))
            {
                SetLastError(ERROR_CANCELLED);
                Ret = FALSE;
                break;
            }
        }
    }

    for (i = 0; i < Count; i++)
        MyFree(Names[i]);
    MyFree(Names);

    return Ret;
}

typedef struct _LN_ENUM_PUBLISHED_INF_ANSI_CONTEXT
{
    INFENUMPROCA Callback;
    PVOID Context;
} LN_ENUM_PUBLISHED_INF_ANSI_CONTEXT, *PLN_ENUM_PUBLISHED_INF_ANSI_CONTEXT;

static BOOL CALLBACK
EnumPublishedInfAnsiTrampoline(
    IN PCWSTR Name,
    IN PVOID Context)
{
    PLN_ENUM_PUBLISHED_INF_ANSI_CONTEXT AnsiContext = Context;
    CHAR AnsiName[MAX_PATH];
    INT Length;

    Length = WideCharToMultiByte(CP_ACP, 0, Name, -1, AnsiName, ARRAY_SIZE(AnsiName), NULL, NULL);
    if (Length == 0)
        return FALSE;

    return AnsiContext->Callback(AnsiName, AnsiContext->Context);
}

BOOL
WINAPI
LnSetupEnumPublishedInfA(
    IN INFENUMPROCA Callback,
    IN PVOID Context OPTIONAL,
    IN DWORD Flags)
{
    LN_ENUM_PUBLISHED_INF_ANSI_CONTEXT AnsiContext;

    if (!Callback)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (Flags != 0)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }

    AnsiContext.Callback = Callback;
    AnsiContext.Context = Context;
    return LnSetupEnumPublishedInfW(EnumPublishedInfAnsiTrampoline, &AnsiContext, Flags);
}

/* PACKAGE ENUMERATION *********************************************************/

/* Checks that PackageName has the <name>_<arch>_<16 hex digits> shape. */
static BOOL
IsPackageDirectoryName(
    IN PCWSTR PackageName,
    OUT PWSTR InfBaseName OPTIONAL,
    IN DWORD InfBaseNameSize)
{
    static const WCHAR HexDigits[] = L"0123456789abcdef";
    PCWSTR Arch = GetStoreArchitecture();
    SIZE_T Length = wcslen(PackageName), ArchLength = wcslen(Arch), i;

    if (Length < ArchLength + 18)
        return FALSE;

    for (i = Length - 16; i < Length; i++)
    {
        PCWSTR Digit = wcschr(HexDigits, towlower(PackageName[i]));

        if (!Digit)
            return FALSE;
    }

    if (PackageName[Length - 17] != L'_' ||
        _wcsnicmp(PackageName + Length - 17 - ArchLength, Arch, ArchLength) != 0 ||
        PackageName[Length - 18 - ArchLength] != L'_')
    {
        return FALSE;
    }

    if (InfBaseName)
    {
        SIZE_T BaseLength = Length - 18 - ArchLength;

        if (BaseLength == 0 || BaseLength >= InfBaseNameSize)
            return FALSE;
        lstrcpynW(InfBaseName, PackageName, (INT)BaseLength + 1);
    }

    return TRUE;
}

/* LiberNT-private assumed contract: no public prototype exists; the shape
 * mirrors the in-tree DriverStore*Package family and only public
 * documentation was used. */
HRESULT
WINAPI
LnDriverStoreEnumDriverPackageW(
    IN PCWSTR EnumFilter OPTIONAL,
    IN PVOID Reserved1,
    IN WORD Architecture,
    OUT PWSTR ReturnPath,
    IN OUT PDWORD ReturnLength,
    IN OUT PDWORD Index)
{
    WCHAR Root[MAX_PATH], Pattern[MAX_PATH], PackagePath[MAX_PATH], InfBaseName[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    HRESULT hr;
    DWORD Seen = 0;
    BOOL Found = FALSE;

    if (Reserved1 != NULL)
        return E_INVALIDARG;
    if (!ReturnPath || !ReturnLength || !Index)
        return E_INVALIDARG;

    hr = CheckStoreRequest(Architecture, ReturnLength);
    if (FAILED(hr))
        return hr;

    if (!GetRepositoryRoot(Root, ARRAY_SIZE(Root)))
        return HRESULT_FROM_WIN32(GetLastError());

    lstrcpyW(Pattern, Root);
    lstrcatW(Pattern, L"\\*");

    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    do
    {
        if (!(FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (!IsPackageDirectoryName(FindData.cFileName, InfBaseName, ARRAY_SIZE(InfBaseName)))
            continue;
        if (EnumFilter && EnumFilter[0] &&
            _wcsnicmp(FindData.cFileName, EnumFilter, lstrlenW(EnumFilter)) != 0)
        {
            continue;
        }

        if (Seen++ < *Index)
            continue;

        if (lstrlenW(Root) + 1 + lstrlenW(FindData.cFileName) + 1 + lstrlenW(InfBaseName) + 1 >= ARRAY_SIZE(PackagePath))
        {
            hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            goto Done;
        }

        swprintf(PackagePath, ARRAY_SIZE(PackagePath), L"%s\\%s\\%s", Root, FindData.cFileName, InfBaseName);
        if (GetFileAttributesW(PackagePath) == INVALID_FILE_ATTRIBUTES)
        {
            /* An incomplete package has no INF to report; skip it */
            (*Index)++;
            continue;
        }

        *Index = Seen;
        hr = ReturnStorePath(PackagePath, ReturnPath, ReturnLength);
        Found = TRUE;
    } while (!Found && FindNextFileW(hFind, &FindData));

    if (!Found && SUCCEEDED(hr))
        hr = HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);

Done:
    FindClose(hFind);
    return hr;
}

HRESULT
WINAPI
LnDriverStoreEnumDriverPackageA(
    IN PCSTR EnumFilter OPTIONAL,
    IN PVOID Reserved1,
    IN WORD Architecture,
    OUT PSTR ReturnPath,
    IN OUT PDWORD ReturnLength,
    IN OUT PDWORD Index)
{
    WCHAR PathW[MAX_PATH];
    DWORD LengthW = ARRAY_SIZE(PathW);
    PWSTR FilterW = NULL;
    HRESULT hr;

    if (*ReturnLength < MAX_PATH)
        return E_INVALIDARG;

    if (EnumFilter && EnumFilter[0])
    {
        FilterW = StoreAnsiToUnicode(EnumFilter);
        if (!FilterW)
            return E_OUTOFMEMORY;
    }

    hr = LnDriverStoreEnumDriverPackageW(FilterW, Reserved1, Architecture, PathW, &LengthW, Index);
    MyFree(FilterW);
    return ReturnStorePathA(hr, PathW, ReturnPath, ReturnLength);
}
