/*
 * PROJECT:     LiberNT KernelBase
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Package-family enumeration compatibility
 * COPYRIGHT:   Copyright 2026 Ahmed Arif <arif.ing@outlook.com>
 *              Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <appmodel.h>

static BOOL
IsPackageNameCharacter(
    _In_ WCHAR Character)
{
    return (Character >= L'0' && Character <= L'9') ||
           (Character >= L'A' && Character <= L'Z') ||
           (Character >= L'a' && Character <= L'z') ||
           Character == L'.' || Character == L'-';
}

static BOOL
IsPublisherIdCharacter(
    _In_ WCHAR Character)
{
    static const WCHAR PublisherIdAlphabet[] = L"0123456789abcdefghjkmnpqrstvwxyz";
    const WCHAR *Current;

    if (Character >= L'A' && Character <= L'Z')
        Character += L'a' - L'A';

    for (Current = PublisherIdAlphabet; *Current; Current++)
    {
        if (*Current == Character)
            return TRUE;
    }

    return FALSE;
}

static BOOL
IsValidPackageFamilyName(
    _In_ PCWSTR PackageFamilyName)
{
    const WCHAR *Current;
    const WCHAR *Separator = NULL;
    SIZE_T NameLength;
    SIZE_T PublisherIdLength;

    for (Current = PackageFamilyName; *Current; Current++)
    {
        if (*Current == L'_')
        {
            if (Separator)
                return FALSE;
            Separator = Current;
        }
        else if (!Separator && !IsPackageNameCharacter(*Current))
        {
            return FALSE;
        }
        else if (Separator && !IsPublisherIdCharacter(*Current))
        {
            return FALSE;
        }
    }

    if (!Separator)
        return FALSE;

    NameLength = Separator - PackageFamilyName;
    PublisherIdLength = Current - Separator - 1;
    if (NameLength < 3 || NameLength > 50 || PublisherIdLength != 13)
        return FALSE;

    return Separator[-1] != L'.';
}

LONG
WINAPI
FindPackagesByPackageFamily(
    _In_ PCWSTR PackageFamilyName,
    _In_ UINT32 PackageFilters,
    _Inout_ UINT32 *Count,
    _Out_writes_opt_(*Count) PWSTR *PackageFullNames,
    _Inout_ UINT32 *BufferLength,
    _Out_writes_opt_(*BufferLength) WCHAR *Buffer,
    _Out_writes_opt_(*Count) UINT32 *PackageProperties)
{
    UNREFERENCED_PARAMETER(PackageFilters);
    UNREFERENCED_PARAMETER(PackageProperties);

    if (!PackageFamilyName || !Count || !BufferLength ||
        (!PackageFullNames && *Count) || (!Buffer && *BufferLength) ||
        !IsValidPackageFamilyName(PackageFamilyName))
    {
        return ERROR_INVALID_PARAMETER;
    }

    *Count = 0;
    *BufferLength = 0;
    return ERROR_SUCCESS;
}

static BOOL
IsValidPackageVersion(
    _In_reads_(Length) PCWSTR Version,
    _In_ SIZE_T Length)
{
    SIZE_T Index, Digits = 0;
    ULONG Parts = 1, Value = 0;

    for (Index = 0; Index < Length; Index++)
    {
        if (Version[Index] == L'.')
        {
            if (!Digits || ++Parts > 4)
                return FALSE;
            Digits = 0;
            Value = 0;
        }
        else if (Version[Index] >= L'0' && Version[Index] <= L'9')
        {
            Value = Value * 10 + (Version[Index] - L'0');
            if (++Digits > 5 || Value > 0xFFFF)
                return FALSE;
        }
        else
        {
            return FALSE;
        }
    }

    return Parts == 4 && Digits;
}

static BOOL
IsValidPackageArchitecture(
    _In_reads_(Length) PCWSTR Architecture,
    _In_ SIZE_T Length)
{
    static const PCWSTR Architectures[] = { L"x86", L"x64", L"arm", L"arm64", L"neutral", L"x86a64" };
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(Architectures); Index++)
    {
        if (lstrlenW(Architectures[Index]) == (int)Length &&
            CompareStringOrdinal(Architecture, (int)Length, Architectures[Index], (int)Length, TRUE) == CSTR_EQUAL)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL
IsReservedPackageName(
    _In_reads_(Length) PCWSTR Value,
    _In_ SIZE_T Length)
{
    static const PCWSTR Reserved[] =
    {
        L"con", L"prn", L"aux", L"nul", L"com1", L"com2", L"com3",
        L"com4", L"com5", L"com6", L"com7", L"com8", L"com9", L"lpt1",
        L"lpt2", L"lpt3", L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"
    };
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(Reserved); Index++)
    {
        if (lstrlenW(Reserved[Index]) == (int)Length &&
            CompareStringOrdinal(Value, (int)Length, Reserved[Index], (int)Length, TRUE) == CSTR_EQUAL)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL
IsValidPackageString(
    _In_reads_(Length) PCWSTR Value,
    _In_ SIZE_T Length,
    _In_ SIZE_T Minimum,
    _In_ SIZE_T Maximum,
    _In_ BOOL AllowEmpty)
{
    SIZE_T Index, SegmentStart = 0;

    if (Length > Maximum || (Length < Minimum && !(AllowEmpty && !Length)))
        return FALSE;
    if (!Length)
        return TRUE;
    if (Value[Length - 1] == L'.' ||
        (Length >= 4 && CompareStringOrdinal(Value, 4, L"xn--", 4, TRUE) == CSTR_EQUAL))
    {
        return FALSE;
    }
    for (Index = 0; Index < Length; Index++)
    {
        if (!IsPackageNameCharacter(Value[Index]))
            return FALSE;
        if (Value[Index] == L'.')
        {
            if (Index == SegmentStart || IsReservedPackageName(Value + SegmentStart, Index - SegmentStart))
                return FALSE;
            if (Index + 5 <= Length &&
                CompareStringOrdinal(Value + Index + 1, 4, L"xn--", 4, TRUE) == CSTR_EQUAL)
            {
                return FALSE;
            }
            SegmentStart = Index + 1;
        }
    }
    return !IsReservedPackageName(Value + SegmentStart, Length - SegmentStart);
}

static UINT32
PackageArchitectureFromString(
    _In_reads_(Length) PCWSTR Architecture,
    _In_ SIZE_T Length)
{
    static const struct
    {
        PCWSTR Name;
        UINT32 Value;
    } Architectures[] =
    {
        { L"x86", PROCESSOR_ARCHITECTURE_INTEL },
        { L"arm", PROCESSOR_ARCHITECTURE_ARM },
        { L"x64", PROCESSOR_ARCHITECTURE_AMD64 },
        { L"neutral", PROCESSOR_ARCHITECTURE_NEUTRAL },
        { L"arm64", PROCESSOR_ARCHITECTURE_ARM64 },
        { L"x86a64", PROCESSOR_ARCHITECTURE_IA32_ON_ARM64 }
    };
    ULONG Index;

    for (Index = 0; Index < ARRAYSIZE(Architectures); Index++)
    {
        if (lstrlenW(Architectures[Index].Name) == (int)Length &&
            CompareStringOrdinal(Architecture, (int)Length, Architectures[Index].Name, (int)Length, TRUE) == CSTR_EQUAL)
        {
            return Architectures[Index].Value;
        }
    }
    return MAXUINT32;
}

static BOOL
ParsePackageVersion(
    _In_reads_(Length) PCWSTR Version,
    _In_ SIZE_T Length,
    _Out_ PACKAGE_VERSION *Value)
{
    USHORT *Parts[] = { &Value->Major, &Value->Minor, &Value->Build, &Value->Revision };
    SIZE_T Start = 0, End;
    ULONG Part, Number;

    Value->Version = 0;
    for (Part = 0; Part < ARRAYSIZE(Parts); Part++)
    {
        Number = 0;
        for (End = Start; End < Length && Version[End] != L'.'; End++)
        {
            if (Version[End] < L'0' || Version[End] > L'9')
                return FALSE;
            Number = Number * 10 + Version[End] - L'0';
            if (Number > 0xffff)
                return FALSE;
        }
        if (End == Start || (Part != 3 && End == Length) || (Part == 3 && End != Length))
            return FALSE;
        *Parts[Part] = (USHORT)Number;
        Start = End + 1;
    }
    return TRUE;
}

LONG
WINAPI
PackageIdFromFullName(
    _In_ PCWSTR FullName,
    _In_ UINT32 Flags,
    _Inout_ UINT32 *BufferLength,
    _Out_writes_bytes_opt_(*BufferLength) BYTE *Buffer)
{
    PCWSTR Fields[5], Current;
    SIZE_T Lengths[5];
    ULONG Count = 0, Index;
    UINT32 Architecture, Required;
    PACKAGE_VERSION Version;
    PACKAGE_ID *Id;
    PWSTR String;

    if (!FullName || !BufferLength)
        return ERROR_INVALID_PARAMETER;
    Fields[0] = FullName;
    for (Current = FullName; ; Current++)
    {
        if (*Current == L'_' || !*Current)
        {
            if (Count >= ARRAYSIZE(Fields))
                return ERROR_INVALID_PARAMETER;
            Lengths[Count] = Current - Fields[Count];
            Count++;
            if (!*Current)
                break;
            if (Count < ARRAYSIZE(Fields))
                Fields[Count] = Current + 1;
        }
    }
    if (Count != ARRAYSIZE(Fields) ||
        !IsValidPackageString(Fields[0], Lengths[0], 3, 50, FALSE) ||
        !IsValidPackageString(Fields[3], Lengths[3], 0, 30, TRUE) || Lengths[4] != 13 ||
        !ParsePackageVersion(Fields[1], Lengths[1], &Version) ||
        (Architecture = PackageArchitectureFromString(Fields[2], Lengths[2])) == MAXUINT32)
        return ERROR_INVALID_PARAMETER;
    for (Index = 0; Index < Lengths[4]; Index++)
    {
        if (!IsPublisherIdCharacter(Fields[4][Index]))
            return ERROR_INVALID_PARAMETER;
    }
    if (Flags & PACKAGE_INFORMATION_FULL)
        return ERROR_NOT_FOUND;
    Required = sizeof(*Id) + (UINT32)(Lengths[0] + Lengths[3] + Lengths[4] + 3) * sizeof(WCHAR);
    if (!Buffer || *BufferLength < Required)
    {
        *BufferLength = Required;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    Id = (PACKAGE_ID *)Buffer;
    ZeroMemory(Id, sizeof(*Id));
    Id->processorArchitecture = Architecture;
    Id->version = Version;
    String = (PWSTR)(Id + 1);
    Id->name = String;
    CopyMemory(String, Fields[0], Lengths[0] * sizeof(WCHAR));
    String[Lengths[0]] = UNICODE_NULL;
    String += Lengths[0] + 1;
    Id->resourceId = String;
    CopyMemory(String, Fields[3], Lengths[3] * sizeof(WCHAR));
    String[Lengths[3]] = UNICODE_NULL;
    String += Lengths[3] + 1;
    Id->publisherId = String;
    CopyMemory(String, Fields[4], (Lengths[4] + 1) * sizeof(WCHAR));
    *BufferLength = Required;
    return ERROR_SUCCESS;
}

LONG
WINAPI
PackageFamilyNameFromFullName(
    _In_ PCWSTR packageFullName,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName)
{
    PCWSTR Fields[5], Current;
    WCHAR Family[64 + 1 + 13 + 1];
    SIZE_T Lengths[5], NameLength;
    ULONG Count = 0;

    if (!packageFullName || !packageFamilyNameLength || (!packageFamilyName && *packageFamilyNameLength))
        return ERROR_INVALID_PARAMETER;

    Fields[0] = packageFullName;
    for (Current = packageFullName; ; Current++)
    {
        if (*Current == L'_' || !*Current)
        {
            if (Count >= ARRAYSIZE(Fields))
                return ERROR_INVALID_PARAMETER;
            Lengths[Count] = Current - Fields[Count];
            Count++;
            if (!*Current)
                break;
            if (Count < ARRAYSIZE(Fields))
                Fields[Count] = Current + 1;
        }
    }

    if (Count != ARRAYSIZE(Fields) ||
        !IsValidPackageVersion(Fields[1], Lengths[1]) ||
        !IsValidPackageArchitecture(Fields[2], Lengths[2]))
    {
        return ERROR_INVALID_PARAMETER;
    }

    NameLength = Lengths[0];
    if (NameLength + 1 + Lengths[4] + 1 > ARRAYSIZE(Family))
        return ERROR_INVALID_PARAMETER;

    CopyMemory(Family, Fields[0], NameLength * sizeof(WCHAR));
    Family[NameLength] = L'_';
    CopyMemory(Family + NameLength + 1, Fields[4], Lengths[4] * sizeof(WCHAR));
    Family[NameLength + 1 + Lengths[4]] = UNICODE_NULL;

    if (!IsValidPackageFamilyName(Family))
        return ERROR_INVALID_PARAMETER;

    if (!packageFamilyName || *packageFamilyNameLength < (UINT32)lstrlenW(Family) + 1)
    {
        *packageFamilyNameLength = lstrlenW(Family) + 1;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    *packageFamilyNameLength = lstrlenW(Family) + 1;
    CopyMemory(packageFamilyName, Family, *packageFamilyNameLength * sizeof(WCHAR));
    return ERROR_SUCCESS;
}

LONG
WINAPI
ParseApplicationUserModelId(
    _In_ PCWSTR applicationUserModelId,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName,
    _Inout_ UINT32 *packageRelativeApplicationIdLength,
    _Out_writes_opt_(*packageRelativeApplicationIdLength) PWSTR packageRelativeApplicationId)
{
    WCHAR Family[64 + 1 + 13 + 1];
    PCWSTR Separator;
    SIZE_T FamilyLength;
    UINT32 FamilyRequired, ApplicationRequired;

    if (!applicationUserModelId || !packageFamilyNameLength || !packageRelativeApplicationIdLength ||
        (!packageFamilyName && *packageFamilyNameLength) ||
        (!packageRelativeApplicationId && *packageRelativeApplicationIdLength))
    {
        return ERROR_INVALID_PARAMETER;
    }

    Separator = wcschr(applicationUserModelId, L'!');
    if (!Separator || !Separator[1])
        return ERROR_INVALID_PARAMETER;

    FamilyLength = Separator - applicationUserModelId;
    if (FamilyLength >= ARRAYSIZE(Family))
        return ERROR_INVALID_PARAMETER;
    CopyMemory(Family, applicationUserModelId, FamilyLength * sizeof(WCHAR));
    Family[FamilyLength] = UNICODE_NULL;
    if (!IsValidPackageFamilyName(Family))
        return ERROR_INVALID_PARAMETER;

    FamilyRequired = (UINT32)FamilyLength + 1;
    ApplicationRequired = (UINT32)lstrlenW(Separator + 1) + 1;

    if (!packageFamilyName || *packageFamilyNameLength < FamilyRequired ||
        !packageRelativeApplicationId || *packageRelativeApplicationIdLength < ApplicationRequired)
    {
        *packageFamilyNameLength = FamilyRequired;
        *packageRelativeApplicationIdLength = ApplicationRequired;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    CopyMemory(packageFamilyName, Family, FamilyRequired * sizeof(WCHAR));
    CopyMemory(packageRelativeApplicationId, Separator + 1, ApplicationRequired * sizeof(WCHAR));
    *packageFamilyNameLength = FamilyRequired;
    *packageRelativeApplicationIdLength = ApplicationRequired;
    return ERROR_SUCCESS;
}

HRESULT
WINAPI
AddDependencyToProcessPackageGraph(
    _In_ PCWSTR PackageFamilyName,
    _In_opt_ PVOID Reserved1,
    _In_opt_ PVOID Reserved2,
    _In_opt_ PVOID Reserved3)
{
    UNREFERENCED_PARAMETER(PackageFamilyName);
    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);
    UNREFERENCED_PARAMETER(Reserved3);

    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT
WINAPI
AppXGetPackageSid(
    _In_ PCWSTR PackageFullName,
    _Out_ PSID *PackageSid)
{
    UNREFERENCED_PARAMETER(PackageFullName);
    UNREFERENCED_PARAMETER(PackageSid);

    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

VOID
WINAPI
AppXFreeMemory(
    _In_opt_ PVOID Memory)
{
    if (Memory)
        HeapFree(GetProcessHeap(), 0, Memory);
}

HMODULE
WINAPI
LoadPackagedLibrary(
    _In_ LPCWSTR lpwLibFileName,
    _Reserved_ DWORD Reserved)
{
    UNREFERENCED_PARAMETER(lpwLibFileName);
    UNREFERENCED_PARAMETER(Reserved);

    SetLastError(APPMODEL_ERROR_NO_PACKAGE);
    return NULL;
}
