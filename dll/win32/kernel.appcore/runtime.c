/*
 * PROJECT:     LiberNT AppModel Runtime
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Windows AppModel runtime host
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "appcore.h"

#define APPCORE_MAX_PACKAGE_NAME 50
#define APPCORE_MAX_RESOURCE_ID 30
#define APPCORE_PUBLISHER_ID_LENGTH 13
#define APPCORE_MAX_RELATIVE_ID 64
#define APPCORE_MAX_APPLICATION_USER_MODEL_ID 130
#define APPCORE_MAX_FULL_NAME 127

typedef struct _APPCORE_FULL_NAME_PARTS
{
    PCWSTR Name;
    UINT32 NameLength;
    PACKAGE_VERSION Version;
    UINT32 Architecture;
    PCWSTR ResourceId;
    UINT32 ResourceIdLength;
    PCWSTR PublisherId;
} APPCORE_FULL_NAME_PARTS;

NTSYSAPI
NTSTATUS
NTAPI
NtQueryInformationToken(
    _In_ HANDLE TokenHandle,
    _In_ TOKEN_INFORMATION_CLASS TokenInformationClass,
    _Out_writes_bytes_to_(TokenInformationLength, *ReturnLength) PVOID TokenInformation,
    _In_ ULONG TokenInformationLength,
    _Out_ PULONG ReturnLength);

static BOOL
AppCoreIsAsciiLetter(WCHAR Character)
{
    return (Character >= L'A' && Character <= L'Z') ||
           (Character >= L'a' && Character <= L'z');
}

static BOOL
AppCoreIsAsciiDigit(WCHAR Character)
{
    return Character >= L'0' && Character <= L'9';
}

static WCHAR
AppCoreToLower(WCHAR Character)
{
    if (Character >= L'A' && Character <= L'Z')
        return Character + (L'a' - L'A');
    return Character;
}

static BOOL
AppCoreEqualsInsensitiveN(PCWSTR Left, UINT32 LeftLength, PCWSTR Right)
{
    UINT32 Index;

    if ((UINT32)lstrlenW(Right) != LeftLength)
        return FALSE;
    for (Index = 0; Index < LeftLength; ++Index)
    {
        if (AppCoreToLower(Left[Index]) != AppCoreToLower(Right[Index]))
            return FALSE;
    }
    return TRUE;
}

static BOOL
AppCoreIsReservedName(PCWSTR Value, UINT32 Length)
{
    static const PCWSTR Reserved[] =
    {
        L"con", L"prn", L"aux", L"nul", L"com1", L"com2", L"com3",
        L"com4", L"com5", L"com6", L"com7", L"com8", L"com9", L"lpt1",
        L"lpt2", L"lpt3", L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"
    };
    UINT32 Index;

    for (Index = 0; Index < _countof(Reserved); ++Index)
    {
        if (AppCoreEqualsInsensitiveN(Value, Length, Reserved[Index]))
            return TRUE;
    }
    return FALSE;
}

static BOOL
AppCoreIsPackageString(PCWSTR Value, UINT32 Length, UINT32 Minimum, UINT32 Maximum, BOOL AllowEmpty)
{
    UINT32 Index, SegmentStart;

    if (!Value || Length > Maximum || (Length < Minimum && !(AllowEmpty && Length == 0)))
        return FALSE;
    if (!Length)
        return TRUE;
    if (Value[Length - 1] == L'.')
        return FALSE;
    if (Length >= 4 && AppCoreToLower(Value[0]) == L'x' && AppCoreToLower(Value[1]) == L'n' &&
        Value[2] == L'-' && Value[3] == L'-')
        return FALSE;

    SegmentStart = 0;
    for (Index = 0; Index < Length; ++Index)
    {
        if (!(AppCoreIsAsciiLetter(Value[Index]) || AppCoreIsAsciiDigit(Value[Index]) ||
              Value[Index] == L'.' || Value[Index] == L'-'))
            return FALSE;
        if (Value[Index] == L'.')
        {
            if (Index == SegmentStart || AppCoreIsReservedName(Value + SegmentStart, Index - SegmentStart))
                return FALSE;
            if (Index + 4 < Length && AppCoreToLower(Value[Index + 1]) == L'x' &&
                AppCoreToLower(Value[Index + 2]) == L'n' && Value[Index + 3] == L'-' && Value[Index + 4] == L'-')
                return FALSE;
            SegmentStart = Index + 1;
        }
    }
    return !AppCoreIsReservedName(Value + SegmentStart, Length - SegmentStart);
}

static BOOL
AppCoreIsPublisherId(PCWSTR Value)
{
    UINT32 Index;

    if (!Value || lstrlenW(Value) != APPCORE_PUBLISHER_ID_LENGTH)
        return FALSE;
    for (Index = 0; Index < APPCORE_PUBLISHER_ID_LENGTH; ++Index)
    {
        WCHAR Character = AppCoreToLower(Value[Index]);
        if (!AppCoreIsAsciiDigit(Character) &&
            !(Character >= L'a' && Character <= L'h') &&
            !(Character >= L'j' && Character <= L'k') &&
            !(Character >= L'm' && Character <= L'n') &&
            !(Character >= L'p' && Character <= L't') &&
            !(Character >= L'v' && Character <= L'z'))
            return FALSE;
    }
    return TRUE;
}

static BOOL
AppCoreParseNumber(PCWSTR Value, UINT32 Length, USHORT *Number)
{
    UINT32 Index, Result = 0;

    if (!Length)
        return FALSE;
    for (Index = 0; Index < Length; ++Index)
    {
        if (!AppCoreIsAsciiDigit(Value[Index]))
            return FALSE;
        Result = Result * 10 + Value[Index] - L'0';
        if (Result > 65535)
            return FALSE;
    }
    *Number = (USHORT)Result;
    return TRUE;
}

static BOOL
AppCoreParseVersion(PCWSTR Value, UINT32 Length, PACKAGE_VERSION *Version)
{
    USHORT *Fields[4] =
    {
        &Version->Major,
        &Version->Minor,
        &Version->Build,
        &Version->Revision
    };
    UINT32 Field, Start = 0, Index;

    Version->Version = 0;
    for (Field = 0; Field < 4; ++Field)
    {
        for (Index = Start; Index < Length && Value[Index] != L'.'; ++Index)
            ;
        if (!AppCoreParseNumber(Value + Start, Index - Start, Fields[Field]))
            return FALSE;
        if ((Field != 3 && Index == Length) || (Field == 3 && Index != Length))
            return FALSE;
        Start = Index + 1;
    }
    return TRUE;
}

static BOOL
AppCoreParseArchitecture(PCWSTR Value, UINT32 Length, UINT32 *Architecture)
{
    if (AppCoreEqualsInsensitiveN(Value, Length, L"x86"))
        *Architecture = PROCESSOR_ARCHITECTURE_INTEL;
    else if (AppCoreEqualsInsensitiveN(Value, Length, L"arm"))
        *Architecture = PROCESSOR_ARCHITECTURE_ARM;
    else if (AppCoreEqualsInsensitiveN(Value, Length, L"x64"))
        *Architecture = PROCESSOR_ARCHITECTURE_AMD64;
    else if (AppCoreEqualsInsensitiveN(Value, Length, L"neutral"))
        *Architecture = PROCESSOR_ARCHITECTURE_NEUTRAL;
    else if (AppCoreEqualsInsensitiveN(Value, Length, L"arm64"))
        *Architecture = PROCESSOR_ARCHITECTURE_ARM64;
    else if (AppCoreEqualsInsensitiveN(Value, Length, L"x86a64"))
        *Architecture = PROCESSOR_ARCHITECTURE_IA32_ON_ARM64;
    else
        return FALSE;
    return TRUE;
}

static PCWSTR
AppCoreArchitectureName(UINT32 Architecture)
{
    switch (Architecture)
    {
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
        case PROCESSOR_ARCHITECTURE_ARM: return L"arm";
        case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
        case PROCESSOR_ARCHITECTURE_NEUTRAL: return L"neutral";
        case PROCESSOR_ARCHITECTURE_ARM64: return L"arm64";
        case PROCESSOR_ARCHITECTURE_IA32_ON_ARM64: return L"x86a64";
        default: return NULL;
    }
}

static BOOL
AppCoreParseFullName(PCWSTR FullName, APPCORE_FULL_NAME_PARTS *Parts)
{
    UINT32 Length, Start[5], PartLength[5], Part = 0, Index;

    if (!FullName)
        return FALSE;
    Length = lstrlenW(FullName);
    Start[0] = 0;
    for (Index = 0; Index <= Length; ++Index)
    {
        if (FullName[Index] == L'_' || FullName[Index] == UNICODE_NULL)
        {
            if (Part >= 5)
                return FALSE;
            PartLength[Part] = Index - Start[Part];
            ++Part;
            if (Part < 5)
                Start[Part] = Index + 1;
        }
    }
    if (Part != 5 || !AppCoreIsPackageString(FullName, PartLength[0], 3, APPCORE_MAX_PACKAGE_NAME, FALSE) ||
        !AppCoreParseVersion(FullName + Start[1], PartLength[1], &Parts->Version) ||
        !AppCoreParseArchitecture(FullName + Start[2], PartLength[2], &Parts->Architecture) ||
        !AppCoreIsPackageString(FullName + Start[3], PartLength[3], 0, APPCORE_MAX_RESOURCE_ID, TRUE) ||
        PartLength[4] != APPCORE_PUBLISHER_ID_LENGTH || !AppCoreIsPublisherId(FullName + Start[4]))
        return FALSE;
    Parts->Name = FullName;
    Parts->NameLength = PartLength[0];
    Parts->ResourceId = FullName + Start[3];
    Parts->ResourceIdLength = PartLength[3];
    Parts->PublisherId = FullName + Start[4];
    return TRUE;
}

static BOOL
AppCoreParseFamilyName(PCWSTR Family, UINT32 *NameLength, PCWSTR *PublisherId)
{
    UINT32 Length, Index, Separator = MAXUINT32;

    if (!Family)
        return FALSE;
    Length = lstrlenW(Family);
    for (Index = 0; Index < Length; ++Index)
    {
        if (Family[Index] == L'_')
        {
            if (Separator != MAXUINT32)
                return FALSE;
            Separator = Index;
        }
    }
    if (Separator == MAXUINT32 || !AppCoreIsPackageString(Family, Separator, 1, APPCORE_MAX_PACKAGE_NAME, FALSE) ||
        Length - Separator - 1 != APPCORE_PUBLISHER_ID_LENGTH || !AppCoreIsPublisherId(Family + Separator + 1))
        return FALSE;
    *NameLength = Separator;
    *PublisherId = Family + Separator + 1;
    return TRUE;
}

static BOOL
AppCoreIsRelativeApplicationId(PCWSTR Value)
{
    UINT32 Length, Index, FieldStart = 0;

    if (!Value || !(Length = lstrlenW(Value)) || Length > APPCORE_MAX_RELATIVE_ID)
        return FALSE;
    for (Index = 0; Index <= Length; ++Index)
    {
        if (Value[Index] == L'.' || Value[Index] == UNICODE_NULL)
        {
            if (Index == FieldStart || !AppCoreIsAsciiLetter(Value[FieldStart]) ||
                AppCoreIsReservedName(Value + FieldStart, Index - FieldStart))
                return FALSE;
            FieldStart = Index + 1;
        }
        else if (!AppCoreIsAsciiLetter(Value[Index]) && !AppCoreIsAsciiDigit(Value[Index]))
            return FALSE;
    }
    return TRUE;
}

static LONG
AppCoreWriteString(PCWSTR Value, UINT32 ValueLength, UINT32 *Length, PWSTR Buffer)
{
    UINT32 Required = ValueLength + 1;

    if (!Length)
        return ERROR_INVALID_PARAMETER;
    if (!Buffer || *Length < Required)
    {
        *Length = Required;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    CopyMemory(Buffer, Value, ValueLength * sizeof(WCHAR));
    Buffer[ValueLength] = UNICODE_NULL;
    *Length = Required;
    return ERROR_SUCCESS;
}

static UINT32
AppCoreDecimalLength(USHORT Value)
{
    UINT32 Length = 1;
    while (Value >= 10)
    {
        Value /= 10;
        ++Length;
    }
    return Length;
}

static PWSTR
AppCoreWriteDecimal(PWSTR Buffer, USHORT Value)
{
    UINT32 Length = AppCoreDecimalLength(Value), Index;
    for (Index = Length; Index; --Index)
    {
        Buffer[Index - 1] = L'0' + Value % 10;
        Value /= 10;
    }
    return Buffer + Length;
}

static LONG
AppCorePolicy(HANDLE Token, PVOID Policy, UINT32 Value)
{
    TOKEN_TYPE Type;
    ULONG Length;
    NTSTATUS Status;

    if (!Token || !Policy)
        return ERROR_INVALID_PARAMETER;
    *(UINT32 *)Policy = 0;
    Status = NtQueryInformationToken(Token, TokenType, &Type, sizeof(Type), &Length);
    if (Status < 0)
        return Status;
    *(UINT32 *)Policy = Value;
    return ERROR_SUCCESS;
}

#define APPCORE_LONG_STUB(Name, Parameters) LONG WINAPI Name Parameters { return ERROR_CALL_NOT_IMPLEMENTED; }
#define APPCORE_HRESULT_STUB(Name, Parameters) HRESULT WINAPI Name Parameters { return E_NOTIMPL; }

APPCORE_HRESULT_STUB(AddPackageDependency, (PCWSTR Id, INT32 Rank, AddPackageDependencyOptions Options, PACKAGEDEPENDENCY_CONTEXT *Context, PWSTR *FullName))
APPCORE_HRESULT_STUB(AddPackageDependency2, (PCWSTR Id, INT32 Rank, AddPackageDependencyOptions2 Options, PACKAGEDEPENDENCY_CONTEXT *Context, PWSTR *FullName))
LONG WINAPI AppPolicyGetClrCompat(HANDLE Token, AppPolicyClrCompat *Policy) { return AppCorePolicy(Token, Policy, AppPolicyClrCompat_ClassicDesktop); }
LONG WINAPI AppPolicyGetCreateFileAccess(HANDLE Token, AppPolicyCreateFileAccess *Policy) { return AppCorePolicy(Token, Policy, AppPolicyCreateFileAccess_Full); }
LONG WINAPI AppPolicyGetLifecycleManagement(HANDLE Token, AppPolicyLifecycleManagement *Policy) { return AppCorePolicy(Token, Policy, AppPolicyLifecycleManagement_Unmanaged); }
LONG WINAPI AppPolicyGetMediaFoundationCodecLoading(HANDLE Token, AppPolicyMediaFoundationCodecLoading *Policy) { return AppCorePolicy(Token, Policy, AppPolicyMediaFoundationCodecLoading_All); }
LONG WINAPI AppPolicyGetProcessTerminationMethod(HANDLE Token, AppPolicyProcessTerminationMethod *Policy) { return AppCorePolicy(Token, Policy, AppPolicyProcessTerminationMethod_ExitProcess); }
LONG WINAPI AppPolicyGetShowDeveloperDiagnostic(HANDLE Token, AppPolicyShowDeveloperDiagnostic *Policy) { return AppCorePolicy(Token, Policy, AppPolicyShowDeveloperDiagnostic_ShowUI); }
LONG WINAPI AppPolicyGetThreadInitializationType(HANDLE Token, AppPolicyThreadInitializationType *Policy) { return AppCorePolicy(Token, Policy, AppPolicyThreadInitializationType_None); }
LONG WINAPI AppPolicyGetWindowingModel(HANDLE Token, AppPolicyWindowingModel *Policy) { return AppCorePolicy(Token, Policy, AppPolicyWindowingModel_ClassicDesktop); }
APPCORE_HRESULT_STUB(CheckIsMSIXPackage, (PCWSTR FullName, BOOL *IsMsix))
APPCORE_LONG_STUB(ClosePackageInfo, (PACKAGE_INFO_REFERENCE Reference))
APPCORE_HRESULT_STUB(DeletePackageDependency, (PCWSTR Id))
APPCORE_HRESULT_STUB(FindPackageDependency, (const FindPackageDependencyCriteria *Criteria, UINT32 *Count, PWSTR **Ids))
APPCORE_HRESULT_STUB(GetIdForPackageDependencyContext, (PACKAGEDEPENDENCY_CONTEXT Context, PWSTR *Id))
APPCORE_LONG_STUB(GetPackageApplicationIds, (PACKAGE_INFO_REFERENCE Reference, UINT32 *Length, BYTE *Buffer, UINT32 *Count))
APPCORE_HRESULT_STUB(GetPackageDependencyInformation, (PCWSTR Id, PSID *User, PWSTR *Family, PACKAGE_VERSION *Version, PackageDependencyProcessorArchitectures *Architectures, PackageDependencyLifetimeKind *Lifetime, PWSTR *Artifact, CreatePackageDependencyOptions *Options, FILETIME *Expiration))
APPCORE_LONG_STUB(GetPackageInfo, (PACKAGE_INFO_REFERENCE Reference, UINT32 Flags, UINT32 *Length, BYTE *Buffer, UINT32 *Count))
APPCORE_LONG_STUB(GetPackageInfo2, (PACKAGE_INFO_REFERENCE Reference, UINT32 Flags, PackagePathType Type, UINT32 *Length, BYTE *Buffer, UINT32 *Count))
APPCORE_LONG_STUB(GetPackagePath, (const PACKAGE_ID *Id, UINT32 Reserved, UINT32 *Length, PWSTR Path))
APPCORE_LONG_STUB(GetPackagePathByFullName2, (PCWSTR FullName, PackagePathType Type, UINT32 *Length, PWSTR Path))
APPCORE_HRESULT_STUB(GetProcessesUsingPackageDependency, (PCWSTR Id, PSID User, BOOL System, UINT32 *Count, DWORD **Processes))
APPCORE_HRESULT_STUB(GetResolvedPackageFullNameForPackageDependency, (PCWSTR Id, PWSTR *FullName))
APPCORE_HRESULT_STUB(GetResolvedPackageFullNameForPackageDependency2, (PCWSTR Id, PWSTR *FullName))
APPCORE_LONG_STUB(GetStagedPackageOrigin, (PCWSTR FullName, PackageOrigin *Origin))
APPCORE_LONG_STUB(GetStagedPackagePathByFullName, (PCWSTR FullName, UINT32 *Length, PWSTR Path))
APPCORE_LONG_STUB(GetStagedPackagePathByFullName2, (PCWSTR FullName, PackagePathType Type, UINT32 *Length, PWSTR Path))
APPCORE_LONG_STUB(OpenPackageInfoByFullName, (PCWSTR FullName, UINT32 Reserved, PACKAGE_INFO_REFERENCE *Reference))
APPCORE_LONG_STUB(OpenPackageInfoByFullNameForUser, (PSID User, PCWSTR FullName, UINT32 Reserved, PACKAGE_INFO_REFERENCE *Reference))
APPCORE_HRESULT_STUB(RemovePackageDependency, (PACKAGEDEPENDENCY_CONTEXT Context))
APPCORE_HRESULT_STUB(TryCreatePackageDependency, (PSID User, PCWSTR Family, PACKAGE_VERSION Version, PackageDependencyProcessorArchitectures Architectures, PackageDependencyLifetimeKind Lifetime, PCWSTR Artifact, CreatePackageDependencyOptions Options, PWSTR *Id))
APPCORE_HRESULT_STUB(TryCreatePackageDependency2, (PSID User, PCWSTR Family, PACKAGE_VERSION Version, PackageDependencyProcessorArchitectures Architectures, PackageDependencyLifetimeKind Lifetime, PCWSTR Artifact, CreatePackageDependencyOptions Options, const FILETIME *Expiration, PWSTR *Id))

LONG
WINAPI
VerifyPackageRelativeApplicationId(
    _In_ PCWSTR Value)
{
    return AppCoreIsRelativeApplicationId(Value) ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

LONG
WINAPI
VerifyPackageFamilyName(
    _In_ PCWSTR Value)
{
    UINT32 NameLength;
    PCWSTR PublisherId;

    return AppCoreParseFamilyName(Value, &NameLength, &PublisherId) ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

LONG
WINAPI
VerifyPackageFullName(
    _In_ PCWSTR Value)
{
    APPCORE_FULL_NAME_PARTS Parts;

    return AppCoreParseFullName(Value, &Parts) ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}

LONG
WINAPI
VerifyPackageId(
    _In_ const PACKAGE_ID *Id)
{
    if (!Id || !AppCoreArchitectureName(Id->processorArchitecture) ||
        !AppCoreIsPackageString(Id->name, Id->name ? lstrlenW(Id->name) : 0, 3, APPCORE_MAX_PACKAGE_NAME, FALSE) ||
        (Id->resourceId && !AppCoreIsPackageString(Id->resourceId, lstrlenW(Id->resourceId), 0, APPCORE_MAX_RESOURCE_ID, TRUE)) ||
        !AppCoreIsPublisherId(Id->publisherId))
        return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

LONG
WINAPI
VerifyApplicationUserModelId(
    _In_ PCWSTR Value)
{
    UINT32 Index, Length, NameLength;
    PCWSTR PublisherId;
    WCHAR Family[APPCORE_MAX_PACKAGE_NAME + APPCORE_PUBLISHER_ID_LENGTH + 2];

    if (!Value || !(Length = lstrlenW(Value)) || Length + 1 > APPCORE_MAX_APPLICATION_USER_MODEL_ID)
        return ERROR_INVALID_PARAMETER;
    for (Index = 0; Index < Length && Value[Index] != L'!'; ++Index)
        ;
    if (Index == Length || !Index || !Value[Index + 1])
        return ERROR_INVALID_PARAMETER;
    for (NameLength = Index + 1; NameLength < Length; ++NameLength)
    {
        if (Value[NameLength] == L'!')
            return ERROR_INVALID_PARAMETER;
    }
    if (Index >= _countof(Family))
        return ERROR_INVALID_PARAMETER;
    CopyMemory(Family, Value, Index * sizeof(WCHAR));
    Family[Index] = UNICODE_NULL;
    if (!AppCoreParseFamilyName(Family, &NameLength, &PublisherId) || !AppCoreIsRelativeApplicationId(Value + Index + 1))
        return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

LONG
WINAPI
FormatApplicationUserModelId(
    _In_ PCWSTR Family,
    _In_ PCWSTR RelativeId,
    _Inout_ UINT32 *Length,
    _Out_writes_opt_(*Length) PWSTR Value)
{
    UINT32 NameLength, FamilyLength, RelativeLength, Required;
    PCWSTR PublisherId;

    if (!AppCoreParseFamilyName(Family, &NameLength, &PublisherId) || !AppCoreIsRelativeApplicationId(RelativeId) || !Length)
        return ERROR_INVALID_PARAMETER;
    FamilyLength = lstrlenW(Family);
    RelativeLength = lstrlenW(RelativeId);
    Required = FamilyLength + RelativeLength + 2;
    if (Required > APPCORE_MAX_APPLICATION_USER_MODEL_ID)
        return ERROR_INVALID_PARAMETER;
    if (!Value || *Length < Required)
    {
        *Length = Required;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    CopyMemory(Value, Family, FamilyLength * sizeof(WCHAR));
    Value[FamilyLength] = L'!';
    CopyMemory(Value + FamilyLength + 1, RelativeId, (RelativeLength + 1) * sizeof(WCHAR));
    *Length = Required;
    return ERROR_SUCCESS;
}

LONG
WINAPI
PackageFamilyNameFromId(
    _In_ const PACKAGE_ID *Id,
    _Inout_ UINT32 *Length,
    _Out_writes_opt_(*Length) PWSTR Family)
{
    WCHAR Value[APPCORE_MAX_PACKAGE_NAME + APPCORE_PUBLISHER_ID_LENGTH + 2];
    UINT32 NameLength;

    if (VerifyPackageId(Id) != ERROR_SUCCESS || !Length)
        return ERROR_INVALID_PARAMETER;
    NameLength = lstrlenW(Id->name);
    CopyMemory(Value, Id->name, NameLength * sizeof(WCHAR));
    Value[NameLength] = L'_';
    CopyMemory(Value + NameLength + 1, Id->publisherId, (APPCORE_PUBLISHER_ID_LENGTH + 1) * sizeof(WCHAR));
    return AppCoreWriteString(Value, NameLength + APPCORE_PUBLISHER_ID_LENGTH + 1, Length, Family);
}

LONG
WINAPI
PackageFullNameFromId(
    _In_ const PACKAGE_ID *Id,
    _Inout_ UINT32 *Length,
    _Out_writes_opt_(*Length) PWSTR FullName)
{
    WCHAR Value[APPCORE_MAX_FULL_NAME + 1];
    PCWSTR Architecture;
    PWSTR Cursor = Value;
    UINT32 NameLength, ResourceLength, ValueLength;

    if (VerifyPackageId(Id) != ERROR_SUCCESS || !Length)
        return ERROR_INVALID_PARAMETER;
    Architecture = AppCoreArchitectureName(Id->processorArchitecture);
    NameLength = lstrlenW(Id->name);
    ResourceLength = Id->resourceId ? lstrlenW(Id->resourceId) : 0;
    CopyMemory(Cursor, Id->name, NameLength * sizeof(WCHAR));
    Cursor += NameLength;
    *Cursor++ = L'_';
    Cursor = AppCoreWriteDecimal(Cursor, Id->version.Major);
    *Cursor++ = L'.';
    Cursor = AppCoreWriteDecimal(Cursor, Id->version.Minor);
    *Cursor++ = L'.';
    Cursor = AppCoreWriteDecimal(Cursor, Id->version.Build);
    *Cursor++ = L'.';
    Cursor = AppCoreWriteDecimal(Cursor, Id->version.Revision);
    *Cursor++ = L'_';
    CopyMemory(Cursor, Architecture, lstrlenW(Architecture) * sizeof(WCHAR));
    Cursor += lstrlenW(Architecture);
    *Cursor++ = L'_';
    if (ResourceLength)
        CopyMemory(Cursor, Id->resourceId, ResourceLength * sizeof(WCHAR));
    Cursor += ResourceLength;
    *Cursor++ = L'_';
    CopyMemory(Cursor, Id->publisherId, APPCORE_PUBLISHER_ID_LENGTH * sizeof(WCHAR));
    Cursor += APPCORE_PUBLISHER_ID_LENGTH;
    *Cursor = UNICODE_NULL;
    ValueLength = (UINT32)(Cursor - Value);
    return AppCoreWriteString(Value, ValueLength, Length, FullName);
}

LONG
WINAPI
PackageNameAndPublisherIdFromFamilyName(
    _In_ PCWSTR Family,
    _Inout_ UINT32 *NameLength,
    _Out_writes_opt_(*NameLength) PWSTR Name,
    _Inout_ UINT32 *PublisherLength,
    _Out_writes_opt_(*PublisherLength) PWSTR Publisher)
{
    UINT32 ParsedNameLength;
    PCWSTR PublisherId;
    BOOL TooSmall = FALSE;

    if (!NameLength || !PublisherLength || !AppCoreParseFamilyName(Family, &ParsedNameLength, &PublisherId))
        return ERROR_INVALID_PARAMETER;
    if (!Name || *NameLength < ParsedNameLength + 1)
        TooSmall = TRUE;
    if (!Publisher || *PublisherLength < APPCORE_PUBLISHER_ID_LENGTH + 1)
        TooSmall = TRUE;
    if (TooSmall)
    {
        *NameLength = ParsedNameLength + 1;
        *PublisherLength = APPCORE_PUBLISHER_ID_LENGTH + 1;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    CopyMemory(Name, Family, ParsedNameLength * sizeof(WCHAR));
    Name[ParsedNameLength] = UNICODE_NULL;
    CopyMemory(Publisher, PublisherId, (APPCORE_PUBLISHER_ID_LENGTH + 1) * sizeof(WCHAR));
    *NameLength = ParsedNameLength + 1;
    *PublisherLength = APPCORE_PUBLISHER_ID_LENGTH + 1;
    return ERROR_SUCCESS;
}

LONG
WINAPI
PackageIdFromFullName(
    _In_ PCWSTR FullName,
    _In_ UINT32 Flags,
    _Inout_ UINT32 *Length,
    _Out_writes_bytes_opt_(*Length) BYTE *Buffer)
{
    APPCORE_FULL_NAME_PARTS Parts;
    PACKAGE_ID *Id;
    PWSTR Cursor;
    UINT32 Required;

    if (!Length || !AppCoreParseFullName(FullName, &Parts))
        return ERROR_INVALID_PARAMETER;
    if (Flags & PACKAGE_INFORMATION_FULL)
        return ERROR_NOT_FOUND;
    Required = sizeof(PACKAGE_ID) +
               (Parts.NameLength + Parts.ResourceIdLength + APPCORE_PUBLISHER_ID_LENGTH + 3) * sizeof(WCHAR);
    if (!Buffer || *Length < Required)
    {
        *Length = Required;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    Id = (PACKAGE_ID *)Buffer;
    ZeroMemory(Id, sizeof(*Id));
    Id->processorArchitecture = Parts.Architecture;
    Id->version = Parts.Version;
    Cursor = (PWSTR)(Id + 1);
    Id->name = Cursor;
    CopyMemory(Cursor, Parts.Name, Parts.NameLength * sizeof(WCHAR));
    Cursor[Parts.NameLength] = UNICODE_NULL;
    Cursor += Parts.NameLength + 1;
    Id->resourceId = Cursor;
    CopyMemory(Cursor, Parts.ResourceId, Parts.ResourceIdLength * sizeof(WCHAR));
    Cursor[Parts.ResourceIdLength] = UNICODE_NULL;
    Cursor += Parts.ResourceIdLength + 1;
    Id->publisherId = Cursor;
    CopyMemory(Cursor, Parts.PublisherId, (APPCORE_PUBLISHER_ID_LENGTH + 1) * sizeof(WCHAR));
    *Length = Required;
    return ERROR_SUCCESS;
}

LONG
WINAPI
GetApplicationUserModelId(
    _In_ HANDLE Process,
    _Inout_ UINT32 *Length,
    _Out_writes_opt_(*Length) PWSTR Value)
{
    UNREFERENCED_PARAMETER(Value);

    if (!Process || !Length)
        return ERROR_INVALID_PARAMETER;
    if (!GetProcessId(Process))
        return GetLastError();
    return APPMODEL_ERROR_NO_APPLICATION;
}

LONG
WINAPI
GetCurrentPackageInfo2(
    _In_ UINT32 Flags,
    _In_ PackagePathType Type,
    _Inout_ UINT32 *Length,
    _Out_writes_bytes_opt_(*Length) BYTE *Buffer,
    _Out_ UINT32 *Count)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Buffer);

    if (!Length)
        return ERROR_INVALID_PARAMETER;
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG
WINAPI
GetCurrentPackagePath2(
    _In_ PackagePathType Type,
    _Inout_ UINT32 *Length,
    _Out_writes_opt_(*Length) PWSTR Path)
{
    UNREFERENCED_PARAMETER(Path);
    UNREFERENCED_PARAMETER(Type);

    if (!Length)
        return ERROR_INVALID_PARAMETER;
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG
WINAPI
GetPackageId(
    _In_ HANDLE Process,
    _Inout_ UINT32 *Length,
    _Out_writes_bytes_opt_(*Length) BYTE *Buffer)
{
    UNREFERENCED_PARAMETER(Buffer);

    if (!Process || !Length)
        return ERROR_INVALID_PARAMETER;
    if (!GetProcessId(Process))
        return GetLastError();
    return APPMODEL_ERROR_NO_PACKAGE;
}

UINT32
WINAPI
GetPackageGraphRevisionId(VOID)
{
    return 0;
}
