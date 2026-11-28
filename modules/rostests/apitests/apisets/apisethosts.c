/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Check that API set contracts resolve to the Win11 host DLL and that
 *              the host exports every function of the contract
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>

typedef struct _CONTRACT_HOST
{
    PCWSTR Contract;
    PCWSTR Host;
    const PCSTR *Functions;
    ULONG Count;
} CONTRACT_HOST;

static const PCSTR SecurityBaseFunctions[] =
{
    "AccessCheck",
    "AccessCheckAndAuditAlarmW",
    "AccessCheckByType",
    "AccessCheckByTypeAndAuditAlarmW",
    "AccessCheckByTypeResultList",
    "AccessCheckByTypeResultListAndAuditAlarmByHandleW",
    "AccessCheckByTypeResultListAndAuditAlarmW",
    "AddAccessAllowedAce",
    "AddAccessAllowedAceEx",
    "AddAccessAllowedObjectAce",
    "AddAccessDeniedAce",
    "AddAccessDeniedAceEx",
    "AddAccessDeniedObjectAce",
    "AddAce",
    "AddAuditAccessAce",
    "AddAuditAccessAceEx",
    "AddAuditAccessObjectAce",
    "AddMandatoryAce",
    "AdjustTokenGroups",
    "AdjustTokenPrivileges",
    "AllocateAndInitializeSid",
    "AllocateLocallyUniqueId",
    "AreAllAccessesGranted",
    "AreAnyAccessesGranted",
    "CheckTokenMembership",
    "ConvertToAutoInheritPrivateObjectSecurity",
    "CopySid",
    "CreatePrivateObjectSecurity",
    "CreatePrivateObjectSecurityEx",
    "CreatePrivateObjectSecurityWithMultipleInheritance",
    "CreateRestrictedToken",
    "CreateWellKnownSid",
    "DeleteAce",
    "DestroyPrivateObjectSecurity",
    "DuplicateToken",
    "DuplicateTokenEx",
    "EqualDomainSid",
    "EqualPrefixSid",
    "EqualSid",
    "FindFirstFreeAce",
    "FreeSid",
    "GetAce",
    "GetAclInformation",
    "GetFileSecurityW",
    "GetKernelObjectSecurity",
    "GetLengthSid",
    "GetPrivateObjectSecurity",
    "GetSecurityDescriptorControl",
    "GetSecurityDescriptorDacl",
    "GetSecurityDescriptorGroup",
    "GetSecurityDescriptorLength",
    "GetSecurityDescriptorOwner",
    "GetSecurityDescriptorRMControl",
    "GetSecurityDescriptorSacl",
    "GetSidIdentifierAuthority",
    "GetSidLengthRequired",
    "GetSidSubAuthority",
    "GetSidSubAuthorityCount",
    "GetTokenInformation",
    "GetWindowsAccountDomainSid",
    "ImpersonateAnonymousToken",
    "ImpersonateLoggedOnUser",
    "ImpersonateSelf",
    "InitializeAcl",
    "InitializeSecurityDescriptor",
    "InitializeSid",
    "IsTokenRestricted",
    "IsValidAcl",
    "IsValidSecurityDescriptor",
    "IsValidSid",
    "IsWellKnownSid",
    "MakeAbsoluteSD",
    "MakeSelfRelativeSD",
    "MapGenericMask",
    "ObjectCloseAuditAlarmW",
    "ObjectDeleteAuditAlarmW",
    "ObjectOpenAuditAlarmW",
    "ObjectPrivilegeAuditAlarmW",
    "PrivilegeCheck",
    "PrivilegedServiceAuditAlarmW",
    "QuerySecurityAccessMask",
    "RevertToSelf",
    "SetAclInformation",
    "SetFileSecurityW",
    "SetKernelObjectSecurity",
    "SetPrivateObjectSecurity",
    "SetPrivateObjectSecurityEx",
    "SetSecurityAccessMask",
    "SetSecurityDescriptorControl",
    "SetSecurityDescriptorDacl",
    "SetSecurityDescriptorGroup",
    "SetSecurityDescriptorOwner",
    "SetSecurityDescriptorRMControl",
    "SetSecurityDescriptorSacl",
    "SetTokenInformation",
};

static const PCSTR StringAnsiFunctions[] =
{
    "CharLowerA",
    "CharLowerBuffA",
    "CharNextA",
    "CharNextExA",
    "CharPrevA",
    "CharPrevExA",
    "CharUpperA",
    "CharUpperBuffA",
    "IsCharAlphaA",
    "IsCharAlphaNumericA",
    "IsCharLowerA",
    "IsCharUpperA",
};

static const PCSTR UnicodeAnsiFunctions[] =
{
    "SHAnsiToAnsi",
    "SHAnsiToUnicode",
    "SHUnicodeToAnsi",
    "SHUnicodeToUnicode",
};

static const PCSTR MidlProxyStubFunctions[] =
{
    "CStdAsyncStubBuffer2_Connect",
    "CStdAsyncStubBuffer2_Disconnect",
    "CStdAsyncStubBuffer2_Release",
    "CStdAsyncStubBuffer_AddRef",
    "CStdAsyncStubBuffer_Connect",
    "CStdAsyncStubBuffer_Disconnect",
    "CStdAsyncStubBuffer_Invoke",
    "CStdAsyncStubBuffer_QueryInterface",
    "CStdAsyncStubBuffer_Release",
    "CStdStubBuffer2_Connect",
    "CStdStubBuffer2_CountRefs",
    "CStdStubBuffer2_Disconnect",
    "CStdStubBuffer2_QueryInterface",
    "NdrProxyForwardingFunction10",
    "NdrProxyForwardingFunction11",
    "NdrProxyForwardingFunction12",
    "NdrProxyForwardingFunction13",
    "NdrProxyForwardingFunction14",
    "NdrProxyForwardingFunction15",
    "NdrProxyForwardingFunction16",
    "NdrProxyForwardingFunction17",
    "NdrProxyForwardingFunction18",
    "NdrProxyForwardingFunction19",
    "NdrProxyForwardingFunction20",
    "NdrProxyForwardingFunction21",
    "NdrProxyForwardingFunction22",
    "NdrProxyForwardingFunction23",
    "NdrProxyForwardingFunction24",
    "NdrProxyForwardingFunction25",
    "NdrProxyForwardingFunction26",
    "NdrProxyForwardingFunction27",
    "NdrProxyForwardingFunction28",
    "NdrProxyForwardingFunction29",
    "NdrProxyForwardingFunction3",
    "NdrProxyForwardingFunction30",
    "NdrProxyForwardingFunction31",
    "NdrProxyForwardingFunction32",
    "NdrProxyForwardingFunction4",
    "NdrProxyForwardingFunction5",
    "NdrProxyForwardingFunction6",
    "NdrProxyForwardingFunction7",
    "NdrProxyForwardingFunction8",
    "NdrProxyForwardingFunction9",
    "ObjectStublessClient10",
    "ObjectStublessClient11",
    "ObjectStublessClient12",
    "ObjectStublessClient13",
    "ObjectStublessClient14",
    "ObjectStublessClient15",
    "ObjectStublessClient16",
    "ObjectStublessClient17",
    "ObjectStublessClient18",
    "ObjectStublessClient19",
    "ObjectStublessClient20",
    "ObjectStublessClient21",
    "ObjectStublessClient22",
    "ObjectStublessClient23",
    "ObjectStublessClient24",
    "ObjectStublessClient25",
    "ObjectStublessClient26",
    "ObjectStublessClient27",
    "ObjectStublessClient28",
    "ObjectStublessClient29",
    "ObjectStublessClient3",
    "ObjectStublessClient30",
    "ObjectStublessClient31",
    "ObjectStublessClient32",
    "ObjectStublessClient4",
    "ObjectStublessClient5",
    "ObjectStublessClient6",
    "ObjectStublessClient7",
    "ObjectStublessClient8",
    "ObjectStublessClient9",
};

/* Batch 2: the core contracts Windows 11 hosts in kernelbase. Each list is the contract's functions
 * (Windows SDK 10.0.26100 apiset libraries, Win11 downlevel DLLs, Wine's contract specs) that kernelbase
 * exports; none of them was lost by moving the contract off kernel32. */

static const PCSTR Kb_CoreApiqueryL21Functions[] =
{
    "IsApiSetImplemented",
};

static const PCSTR Kb_CoreAppcompatL11Functions[] =
{
    "BaseCheckAppcompatCache",
    "BaseCleanupAppcompatCacheSupport",
    "BaseDumpAppcompatCache",
    "BaseFlushAppcompatCache",
    "BaseFreeAppCompatDataForProcess",
    "BaseInitAppcompatCacheSupport",
    "BaseIsAppcompatInfrastructureDisabled",
    "BaseReadAppCompatDataForProcess",
    "BaseUpdateAppcompatCache",
};

static const PCSTR Kb_CoreCommL11Functions[] =
{
    "ClearCommBreak",
    "ClearCommError",
    "EscapeCommFunction",
    "GetCommConfig",
    "GetCommMask",
    "GetCommModemStatus",
    "GetCommProperties",
    "GetCommState",
    "GetCommTimeouts",
    "PurgeComm",
    "SetCommBreak",
    "SetCommConfig",
    "SetCommMask",
    "SetCommState",
    "SetCommTimeouts",
    "SetupComm",
    "TransmitCommChar",
    "WaitCommEvent",
};

static const PCSTR Kb_CoreConsoleL11Functions[] =
{
    "AllocConsole",
    "GetConsoleCP",
    "GetConsoleMode",
    "GetConsoleOutputCP",
    "GetNumberOfConsoleInputEvents",
    "PeekConsoleInputA",
    "ReadConsoleA",
    "ReadConsoleInputA",
    "ReadConsoleInputW",
    "ReadConsoleW",
    "SetConsoleCtrlHandler",
    "SetConsoleMode",
    "WriteConsoleA",
    "WriteConsoleW",
};

static const PCSTR Kb_CoreConsoleL12Functions[] =
{
    "AttachConsole",
    "ClosePseudoConsole",
    "CreatePseudoConsole",
    "FreeConsole",
    "PeekConsoleInputA",
    "PeekConsoleInputW",
    "ResizePseudoConsole",
};

static const PCSTR Kb_CoreConsoleL21Functions[] =
{
    "CreateConsoleScreenBuffer",
    "FillConsoleOutputAttribute",
    "FillConsoleOutputCharacterA",
    "FillConsoleOutputCharacterW",
    "FlushConsoleInputBuffer",
    "GenerateConsoleCtrlEvent",
    "GetConsoleCursorInfo",
    "GetConsoleScreenBufferInfo",
    "GetConsoleScreenBufferInfoEx",
    "GetLargestConsoleWindowSize",
    "ReadConsoleOutputA",
    "ReadConsoleOutputAttribute",
    "ReadConsoleOutputCharacterA",
    "ReadConsoleOutputCharacterW",
    "ReadConsoleOutputW",
    "ScrollConsoleScreenBufferA",
    "ScrollConsoleScreenBufferW",
    "SetConsoleActiveScreenBuffer",
    "SetConsoleCP",
    "SetConsoleCursorInfo",
    "SetConsoleCursorPosition",
    "SetConsoleOutputCP",
    "SetConsoleScreenBufferInfoEx",
    "SetConsoleScreenBufferSize",
    "SetConsoleTextAttribute",
    "SetConsoleWindowInfo",
    "WriteConsoleInputA",
    "WriteConsoleInputW",
    "WriteConsoleOutputA",
    "WriteConsoleOutputAttribute",
    "WriteConsoleOutputCharacterA",
    "WriteConsoleOutputCharacterW",
    "WriteConsoleOutputW",
};

static const PCSTR Kb_CoreConsoleL22Functions[] =
{
    "GetConsoleOriginalTitleA",
    "GetConsoleOriginalTitleW",
    "GetConsoleTitleA",
    "GetConsoleTitleW",
    "SetConsoleTitleA",
    "SetConsoleTitleW",
};

static const PCSTR Kb_CoreConsoleL32Functions[] =
{
    "AddConsoleAliasA",
    "AddConsoleAliasW",
    "ExpungeConsoleCommandHistoryA",
    "ExpungeConsoleCommandHistoryW",
    "GetConsoleAliasA",
    "GetConsoleAliasExesA",
    "GetConsoleAliasExesLengthA",
    "GetConsoleAliasExesLengthW",
    "GetConsoleAliasExesW",
    "GetConsoleAliasW",
    "GetConsoleAliasesA",
    "GetConsoleAliasesLengthA",
    "GetConsoleAliasesLengthW",
    "GetConsoleAliasesW",
    "GetConsoleCommandHistoryA",
    "GetConsoleCommandHistoryLengthA",
    "GetConsoleCommandHistoryLengthW",
    "GetConsoleCommandHistoryW",
    "GetConsoleDisplayMode",
    "GetConsoleFontSize",
    "GetConsoleHistoryInfo",
    "GetConsoleProcessList",
    "GetConsoleSelectionInfo",
    "GetConsoleWindow",
    "GetCurrentConsoleFont",
    "GetCurrentConsoleFontEx",
    "GetNumberOfConsoleMouseButtons",
    "SetConsoleDisplayMode",
    "SetConsoleHistoryInfo",
    "SetConsoleNumberOfCommandsA",
    "SetConsoleNumberOfCommandsW",
    "SetCurrentConsoleFontEx",
};

static const PCSTR Kb_CoreDatetimeL11Functions[] =
{
    "GetDateFormatA",
    "GetDateFormatEx",
    "GetDateFormatW",
    "GetDurationFormatEx",
    "GetTimeFormatA",
    "GetTimeFormatEx",
    "GetTimeFormatW",
};

static const PCSTR Kb_CoreDebugL11Functions[] =
{
    "CheckRemoteDebuggerPresent",
    "ContinueDebugEvent",
    "DebugActiveProcess",
    "DebugActiveProcessStop",
    "DebugBreak",
    "IsDebuggerPresent",
    "OutputDebugStringA",
    "OutputDebugStringW",
    "WaitForDebugEvent",
    "WaitForDebugEventEx",
};

static const PCSTR Kb_CoreDelayloadL11Functions[] =
{
    "DelayLoadFailureHook",
    "ResolveDelayLoadedAPI",
    "ResolveDelayLoadsFromDll",
};

static const PCSTR Kb_CoreErrorhandlingL11Functions[] =
{
    "AddVectoredContinueHandler",
    "AddVectoredExceptionHandler",
    "FatalAppExitA",
    "FatalAppExitW",
    "GetErrorMode",
    "GetLastError",
    "GetThreadErrorMode",
    "RaiseException",
    "RaiseFailFastException",
    "RemoveVectoredContinueHandler",
    "RemoveVectoredExceptionHandler",
    "RestoreLastError",
    "SetErrorMode",
    "SetLastError",
    "SetThreadErrorMode",
    "SetUnhandledExceptionFilter",
    "UnhandledExceptionFilter",
};

static const PCSTR Kb_CoreFibersL11Functions[] =
{
    "FlsAlloc",
    "FlsFree",
    "FlsGetValue",
    "FlsSetValue",
    "IsThreadAFiber",
};

static const PCSTR Kb_CoreFibersL21Functions[] =
{
    "ConvertFiberToThread",
    "ConvertThreadToFiber",
    "ConvertThreadToFiberEx",
    "CreateFiber",
    "CreateFiberEx",
    "DeleteFiber",
    "SwitchToFiber",
};

static const PCSTR Kb_CoreFileL11Functions[] =
{
    "CompareFileTime",
    "CreateDirectoryA",
    "CreateDirectoryW",
    "CreateFileA",
    "CreateFileW",
    "DefineDosDeviceW",
    "DeleteFileA",
    "DeleteFileW",
    "DeleteVolumeMountPointW",
    "FileTimeToLocalFileTime",
    "FindClose",
    "FindCloseChangeNotification",
    "FindFirstChangeNotificationA",
    "FindFirstChangeNotificationW",
    "FindFirstFileA",
    "FindFirstFileExA",
    "FindFirstFileExW",
    "FindFirstFileNameW",
    "FindFirstFileW",
    "FindFirstVolumeW",
    "FindNextChangeNotification",
    "FindNextFileA",
    "FindNextFileNameW",
    "FindNextFileW",
    "FindNextVolumeW",
    "FindVolumeClose",
    "FlushFileBuffers",
    "GetDiskFreeSpaceA",
    "GetDiskFreeSpaceExA",
    "GetDiskFreeSpaceExW",
    "GetDiskFreeSpaceW",
    "GetDriveTypeA",
    "GetDriveTypeW",
    "GetFileAttributesA",
    "GetFileAttributesExA",
    "GetFileAttributesExW",
    "GetFileAttributesW",
    "GetFileInformationByHandle",
    "GetFileSize",
    "GetFileSizeEx",
    "GetFileTime",
    "GetFileType",
    "GetFinalPathNameByHandleA",
    "GetFinalPathNameByHandleW",
    "GetFullPathNameA",
    "GetFullPathNameW",
    "GetLogicalDriveStringsW",
    "GetLogicalDrives",
    "GetLongPathNameA",
    "GetLongPathNameW",
    "GetShortPathNameW",
    "GetTempFileNameW",
    "GetVolumeInformationByHandleW",
    "GetVolumeInformationW",
    "GetVolumePathNameW",
    "LocalFileTimeToFileTime",
    "LockFile",
    "LockFileEx",
    "QueryDosDeviceW",
    "ReadFile",
    "ReadFileEx",
    "ReadFileScatter",
    "RemoveDirectoryA",
    "RemoveDirectoryW",
    "SetEndOfFile",
    "SetFileAttributesA",
    "SetFileAttributesW",
    "SetFileInformationByHandle",
    "SetFilePointer",
    "SetFilePointerEx",
    "SetFileTime",
    "SetFileValidData",
    "UnlockFile",
    "UnlockFileEx",
    "WriteFile",
    "WriteFileEx",
    "WriteFileGather",
};

static const PCSTR Kb_CoreFileL12Functions[] =
{
    "AreFileApisANSI",
    "CreateFile2",
    "FindFirstStreamW",
    "FindNextStreamW",
    "GetCompressedFileSizeA",
    "GetCompressedFileSizeW",
    "GetTempFileNameA",
    "GetTempPath2A",
    "GetTempPath2W",
    "GetTempPathA",
    "GetTempPathW",
    "GetVolumeInformationA",
    "GetVolumeNameForVolumeMountPointW",
    "GetVolumePathNamesForVolumeNameW",
    "SetFileApisToANSI",
    "SetFileApisToOEM",
    "SetFileIoOverlappedRange",
};

static const PCSTR Kb_CoreFileL21Functions[] =
{
    "CopyFile2",
    "CopyFileExW",
    "CopyFileW",
    "CreateDirectoryExW",
    "CreateHardLinkA",
    "CreateHardLinkW",
    "CreateSymbolicLinkW",
    "GetFileInformationByHandleEx",
    "MoveFileExW",
    "MoveFileWithProgressW",
    "OpenFileById",
    "ReOpenFile",
    "ReadDirectoryChangesW",
    "ReplaceFileW",
};

static const PCSTR Kb_CoreHandleL11Functions[] =
{
    "CloseHandle",
    "CompareObjectHandles",
    "DuplicateHandle",
    "GetHandleInformation",
    "SetHandleInformation",
};

static const PCSTR Kb_CoreHeapL11Functions[] =
{
    "GetProcessHeap",
    "GetProcessHeaps",
    "HeapAlloc",
    "HeapCompact",
    "HeapCreate",
    "HeapDestroy",
    "HeapFree",
    "HeapLock",
    "HeapQueryInformation",
    "HeapReAlloc",
    "HeapSetInformation",
    "HeapSize",
    "HeapSummary",
    "HeapUnlock",
    "HeapValidate",
    "HeapWalk",
};

static const PCSTR Kb_CoreHeapL12Functions[] =
{
    "GetProcessHeap",
    "GetProcessHeaps",
    "HeapAlloc",
    "HeapCompact",
    "HeapCreate",
    "HeapDestroy",
    "HeapFree",
    "HeapLock",
    "HeapQueryInformation",
    "HeapReAlloc",
    "HeapSetInformation",
    "HeapSize",
    "HeapUnlock",
    "HeapValidate",
    "HeapWalk",
};

static const PCSTR Kb_CoreHeapL21Functions[] =
{
    "GlobalAlloc",
    "GlobalFree",
    "LocalAlloc",
    "LocalFree",
    "LocalLock",
    "LocalReAlloc",
    "LocalUnlock",
};

static const PCSTR Kb_CoreHeapObsoleteL11Functions[] =
{
    "GlobalAlloc",
    "GlobalFlags",
    "GlobalFree",
    "GlobalHandle",
    "GlobalLock",
    "GlobalReAlloc",
    "GlobalSize",
    "GlobalUnlock",
    "LocalAlloc",
    "LocalFlags",
    "LocalFree",
    "LocalLock",
    "LocalReAlloc",
    "LocalSize",
    "LocalUnlock",
};

static const PCSTR Kb_CoreInterlockedL11Functions[] =
{
    "InitializeSListHead",
    "InterlockedFlushSList",
    "InterlockedPopEntrySList",
    "InterlockedPushEntrySList",
    "InterlockedPushListSList",
    "QueryDepthSList",
};

static const PCSTR Kb_CoreInterlockedL12Functions[] =
{
    "InterlockedPushListSListEx",
};

static const PCSTR Kb_CoreJobL11Functions[] =
{
    "IsProcessInJob",
};

static const PCSTR Kb_CoreLargeintegerL11Functions[] =
{
    "MulDiv",
};

static const PCSTR Kb_CoreLibraryloaderL11Functions[] =
{
    "AddDllDirectory",
    "DisableThreadLibraryCalls",
    "EnumResourceLanguagesExA",
    "EnumResourceLanguagesExW",
    "EnumResourceNamesExA",
    "EnumResourceNamesExW",
    "EnumResourceTypesExA",
    "EnumResourceTypesExW",
    "FindResourceExW",
    "FindStringOrdinal",
    "FreeLibrary",
    "FreeLibraryAndExitThread",
    "FreeResource",
    "GetModuleFileNameA",
    "GetModuleFileNameW",
    "GetModuleHandleA",
    "GetModuleHandleExA",
    "GetModuleHandleExW",
    "GetModuleHandleW",
    "GetProcAddress",
    "LoadLibraryExA",
    "LoadLibraryExW",
    "LoadResource",
    "LoadStringA",
    "LoadStringW",
    "LockResource",
    "RemoveDllDirectory",
    "SetDefaultDllDirectories",
    "SizeofResource",
};

static const PCSTR Kb_CoreLocalizationL11Functions[] =
{
    "ConvertDefaultLocale",
    "FindNLSString",
    "FindNLSStringEx",
    "GetACP",
    "GetCPFileNameFromRegistry",
    "GetCPInfo",
    "GetCPInfoExW",
    "GetCalendarInfoEx",
    "GetCalendarInfoW",
    "GetFileMUIInfo",
    "GetFileMUIPath",
    "GetLocaleInfoEx",
    "GetLocaleInfoW",
    "GetNLSVersion",
    "GetNLSVersionEx",
    "GetOEMCP",
    "GetProcessPreferredUILanguages",
    "GetSystemDefaultLCID",
    "GetSystemDefaultLangID",
    "GetSystemPreferredUILanguages",
    "GetThreadLocale",
    "GetThreadPreferredUILanguages",
    "GetThreadUILanguage",
    "GetUILanguageInfo",
    "GetUserDefaultLCID",
    "GetUserDefaultLangID",
    "GetUserPreferredUILanguages",
    "IsNLSDefinedString",
    "IsValidCodePage",
    "IsValidLanguageGroup",
    "IsValidLocale",
    "IsValidLocaleName",
    "LCMapStringEx",
    "LCMapStringW",
    "LocaleNameToLCID",
    "NlsCheckPolicy",
    "NlsGetCacheUpdateCount",
    "NlsUpdateLocale",
    "NlsUpdateSystemLocale",
    "ResolveLocaleName",
    "SetCalendarInfoW",
    "SetLocaleInfoW",
    "SetThreadLocale",
    "VerLanguageNameA",
    "VerLanguageNameW",
};

static const PCSTR Kb_CoreLocalizationL12Functions[] =
{
    "ConvertDefaultLocale",
    "EnumSystemGeoID",
    "EnumSystemLocalesA",
    "EnumSystemLocalesEx",
    "EnumSystemLocalesW",
    "FindNLSString",
    "FindNLSStringEx",
    "FormatMessageA",
    "FormatMessageW",
    "GetACP",
    "GetCPInfo",
    "GetCPInfoExW",
    "GetCalendarInfoEx",
    "GetCalendarInfoW",
    "GetFileMUIInfo",
    "GetFileMUIPath",
    "GetGeoInfoEx",
    "GetGeoInfoW",
    "GetLocaleInfoA",
    "GetLocaleInfoEx",
    "GetLocaleInfoW",
    "GetNLSVersion",
    "GetNLSVersionEx",
    "GetOEMCP",
    "GetProcessPreferredUILanguages",
    "GetSystemDefaultLCID",
    "GetSystemDefaultLangID",
    "GetSystemDefaultLocaleName",
    "GetSystemPreferredUILanguages",
    "GetThreadLocale",
    "GetThreadPreferredUILanguages",
    "GetThreadUILanguage",
    "GetUILanguageInfo",
    "GetUserDefaultGeoName",
    "GetUserDefaultLCID",
    "GetUserDefaultLangID",
    "GetUserDefaultLocaleName",
    "GetUserGeoID",
    "GetUserPreferredUILanguages",
    "IdnToAscii",
    "IdnToUnicode",
    "IsDBCSLeadByte",
    "IsDBCSLeadByteEx",
    "IsNLSDefinedString",
    "IsValidCodePage",
    "IsValidLanguageGroup",
    "IsValidLocale",
    "IsValidLocaleName",
    "IsValidNLSVersion",
    "LCIDToLocaleName",
    "LCMapStringA",
    "LCMapStringEx",
    "LCMapStringW",
    "LocaleNameToLCID",
    "ResolveLocaleName",
    "SetCalendarInfoW",
    "SetLocaleInfoW",
    "SetProcessPreferredUILanguages",
    "SetThreadLocale",
    "SetThreadPreferredUILanguages",
    "SetThreadUILanguage",
    "SetUserGeoID",
    "SetUserGeoName",
    "VerLanguageNameA",
    "VerLanguageNameW",
};

static const PCSTR Kb_CoreLocalizationL21Functions[] =
{
    "EnumCalendarInfoExEx",
    "EnumCalendarInfoExW",
    "EnumCalendarInfoW",
    "EnumDateFormatsExEx",
    "EnumDateFormatsExW",
    "EnumDateFormatsW",
    "EnumSystemCodePagesW",
    "EnumTimeFormatsEx",
    "EnumTimeFormatsW",
    "GetCurrencyFormatEx",
    "GetCurrencyFormatW",
    "GetNumberFormatEx",
};

static const PCSTR Kb_CoreLocalizationObsoleteL11Functions[] =
{
    "CompareStringA",
    "EnumLanguageGroupLocalesW",
    "EnumSystemLanguageGroupsW",
    "EnumSystemLocalesEx",
    "EnumUILanguagesW",
    "GetNumberFormatW",
    "GetStringTypeA",
    "GetSystemDefaultLocaleName",
    "GetSystemDefaultUILanguage",
    "GetUserDefaultUILanguage",
    "LCIDToLocaleName",
};

static const PCSTR Kb_CoreLocalizationObsoleteL12Functions[] =
{
    "CompareStringA",
    "EnumLanguageGroupLocalesW",
    "EnumSystemLanguageGroupsW",
    "EnumUILanguagesW",
    "GetNumberFormatW",
    "GetStringTypeA",
    "GetSystemDefaultLocaleName",
    "GetSystemDefaultUILanguage",
    "GetUserDefaultUILanguage",
    "LCIDToLocaleName",
};

static const PCSTR Kb_CoreLocalizationObsoleteL13Functions[] =
{
    "CompareStringA",
    "EnumLanguageGroupLocalesW",
    "EnumSystemLanguageGroupsW",
    "EnumUILanguagesW",
    "GetNumberFormatW",
    "GetStringTypeA",
    "GetSystemDefaultLocaleName",
    "GetSystemDefaultUILanguage",
    "GetUserDefaultUILanguage",
};

static const PCSTR Kb_CoreLocalizationPrivateL11Functions[] =
{
    "GetCPFileNameFromRegistry",
    "LoadStringByReference",
    "NlsCheckPolicy",
    "NlsGetCacheUpdateCount",
    "NlsUpdateLocale",
    "NlsUpdateSystemLocale",
};

static const PCSTR Kb_CoreMemoryL11Functions[] =
{
    "AllocateUserPhysicalPages",
    "AllocateUserPhysicalPagesNuma",
    "CreateFileMappingFromApp",
    "CreateFileMappingNumaW",
    "CreateFileMappingW",
    "CreateMemoryResourceNotification",
    "DiscardVirtualMemory",
    "FlushViewOfFile",
    "FreeUserPhysicalPages",
    "GetLargePageMinimum",
    "GetProcessWorkingSetSize",
    "GetProcessWorkingSetSizeEx",
    "GetSystemFileCacheSize",
    "GetWriteWatch",
    "MapUserPhysicalPages",
    "MapViewOfFile",
#ifdef _WIN64
    "MapViewOfFile3",
#endif
    "MapViewOfFileEx",
    "MapViewOfFileFromApp",
    "OpenFileMappingFromApp",
    "OpenFileMappingW",
    "PrefetchVirtualMemory",
    "QueryMemoryResourceNotification",
    "QueryVirtualMemoryInformation",
    "ReadProcessMemory",
    "ResetWriteWatch",
    "SetProcessValidCallTargets",
    "SetProcessWorkingSetSize",
    "SetProcessWorkingSetSizeEx",
    "SetSystemFileCacheSize",
    "UnmapViewOfFile",
#ifdef _WIN64
    "UnmapViewOfFile2",
#endif
    "UnmapViewOfFileEx",
    "VirtualAlloc",
    "VirtualAlloc2",
    "VirtualAlloc2FromApp",
    "VirtualAllocEx",
    "VirtualAllocExNuma",
    "VirtualAllocFromApp",
    "VirtualFree",
    "VirtualFreeEx",
    "VirtualLock",
    "VirtualProtect",
    "VirtualProtectEx",
    "VirtualQuery",
    "VirtualQueryEx",
    "VirtualUnlock",
    "WriteProcessMemory",
};

static const PCSTR Kb_CoreMiscL11Functions[] =
{
    "EnumSystemLocalesA",
    "FatalAppExitA",
    "FatalAppExitW",
    "FormatMessageA",
    "FormatMessageW",
    "GlobalAlloc",
    "GlobalFree",
    "IsProcessInJob",
    "IsWow64Process",
    "LCMapStringA",
    "LocalAlloc",
    "LocalFree",
    "LocalLock",
    "LocalReAlloc",
    "LocalUnlock",
    "NeedCurrentDirectoryForExePathA",
    "NeedCurrentDirectoryForExePathW",
    "PulseEvent",
    "SetHandleCount",
    "Sleep",
    "Wow64DisableWow64FsRedirection",
    "Wow64RevertWow64FsRedirection",
    "lstrcmp",
    "lstrcmpA",
    "lstrcmpW",
    "lstrcmpi",
    "lstrcmpiA",
    "lstrcmpiW",
    "lstrcpyn",
    "lstrcpynA",
    "lstrcpynW",
    "lstrlen",
    "lstrlenA",
    "lstrlenW",
};

static const PCSTR Kb_CoreNamedpipeL11Functions[] =
{
    "ConnectNamedPipe",
    "CreateNamedPipeW",
    "CreatePipe",
    "DisconnectNamedPipe",
    "GetNamedPipeClientComputerNameW",
    "ImpersonateNamedPipeClient",
    "PeekNamedPipe",
    "SetNamedPipeHandleState",
    "TransactNamedPipe",
    "WaitNamedPipeW",
};

static const PCSTR Kb_CoreNamedpipeL12Functions[] =
{
    "CallNamedPipeW",
    "ConnectNamedPipe",
    "CreateNamedPipeW",
    "CreatePipe",
    "DisconnectNamedPipe",
    "GetNamedPipeClientComputerNameW",
    "GetNamedPipeHandleStateW",
    "GetNamedPipeInfo",
    "ImpersonateNamedPipeClient",
    "PeekNamedPipe",
    "SetNamedPipeHandleState",
    "TransactNamedPipe",
    "WaitNamedPipeW",
};

static const PCSTR Kb_CoreNamespaceL11Functions[] =
{
    "AddSIDToBoundaryDescriptor",
    "ClosePrivateNamespace",
    "CreateBoundaryDescriptorW",
    "CreatePrivateNamespaceW",
    "DeleteBoundaryDescriptor",
    "OpenPrivateNamespaceW",
};

static const PCSTR Kb_CoreNormalizationL11Functions[] =
{
    "GetStringScripts",
    "IdnToNameprepUnicode",
    "IsNormalizedString",
    "NormalizeString",
    "VerifyScripts",
};

static const PCSTR Kb_CoreProcessenvironmentL11Functions[] =
{
    "ExpandEnvironmentStringsA",
    "ExpandEnvironmentStringsW",
    "FreeEnvironmentStringsA",
    "FreeEnvironmentStringsW",
    "GetCommandLineA",
    "GetCommandLineW",
    "GetCurrentDirectoryA",
    "GetCurrentDirectoryW",
    "GetEnvironmentStrings",
    "GetEnvironmentStringsW",
    "GetEnvironmentVariableA",
    "GetEnvironmentVariableW",
    "GetStdHandle",
    "SearchPathW",
    "SetCurrentDirectoryA",
    "SetCurrentDirectoryW",
    "SetEnvironmentStringsW",
    "SetEnvironmentVariableA",
    "SetEnvironmentVariableW",
    "SetStdHandle",
    "SetStdHandleEx",
};

static const PCSTR Kb_CoreProcessenvironmentL12Functions[] =
{
    "NeedCurrentDirectoryForExePathA",
    "NeedCurrentDirectoryForExePathW",
    "SearchPathA",
};

static const PCSTR Kb_CoreProcesssnapshotL11Functions[] =
{
    "PssCaptureSnapshot",
    "PssFreeSnapshot",
    "PssQuerySnapshot",
};

static const PCSTR Kb_CoreProcesstopologyL11Functions[] =
{
    "GetProcessGroupAffinity",
    "GetThreadGroupAffinity",
    "SetThreadGroupAffinity",
};

static const PCSTR Kb_CoreProfileL11Functions[] =
{
    "QueryPerformanceCounter",
    "QueryPerformanceFrequency",
};

static const PCSTR Kb_CorePsapiAnsiL11Functions[] =
{
    "K32EnumPageFilesA",
    "K32GetDeviceDriverBaseNameA",
    "K32GetDeviceDriverFileNameA",
    "K32GetMappedFileNameA",
    "K32GetModuleBaseNameA",
    "K32GetModuleFileNameExA",
    "K32GetProcessImageFileNameA",
    "QueryFullProcessImageNameA",
};

static const PCSTR Kb_CorePsapiL11Functions[] =
{
    "K32EmptyWorkingSet",
    "K32EnumDeviceDrivers",
    "K32EnumPageFilesW",
    "K32EnumProcessModules",
    "K32EnumProcessModulesEx",
    "K32EnumProcesses",
    "K32GetDeviceDriverBaseNameW",
    "K32GetDeviceDriverFileNameW",
    "K32GetMappedFileNameW",
    "K32GetModuleBaseNameW",
    "K32GetModuleFileNameExW",
    "K32GetModuleInformation",
    "K32GetPerformanceInfo",
    "K32GetProcessImageFileNameW",
    "K32GetProcessMemoryInfo",
    "K32GetWsChanges",
    "K32GetWsChangesEx",
    "K32InitializeProcessForWsWatch",
    "K32QueryWorkingSet",
    "K32QueryWorkingSetEx",
    "QueryFullProcessImageNameW",
};

static const PCSTR Kb_CorePsapiObsoleteL11Functions[] =
{
    "K32EnumProcessModules",
    "K32EnumProcessModulesEx",
    "K32GetModuleBaseNameA",
    "K32GetModuleBaseNameW",
    "K32GetModuleFileNameExA",
    "K32GetModuleFileNameExW",
    "K32GetModuleInformation",
};

static const PCSTR Kb_CoreQuirksL11Functions[] =
{
    "QuirkIsEnabled",
    "QuirkIsEnabled3",
};

static const PCSTR Kb_CoreRealtimeL11Functions[] =
{
    "QueryIdleProcessorCycleTime",
    "QueryIdleProcessorCycleTimeEx",
    "QueryInterruptTime",
    "QueryInterruptTimePrecise",
    "QueryProcessCycleTime",
    "QueryThreadCycleTime",
    "QueryUnbiasedInterruptTime",
    "QueryUnbiasedInterruptTimePrecise",
};

static const PCSTR Kb_CoreSidebysideL11Functions[] =
{
    "ActivateActCtx",
    "AddRefActCtx",
    "CreateActCtxW",
    "DeactivateActCtx",
    "FindActCtxSectionGuid",
    "FindActCtxSectionStringW",
    "GetCurrentActCtx",
    "QueryActCtxSettingsW",
    "QueryActCtxW",
    "ReleaseActCtx",
    "ZombifyActCtx",
};

static const PCSTR Kb_CoreStringL11Functions[] =
{
    "CompareStringEx",
    "CompareStringOrdinal",
    "CompareStringW",
    "FoldStringW",
    "GetStringTypeExW",
    "GetStringTypeW",
    "MultiByteToWideChar",
    "WideCharToMultiByte",
};

static const PCSTR Kb_CoreStringloaderL11Functions[] =
{
    "LoadStringA",
    "LoadStringW",
};

static const PCSTR Kb_CoreSynchL11Functions[] =
{
    "AcquireSRWLockExclusive",
    "AcquireSRWLockShared",
    "CancelWaitableTimer",
    "CreateEventA",
    "CreateEventExA",
    "CreateEventExW",
    "CreateEventW",
    "CreateMutexA",
    "CreateMutexExA",
    "CreateMutexExW",
    "CreateMutexW",
    "CreateSemaphoreExW",
    "CreateWaitableTimerExW",
    "DeleteCriticalSection",
    "EnterCriticalSection",
    "InitializeCriticalSection",
    "InitializeCriticalSectionAndSpinCount",
    "InitializeCriticalSectionEx",
    "InitializeSRWLock",
    "LeaveCriticalSection",
    "OpenEventA",
    "OpenEventW",
    "OpenMutexW",
    "OpenSemaphoreW",
    "OpenWaitableTimerW",
    "ReleaseMutex",
    "ReleaseSRWLockExclusive",
    "ReleaseSRWLockShared",
    "ReleaseSemaphore",
    "ResetEvent",
    "SetCriticalSectionSpinCount",
    "SetEvent",
    "SetWaitableTimer",
    "SetWaitableTimerEx",
    "SleepEx",
    "TryAcquireSRWLockExclusive",
    "TryAcquireSRWLockShared",
    "TryEnterCriticalSection",
    "WaitForMultipleObjectsEx",
    "WaitForSingleObject",
    "WaitForSingleObjectEx",
};

static const PCSTR Kb_CoreSynchL12Functions[] =
{
    "CreateSemaphoreW",
    "CreateWaitableTimerW",
    "DeleteSynchronizationBarrier",
    "EnterSynchronizationBarrier",
    "InitOnceBeginInitialize",
    "InitOnceComplete",
    "InitOnceExecuteOnce",
    "InitOnceInitialize",
    "InitializeConditionVariable",
    "InitializeSynchronizationBarrier",
    "SignalObjectAndWait",
    "Sleep",
    "SleepConditionVariableCS",
    "SleepConditionVariableSRW",
    "WaitForMultipleObjects",
    "WaitOnAddress",
    "WakeAllConditionVariable",
    "WakeByAddressAll",
    "WakeByAddressSingle",
    "WakeConditionVariable",
};

static const PCSTR Kb_CoreSysinfoL11Functions[] =
{
    "GetComputerNameExA",
    "GetComputerNameExW",
    "GetLocalTime",
    "GetLogicalProcessorInformation",
    "GetLogicalProcessorInformationEx",
    "GetSystemDirectoryA",
    "GetSystemDirectoryW",
    "GetSystemInfo",
    "GetSystemTime",
    "GetSystemTimeAdjustment",
    "GetSystemTimeAsFileTime",
    "GetSystemWindowsDirectoryA",
    "GetSystemWindowsDirectoryW",
    "GetTickCount",
    "GetTickCount64",
    "GetVersion",
    "GetVersionExA",
    "GetVersionExW",
    "GetWindowsDirectoryA",
    "GetWindowsDirectoryW",
    "GlobalMemoryStatusEx",
    "SetLocalTime",
};

static const PCSTR Kb_CoreSysinfoL12Functions[] =
{
    "DnsHostnameToComputerNameExW",
    "EnumSystemFirmwareTables",
    "GetIntegratedDisplaySize",
    "GetNativeSystemInfo",
    "GetOsSafeBootMode",
    "GetPhysicallyInstalledSystemMemory",
    "GetProcessorSystemCycleTime",
    "GetProductInfo",
    "GetSystemFirmwareTable",
    "GetSystemTimePreciseAsFileTime",
    "SetComputerNameA",
    "SetComputerNameExA",
    "SetComputerNameExW",
    "SetComputerNameW",
    "SetSystemTime",
    "SetSystemTimeAdjustment",
    "VerSetConditionMask",
};

static const PCSTR Kb_CoreSystemtopologyL11Functions[] =
{
    "GetNumaHighestNodeNumber",
    "GetNumaNodeProcessorMask2",
    "GetNumaNodeProcessorMaskEx",
    "GetNumaProximityNodeEx",
};

static const PCSTR Kb_CoreThreadpoolL11Functions[] =
{
    "CallbackMayRunLong",
    "CancelThreadpoolIo",
    "ChangeTimerQueueTimer",
    "CloseThreadpool",
    "CloseThreadpoolCleanupGroup",
    "CloseThreadpoolCleanupGroupMembers",
    "CloseThreadpoolIo",
    "CloseThreadpoolTimer",
    "CloseThreadpoolWait",
    "CloseThreadpoolWork",
    "CreateThreadpool",
    "CreateThreadpoolCleanupGroup",
    "CreateThreadpoolIo",
    "CreateThreadpoolTimer",
    "CreateThreadpoolWait",
    "CreateThreadpoolWork",
    "CreateTimerQueue",
    "CreateTimerQueueTimer",
    "DeleteTimerQueueEx",
    "DeleteTimerQueueTimer",
    "DisassociateCurrentThreadFromCallback",
    "FreeLibraryWhenCallbackReturns",
    "IsThreadpoolTimerSet",
    "LeaveCriticalSectionWhenCallbackReturns",
    "QueryThreadpoolStackInformation",
    "RegisterWaitForSingleObjectEx",
    "ReleaseMutexWhenCallbackReturns",
    "ReleaseSemaphoreWhenCallbackReturns",
    "SetEventWhenCallbackReturns",
    "SetThreadpoolStackInformation",
    "SetThreadpoolThreadMaximum",
    "SetThreadpoolThreadMinimum",
    "SetThreadpoolTimer",
    "SetThreadpoolWait",
    "StartThreadpoolIo",
    "SubmitThreadpoolWork",
    "TrySubmitThreadpoolCallback",
    "UnregisterWaitEx",
    "WaitForThreadpoolIoCallbacks",
    "WaitForThreadpoolTimerCallbacks",
    "WaitForThreadpoolWaitCallbacks",
    "WaitForThreadpoolWorkCallbacks",
};

static const PCSTR Kb_CoreThreadpoolL12Functions[] =
{
    "CallbackMayRunLong",
    "CancelThreadpoolIo",
    "CloseThreadpool",
    "CloseThreadpoolCleanupGroup",
    "CloseThreadpoolCleanupGroupMembers",
    "CloseThreadpoolIo",
    "CloseThreadpoolTimer",
    "CloseThreadpoolWait",
    "CloseThreadpoolWork",
    "CreateThreadpool",
    "CreateThreadpoolCleanupGroup",
    "CreateThreadpoolIo",
    "CreateThreadpoolTimer",
    "CreateThreadpoolWait",
    "CreateThreadpoolWork",
    "DisassociateCurrentThreadFromCallback",
    "FreeLibraryWhenCallbackReturns",
    "IsThreadpoolTimerSet",
    "LeaveCriticalSectionWhenCallbackReturns",
    "QueryThreadpoolStackInformation",
    "ReleaseMutexWhenCallbackReturns",
    "ReleaseSemaphoreWhenCallbackReturns",
    "SetEventWhenCallbackReturns",
    "SetThreadpoolStackInformation",
    "SetThreadpoolThreadMaximum",
    "SetThreadpoolThreadMinimum",
    "SetThreadpoolTimer",
    "SetThreadpoolTimerEx",
    "SetThreadpoolWait",
    "SetThreadpoolWaitEx",
    "StartThreadpoolIo",
    "SubmitThreadpoolWork",
    "TrySubmitThreadpoolCallback",
    "WaitForThreadpoolIoCallbacks",
    "WaitForThreadpoolTimerCallbacks",
    "WaitForThreadpoolWaitCallbacks",
    "WaitForThreadpoolWorkCallbacks",
};

static const PCSTR Kb_CoreThreadpoolLegacyL11Functions[] =
{
    "ChangeTimerQueueTimer",
    "CreateTimerQueue",
    "CreateTimerQueueTimer",
    "DeleteTimerQueue",
    "DeleteTimerQueueEx",
    "DeleteTimerQueueTimer",
    "QueueUserWorkItem",
    "UnregisterWaitEx",
};

static const PCSTR Kb_CoreThreadpoolPrivateL11Functions[] =
{
    "RegisterWaitForSingleObjectEx",
};

static const PCSTR Kb_CoreTimezoneL11Functions[] =
{
    "EnumDynamicTimeZoneInformation",
    "FileTimeToSystemTime",
    "GetDynamicTimeZoneInformation",
    "GetDynamicTimeZoneInformationEffectiveYears",
    "GetTimeZoneInformation",
    "GetTimeZoneInformationForYear",
    "SetDynamicTimeZoneInformation",
    "SetTimeZoneInformation",
    "SystemTimeToFileTime",
    "SystemTimeToTzSpecificLocalTime",
    "SystemTimeToTzSpecificLocalTimeEx",
    "TzSpecificLocalTimeToSystemTime",
    "TzSpecificLocalTimeToSystemTimeEx",
};

static const PCSTR Kb_CoreWindowserrorreportingL11Functions[] =
{
    "GetApplicationRecoveryCallback",
    "GetApplicationRestartSettings",
    "RegisterApplicationRestart",
    "UnregisterApplicationRestart",
    "WerGetFlags",
    "WerRegisterFile",
    "WerRegisterMemoryBlock",
    "WerRegisterRuntimeExceptionModule",
    "WerSetFlags",
    "WerUnregisterFile",
    "WerUnregisterMemoryBlock",
    "WerUnregisterRuntimeExceptionModule",
    "WerpNotifyLoadStringResource",
    "WerpNotifyUseStringResource",
};

static const PCSTR Kb_CoreWow64L11Functions[] =
{
    "GetSystemWow64Directory2A",
    "GetSystemWow64Directory2W",
    "GetSystemWow64DirectoryA",
    "GetSystemWow64DirectoryW",
    "IsWow64GuestMachineSupported",
    "IsWow64Process",
    "IsWow64Process2",
    "Wow64DisableWow64FsRedirection",
    "Wow64EnableWow64FsRedirection",
    "Wow64GetThreadContext",
    "Wow64RevertWow64FsRedirection",
    "Wow64SetThreadContext",
    "Wow64SuspendThread",
};

static const PCSTR Kb_CoreXstateL21Functions[] =
{
    "CopyContext",
    "GetEnabledXStateFeatures",
    "GetXStateFeaturesMask",
    "InitializeContext",
    "InitializeContext2",
    "LocateXStateFeature",
    "SetXStateFeaturesMask",
};

static const PCSTR Kb_DownlevelAdvapi32L11Functions[] =
{
    "AccessCheck",
    "AccessCheckAndAuditAlarmW",
    "AccessCheckByType",
    "AccessCheckByTypeAndAuditAlarmW",
    "AccessCheckByTypeResultList",
    "AccessCheckByTypeResultListAndAuditAlarmByHandleW",
    "AccessCheckByTypeResultListAndAuditAlarmW",
    "AddAccessAllowedAce",
    "AddAccessAllowedAceEx",
    "AddAccessAllowedObjectAce",
    "AddAccessDeniedAce",
    "AddAccessDeniedAceEx",
    "AddAccessDeniedObjectAce",
    "AddAce",
    "AddAuditAccessAce",
    "AddAuditAccessAceEx",
    "AddAuditAccessObjectAce",
    "AddMandatoryAce",
    "AdjustTokenGroups",
    "AdjustTokenPrivileges",
    "AllocateAndInitializeSid",
    "AllocateLocallyUniqueId",
    "AreAllAccessesGranted",
    "AreAnyAccessesGranted",
    "CheckTokenMembership",
    "CopySid",
    "CreatePrivateObjectSecurity",
    "CreatePrivateObjectSecurityEx",
    "CreatePrivateObjectSecurityWithMultipleInheritance",
    "CreateProcessAsUserW",
    "CreateRestrictedToken",
    "CreateWellKnownSid",
    "DeleteAce",
    "DestroyPrivateObjectSecurity",
    "DuplicateToken",
    "DuplicateTokenEx",
    "EqualDomainSid",
    "EqualPrefixSid",
    "EqualSid",
    "EventActivityIdControl",
    "EventEnabled",
    "EventProviderEnabled",
    "EventRegister",
    "EventUnregister",
    "EventWrite",
    "EventWriteString",
    "EventWriteTransfer",
    "FindFirstFreeAce",
    "FreeSid",
    "GetAce",
    "GetAclInformation",
    "GetFileSecurityW",
    "GetKernelObjectSecurity",
    "GetLengthSid",
    "GetPrivateObjectSecurity",
    "GetSecurityDescriptorControl",
    "GetSecurityDescriptorDacl",
    "GetSecurityDescriptorGroup",
    "GetSecurityDescriptorLength",
    "GetSecurityDescriptorOwner",
    "GetSecurityDescriptorRMControl",
    "GetSecurityDescriptorSacl",
    "GetSidIdentifierAuthority",
    "GetSidLengthRequired",
    "GetSidSubAuthority",
    "GetSidSubAuthorityCount",
    "GetTokenInformation",
    "GetTraceEnableFlags",
    "GetTraceEnableLevel",
    "GetTraceLoggerHandle",
    "InitializeAcl",
    "InitializeSecurityDescriptor",
    "InitializeSid",
    "IsTokenRestricted",
    "IsValidAcl",
    "IsValidSecurityDescriptor",
    "IsValidSid",
    "MakeAbsoluteSD",
    "MakeSelfRelativeSD",
    "OpenProcessToken",
    "OpenThreadToken",
    "PrivilegeCheck",
    "PrivilegedServiceAuditAlarmW",
    "QuerySecurityAccessMask",
    "RegCloseKey",
    "RegCopyTreeW",
    "RegCreateKeyExA",
    "RegCreateKeyExW",
    "RegDeleteKeyExA",
    "RegDeleteKeyExW",
    "RegDeleteTreeA",
    "RegDeleteTreeW",
    "RegDeleteValueA",
    "RegDeleteValueW",
    "RegDisablePredefinedCacheEx",
    "RegEnumKeyExA",
    "RegEnumKeyExW",
    "RegEnumValueA",
    "RegEnumValueW",
    "RegFlushKey",
    "RegGetKeySecurity",
    "RegGetValueA",
    "RegGetValueW",
    "RegLoadAppKeyA",
    "RegLoadAppKeyW",
    "RegLoadKeyA",
    "RegLoadKeyW",
    "RegLoadMUIStringA",
    "RegLoadMUIStringW",
    "RegNotifyChangeKeyValue",
    "RegOpenCurrentUser",
    "RegOpenKeyExA",
    "RegOpenKeyExW",
    "RegOpenUserClassesRoot",
    "RegQueryInfoKeyA",
    "RegQueryInfoKeyW",
    "RegQueryValueExA",
    "RegQueryValueExW",
    "RegRestoreKeyA",
    "RegRestoreKeyW",
    "RegSaveKeyExA",
    "RegSaveKeyExW",
    "RegSetKeySecurity",
    "RegSetValueExA",
    "RegSetValueExW",
    "RegUnLoadKeyA",
    "RegUnLoadKeyW",
    "RegisterTraceGuidsW",
    "RevertToSelf",
    "SetAclInformation",
    "SetFileSecurityW",
    "SetKernelObjectSecurity",
    "SetSecurityAccessMask",
    "SetSecurityDescriptorControl",
    "SetSecurityDescriptorDacl",
    "SetSecurityDescriptorGroup",
    "SetSecurityDescriptorOwner",
    "SetSecurityDescriptorRMControl",
    "SetSecurityDescriptorSacl",
    "SetTokenInformation",
    "TraceEvent",
    "TraceMessage",
    "TraceMessageVa",
    "UnregisterTraceGuids",
};

static const PCSTR Kb_DownlevelNormalizL11Functions[] =
{
    "IdnToAscii",
    "IdnToUnicode",
};

static const PCSTR Kb_DownlevelShlwapiL11Functions[] =
{
    "GetAcceptLanguagesW",
    "HashData",
    "IsInternetESCEnabled",
    "ParseURLW",
    "PathAddBackslashA",
    "PathAddBackslashW",
    "PathAddExtensionA",
    "PathAddExtensionW",
    "PathAppendA",
    "PathAppendW",
    "PathCanonicalizeA",
    "PathCanonicalizeW",
    "PathCommonPrefixA",
    "PathCommonPrefixW",
    "PathCreateFromUrlAlloc",
    "PathCreateFromUrlW",
    "PathFileExistsA",
    "PathFileExistsW",
    "PathFindExtensionA",
    "PathFindExtensionW",
    "PathFindFileNameA",
    "PathFindFileNameW",
    "PathFindNextComponentA",
    "PathFindNextComponentW",
    "PathGetArgsA",
    "PathGetArgsW",
    "PathGetDriveNumberA",
    "PathGetDriveNumberW",
    "PathIsFileSpecA",
    "PathIsFileSpecW",
    "PathIsLFNFileSpecA",
    "PathIsLFNFileSpecW",
    "PathIsPrefixA",
    "PathIsPrefixW",
    "PathIsRelativeA",
    "PathIsRelativeW",
    "PathIsRootA",
    "PathIsRootW",
    "PathIsSameRootA",
    "PathIsSameRootW",
    "PathIsUNCA",
    "PathIsUNCServerA",
    "PathIsUNCServerShareA",
    "PathIsUNCServerShareW",
    "PathIsUNCServerW",
    "PathIsUNCW",
    "PathIsURLW",
    "PathParseIconLocationA",
    "PathParseIconLocationW",
    "PathRelativePathToA",
    "PathRelativePathToW",
    "PathRemoveBackslashA",
    "PathRemoveBackslashW",
    "PathRemoveBlanksA",
    "PathRemoveBlanksW",
    "PathRemoveExtensionA",
    "PathRemoveExtensionW",
    "PathRemoveFileSpecA",
    "PathRemoveFileSpecW",
    "PathRenameExtensionA",
    "PathRenameExtensionW",
    "PathSkipRootA",
    "PathSkipRootW",
    "PathStripPathA",
    "PathStripPathW",
    "PathStripToRootA",
    "PathStripToRootW",
    "PathUnquoteSpacesA",
    "PathUnquoteSpacesW",
    "QISearch",
    "SHLoadIndirectString",
    "SHRegCloseUSKey",
    "SHRegDeleteUSValueA",
    "SHRegDeleteUSValueW",
    "SHRegEnumUSKeyA",
    "SHRegEnumUSKeyW",
    "SHRegGetBoolUSValueA",
    "SHRegGetBoolUSValueW",
    "SHRegGetUSValueA",
    "SHRegGetUSValueW",
    "SHRegOpenUSKeyA",
    "SHRegOpenUSKeyW",
    "SHRegQueryUSValueA",
    "SHRegQueryUSValueW",
    "SHRegSetUSValueA",
    "SHRegSetUSValueW",
    "StrCSpnA",
    "StrCSpnIA",
    "StrCSpnIW",
    "StrCSpnW",
    "StrCatBuffA",
    "StrCatBuffW",
    "StrCatChainW",
    "StrChrA",
    "StrChrIA",
    "StrChrIW",
    "StrChrNW",
    "StrChrW",
    "StrCmpCA",
    "StrCmpCW",
    "StrCmpICA",
    "StrCmpICW",
    "StrCmpIW",
    "StrCmpLogicalW",
    "StrCmpNA",
    "StrCmpNCA",
    "StrCmpNCW",
    "StrCmpNIA",
    "StrCmpNICA",
    "StrCmpNICW",
    "StrCmpNIW",
    "StrCmpNW",
    "StrCmpW",
    "StrCpyNW",
    "StrDupA",
    "StrDupW",
    "StrIsIntlEqualA",
    "StrIsIntlEqualW",
    "StrPBrkA",
    "StrPBrkW",
    "StrRChrA",
    "StrRChrIA",
    "StrRChrIW",
    "StrRChrW",
    "StrRStrIA",
    "StrRStrIW",
    "StrSpnA",
    "StrSpnW",
    "StrStrA",
    "StrStrIA",
    "StrStrIW",
    "StrStrNIW",
    "StrStrNW",
    "StrStrW",
    "StrToInt64ExA",
    "StrToInt64ExW",
    "StrToIntA",
    "StrToIntExA",
    "StrToIntExW",
    "StrToIntW",
    "StrTrimA",
    "StrTrimW",
    "UrlApplySchemeW",
    "UrlCanonicalizeW",
    "UrlCombineA",
    "UrlCombineW",
    "UrlCreateFromPathW",
    "UrlEscapeW",
    "UrlFixupW",
    "UrlGetLocationW",
    "UrlGetPartW",
    "UrlIsW",
    "UrlUnescapeA",
    "UrlUnescapeW",
};

static const PCSTR Kb_DownlevelUser32L11Functions[] =
{
    "CharLowerA",
    "CharLowerBuffA",
    "CharLowerBuffW",
    "CharLowerW",
    "CharNextA",
    "CharNextExA",
    "CharNextW",
    "CharPrevA",
    "CharPrevExA",
    "CharPrevW",
    "CharUpperA",
    "CharUpperBuffA",
    "CharUpperBuffW",
    "CharUpperW",
    "IsCharAlphaA",
    "IsCharAlphaNumericA",
    "IsCharAlphaNumericW",
    "IsCharAlphaW",
    "IsCharLowerA",
    "IsCharLowerW",
    "IsCharUpperA",
    "IsCharUpperW",
};

static const PCSTR Kb_DownlevelVersionL11Functions[] =
{
    "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeExW",
    "VerFindFileA",
    "VerFindFileW",
    "VerQueryValueA",
    "VerQueryValueW",
};

static const PCSTR Kb_SecurityAppcontainerL11Functions[] =
{
    "GetAppContainerNamedObjectPath",
};

static const PCSTR Kb_SecurityBaseL12Functions[] =
{
    "AddResourceAttributeAce",
    "AddScopedPolicyIDAce",
    "CheckTokenCapability",
    "CheckTokenMembershipEx",
    "DeriveCapabilitySidsFromName",
    "GetAppContainerAce",
    "SetCachedSigningLevel",
};

static const PCSTR Kb_SecurityBasePrivateL11Functions[] =
{
    "IsValidRelativeSecurityDescriptor",
    "MakeAbsoluteSD2",
};

static const CONTRACT_HOST Contracts[] =
{
    { L"api-ms-win-security-base-l1-1-0.dll", L"kernelbase.dll", SecurityBaseFunctions, RTL_NUMBER_OF(SecurityBaseFunctions) },
    { L"api-ms-win-core-stringansi-l1-1-0.dll", L"kernelbase.dll", StringAnsiFunctions, RTL_NUMBER_OF(StringAnsiFunctions) },
    { L"api-ms-win-shcore-unicodeansi-l1-1-0.dll", L"shcore.dll", UnicodeAnsiFunctions, RTL_NUMBER_OF(UnicodeAnsiFunctions) },
    { L"api-ms-win-core-com-midlproxystub-l1-1-0.dll", L"combase.dll", MidlProxyStubFunctions, RTL_NUMBER_OF(MidlProxyStubFunctions) },
    { L"api-ms-win-core-apiquery-l2-1-0.dll", L"kernelbase.dll", Kb_CoreApiqueryL21Functions, RTL_NUMBER_OF(Kb_CoreApiqueryL21Functions) },
    { L"api-ms-win-core-appcompat-l1-1-0.dll", L"kernelbase.dll", Kb_CoreAppcompatL11Functions, RTL_NUMBER_OF(Kb_CoreAppcompatL11Functions) },
    { L"api-ms-win-core-appcompat-l1-1-1.dll", L"kernelbase.dll", Kb_CoreAppcompatL11Functions, RTL_NUMBER_OF(Kb_CoreAppcompatL11Functions) },
    { L"api-ms-win-core-comm-l1-1-0.dll", L"kernelbase.dll", Kb_CoreCommL11Functions, RTL_NUMBER_OF(Kb_CoreCommL11Functions) },
    { L"api-ms-win-core-comm-l1-1-2.dll", L"kernelbase.dll", Kb_CoreCommL11Functions, RTL_NUMBER_OF(Kb_CoreCommL11Functions) },
    { L"api-ms-win-core-console-l1-1-0.dll", L"kernelbase.dll", Kb_CoreConsoleL11Functions, RTL_NUMBER_OF(Kb_CoreConsoleL11Functions) },
    { L"api-ms-win-core-console-l1-2-1.dll", L"kernelbase.dll", Kb_CoreConsoleL12Functions, RTL_NUMBER_OF(Kb_CoreConsoleL12Functions) },
    { L"api-ms-win-core-console-l2-1-0.dll", L"kernelbase.dll", Kb_CoreConsoleL21Functions, RTL_NUMBER_OF(Kb_CoreConsoleL21Functions) },
    { L"api-ms-win-core-console-l2-2-0.dll", L"kernelbase.dll", Kb_CoreConsoleL22Functions, RTL_NUMBER_OF(Kb_CoreConsoleL22Functions) },
    { L"api-ms-win-core-console-l3-2-0.dll", L"kernelbase.dll", Kb_CoreConsoleL32Functions, RTL_NUMBER_OF(Kb_CoreConsoleL32Functions) },
    { L"api-ms-win-core-datetime-l1-1-0.dll", L"kernelbase.dll", Kb_CoreDatetimeL11Functions, RTL_NUMBER_OF(Kb_CoreDatetimeL11Functions) },
    { L"api-ms-win-core-datetime-l1-1-1.dll", L"kernelbase.dll", Kb_CoreDatetimeL11Functions, RTL_NUMBER_OF(Kb_CoreDatetimeL11Functions) },
    { L"api-ms-win-core-datetime-l1-1-2.dll", L"kernelbase.dll", Kb_CoreDatetimeL11Functions, RTL_NUMBER_OF(Kb_CoreDatetimeL11Functions) },
    { L"api-ms-win-core-debug-l1-1-0.dll", L"kernelbase.dll", Kb_CoreDebugL11Functions, RTL_NUMBER_OF(Kb_CoreDebugL11Functions) },
    { L"api-ms-win-core-debug-l1-1-1.dll", L"kernelbase.dll", Kb_CoreDebugL11Functions, RTL_NUMBER_OF(Kb_CoreDebugL11Functions) },
    { L"api-ms-win-core-debug-l1-1-2.dll", L"kernelbase.dll", Kb_CoreDebugL11Functions, RTL_NUMBER_OF(Kb_CoreDebugL11Functions) },
    { L"api-ms-win-core-delayload-l1-1-0.dll", L"kernelbase.dll", Kb_CoreDelayloadL11Functions, RTL_NUMBER_OF(Kb_CoreDelayloadL11Functions) },
    { L"api-ms-win-core-delayload-l1-1-1.dll", L"kernelbase.dll", Kb_CoreDelayloadL11Functions, RTL_NUMBER_OF(Kb_CoreDelayloadL11Functions) },
    { L"api-ms-win-core-errorhandling-l1-1-0.dll", L"kernelbase.dll", Kb_CoreErrorhandlingL11Functions, RTL_NUMBER_OF(Kb_CoreErrorhandlingL11Functions) },
    { L"api-ms-win-core-errorhandling-l1-1-1.dll", L"kernelbase.dll", Kb_CoreErrorhandlingL11Functions, RTL_NUMBER_OF(Kb_CoreErrorhandlingL11Functions) },
    { L"api-ms-win-core-errorhandling-l1-1-3.dll", L"kernelbase.dll", Kb_CoreErrorhandlingL11Functions, RTL_NUMBER_OF(Kb_CoreErrorhandlingL11Functions) },
    { L"api-ms-win-core-fibers-l1-1-0.dll", L"kernelbase.dll", Kb_CoreFibersL11Functions, RTL_NUMBER_OF(Kb_CoreFibersL11Functions) },
    { L"api-ms-win-core-fibers-l1-1-1.dll", L"kernelbase.dll", Kb_CoreFibersL11Functions, RTL_NUMBER_OF(Kb_CoreFibersL11Functions) },
    { L"api-ms-win-core-fibers-l1-1-2.dll", L"kernelbase.dll", Kb_CoreFibersL11Functions, RTL_NUMBER_OF(Kb_CoreFibersL11Functions) },
    { L"api-ms-win-core-fibers-l2-1-0.dll", L"kernelbase.dll", Kb_CoreFibersL21Functions, RTL_NUMBER_OF(Kb_CoreFibersL21Functions) },
    { L"api-ms-win-core-fibers-l2-1-1.dll", L"kernelbase.dll", Kb_CoreFibersL21Functions, RTL_NUMBER_OF(Kb_CoreFibersL21Functions) },
    { L"api-ms-win-core-file-l1-1-0.dll", L"kernelbase.dll", Kb_CoreFileL11Functions, RTL_NUMBER_OF(Kb_CoreFileL11Functions) },
    { L"api-ms-win-core-file-l1-1-1.dll", L"kernelbase.dll", Kb_CoreFileL11Functions, RTL_NUMBER_OF(Kb_CoreFileL11Functions) },
    { L"api-ms-win-core-file-l1-2-0.dll", L"kernelbase.dll", Kb_CoreFileL12Functions, RTL_NUMBER_OF(Kb_CoreFileL12Functions) },
    { L"api-ms-win-core-file-l1-2-1.dll", L"kernelbase.dll", Kb_CoreFileL12Functions, RTL_NUMBER_OF(Kb_CoreFileL12Functions) },
    { L"api-ms-win-core-file-l1-2-3.dll", L"kernelbase.dll", Kb_CoreFileL12Functions, RTL_NUMBER_OF(Kb_CoreFileL12Functions) },
    { L"api-ms-win-core-file-l2-1-0.dll", L"kernelbase.dll", Kb_CoreFileL21Functions, RTL_NUMBER_OF(Kb_CoreFileL21Functions) },
    { L"api-ms-win-core-file-l2-1-1.dll", L"kernelbase.dll", Kb_CoreFileL21Functions, RTL_NUMBER_OF(Kb_CoreFileL21Functions) },
    { L"api-ms-win-core-file-l2-1-3.dll", L"kernelbase.dll", Kb_CoreFileL21Functions, RTL_NUMBER_OF(Kb_CoreFileL21Functions) },
    { L"api-ms-win-core-handle-l1-1-0.dll", L"kernelbase.dll", Kb_CoreHandleL11Functions, RTL_NUMBER_OF(Kb_CoreHandleL11Functions) },
    { L"api-ms-win-core-heap-l1-1-0.dll", L"kernelbase.dll", Kb_CoreHeapL11Functions, RTL_NUMBER_OF(Kb_CoreHeapL11Functions) },
    { L"api-ms-win-core-heap-l1-2-0.dll", L"kernelbase.dll", Kb_CoreHeapL12Functions, RTL_NUMBER_OF(Kb_CoreHeapL12Functions) },
    { L"api-ms-win-core-heap-l2-1-0.dll", L"kernelbase.dll", Kb_CoreHeapL21Functions, RTL_NUMBER_OF(Kb_CoreHeapL21Functions) },
    { L"api-ms-win-core-heap-obsolete-l1-1-0.dll", L"kernelbase.dll", Kb_CoreHeapObsoleteL11Functions, RTL_NUMBER_OF(Kb_CoreHeapObsoleteL11Functions) },
    { L"api-ms-win-core-interlocked-l1-1-0.dll", L"kernelbase.dll", Kb_CoreInterlockedL11Functions, RTL_NUMBER_OF(Kb_CoreInterlockedL11Functions) },
    { L"api-ms-win-core-interlocked-l1-1-1.dll", L"kernelbase.dll", Kb_CoreInterlockedL11Functions, RTL_NUMBER_OF(Kb_CoreInterlockedL11Functions) },
    { L"api-ms-win-core-interlocked-l1-2-0.dll", L"kernelbase.dll", Kb_CoreInterlockedL12Functions, RTL_NUMBER_OF(Kb_CoreInterlockedL12Functions) },
    { L"api-ms-win-core-job-l1-1-0.dll", L"kernelbase.dll", Kb_CoreJobL11Functions, RTL_NUMBER_OF(Kb_CoreJobL11Functions) },
    { L"api-ms-win-core-largeinteger-l1-1-0.dll", L"kernelbase.dll", Kb_CoreLargeintegerL11Functions, RTL_NUMBER_OF(Kb_CoreLargeintegerL11Functions) },
    { L"api-ms-win-core-libraryloader-l1-1-1.dll", L"kernelbase.dll", Kb_CoreLibraryloaderL11Functions, RTL_NUMBER_OF(Kb_CoreLibraryloaderL11Functions) },
    { L"api-ms-win-core-localization-l1-1-0.dll", L"kernelbase.dll", Kb_CoreLocalizationL11Functions, RTL_NUMBER_OF(Kb_CoreLocalizationL11Functions) },
    { L"api-ms-win-core-localization-l1-2-0.dll", L"kernelbase.dll", Kb_CoreLocalizationL12Functions, RTL_NUMBER_OF(Kb_CoreLocalizationL12Functions) },
    { L"api-ms-win-core-localization-l1-2-1.dll", L"kernelbase.dll", Kb_CoreLocalizationL12Functions, RTL_NUMBER_OF(Kb_CoreLocalizationL12Functions) },
    { L"api-ms-win-core-localization-l1-2-4.dll", L"kernelbase.dll", Kb_CoreLocalizationL12Functions, RTL_NUMBER_OF(Kb_CoreLocalizationL12Functions) },
    { L"api-ms-win-core-localization-l2-1-0.dll", L"kernelbase.dll", Kb_CoreLocalizationL21Functions, RTL_NUMBER_OF(Kb_CoreLocalizationL21Functions) },
    { L"api-ms-win-core-localization-obsolete-l1-1-0.dll", L"kernelbase.dll", Kb_CoreLocalizationObsoleteL11Functions, RTL_NUMBER_OF(Kb_CoreLocalizationObsoleteL11Functions) },
    { L"api-ms-win-core-localization-obsolete-l1-2-0.dll", L"kernelbase.dll", Kb_CoreLocalizationObsoleteL12Functions, RTL_NUMBER_OF(Kb_CoreLocalizationObsoleteL12Functions) },
    { L"api-ms-win-core-localization-obsolete-l1-3-0.dll", L"kernelbase.dll", Kb_CoreLocalizationObsoleteL13Functions, RTL_NUMBER_OF(Kb_CoreLocalizationObsoleteL13Functions) },
    { L"api-ms-win-core-localization-private-l1-1-0.dll", L"kernelbase.dll", Kb_CoreLocalizationPrivateL11Functions, RTL_NUMBER_OF(Kb_CoreLocalizationPrivateL11Functions) },
    { L"api-ms-win-core-memory-l1-1-0.dll", L"kernelbase.dll", Kb_CoreMemoryL11Functions, RTL_NUMBER_OF(Kb_CoreMemoryL11Functions) },
    { L"api-ms-win-core-memory-l1-1-1.dll", L"kernelbase.dll", Kb_CoreMemoryL11Functions, RTL_NUMBER_OF(Kb_CoreMemoryL11Functions) },
    { L"api-ms-win-core-memory-l1-1-2.dll", L"kernelbase.dll", Kb_CoreMemoryL11Functions, RTL_NUMBER_OF(Kb_CoreMemoryL11Functions) },
    { L"api-ms-win-core-memory-l1-1-7.dll", L"kernelbase.dll", Kb_CoreMemoryL11Functions, RTL_NUMBER_OF(Kb_CoreMemoryL11Functions) },
    { L"api-ms-win-core-misc-l1-1-0.dll", L"kernelbase.dll", Kb_CoreMiscL11Functions, RTL_NUMBER_OF(Kb_CoreMiscL11Functions) },
    { L"api-ms-win-core-namedpipe-l1-1-0.dll", L"kernelbase.dll", Kb_CoreNamedpipeL11Functions, RTL_NUMBER_OF(Kb_CoreNamedpipeL11Functions) },
    { L"api-ms-win-core-namedpipe-l1-2-0.dll", L"kernelbase.dll", Kb_CoreNamedpipeL12Functions, RTL_NUMBER_OF(Kb_CoreNamedpipeL12Functions) },
    { L"api-ms-win-core-namedpipe-l1-2-2.dll", L"kernelbase.dll", Kb_CoreNamedpipeL12Functions, RTL_NUMBER_OF(Kb_CoreNamedpipeL12Functions) },
    { L"api-ms-win-core-namespace-l1-1-0.dll", L"kernelbase.dll", Kb_CoreNamespaceL11Functions, RTL_NUMBER_OF(Kb_CoreNamespaceL11Functions) },
    { L"api-ms-win-core-normalization-l1-1-0.dll", L"kernelbase.dll", Kb_CoreNormalizationL11Functions, RTL_NUMBER_OF(Kb_CoreNormalizationL11Functions) },
    { L"api-ms-win-core-processenvironment-l1-1-0.dll", L"kernelbase.dll", Kb_CoreProcessenvironmentL11Functions, RTL_NUMBER_OF(Kb_CoreProcessenvironmentL11Functions) },
    { L"api-ms-win-core-processenvironment-l1-1-1.dll", L"kernelbase.dll", Kb_CoreProcessenvironmentL11Functions, RTL_NUMBER_OF(Kb_CoreProcessenvironmentL11Functions) },
    { L"api-ms-win-core-processenvironment-l1-2-0.dll", L"kernelbase.dll", Kb_CoreProcessenvironmentL12Functions, RTL_NUMBER_OF(Kb_CoreProcessenvironmentL12Functions) },
    { L"api-ms-win-core-processsnapshot-l1-1-0.dll", L"kernelbase.dll", Kb_CoreProcesssnapshotL11Functions, RTL_NUMBER_OF(Kb_CoreProcesssnapshotL11Functions) },
    { L"api-ms-win-core-processtopology-l1-1-0.dll", L"kernelbase.dll", Kb_CoreProcesstopologyL11Functions, RTL_NUMBER_OF(Kb_CoreProcesstopologyL11Functions) },
    { L"api-ms-win-core-profile-l1-1-0.dll", L"kernelbase.dll", Kb_CoreProfileL11Functions, RTL_NUMBER_OF(Kb_CoreProfileL11Functions) },
    { L"api-ms-win-core-psapi-ansi-l1-1-0.dll", L"kernelbase.dll", Kb_CorePsapiAnsiL11Functions, RTL_NUMBER_OF(Kb_CorePsapiAnsiL11Functions) },
    { L"api-ms-win-core-psapi-l1-1-0.dll", L"kernelbase.dll", Kb_CorePsapiL11Functions, RTL_NUMBER_OF(Kb_CorePsapiL11Functions) },
    { L"api-ms-win-core-psapi-obsolete-l1-1-0.dll", L"kernelbase.dll", Kb_CorePsapiObsoleteL11Functions, RTL_NUMBER_OF(Kb_CorePsapiObsoleteL11Functions) },
    { L"api-ms-win-core-quirks-l1-1-0.dll", L"kernelbase.dll", Kb_CoreQuirksL11Functions, RTL_NUMBER_OF(Kb_CoreQuirksL11Functions) },
    { L"api-ms-win-core-quirks-l1-1-1.dll", L"kernelbase.dll", Kb_CoreQuirksL11Functions, RTL_NUMBER_OF(Kb_CoreQuirksL11Functions) },
    { L"api-ms-win-core-realtime-l1-1-0.dll", L"kernelbase.dll", Kb_CoreRealtimeL11Functions, RTL_NUMBER_OF(Kb_CoreRealtimeL11Functions) },
    { L"api-ms-win-core-realtime-l1-1-1.dll", L"kernelbase.dll", Kb_CoreRealtimeL11Functions, RTL_NUMBER_OF(Kb_CoreRealtimeL11Functions) },
    { L"api-ms-win-core-realtime-l1-1-2.dll", L"kernelbase.dll", Kb_CoreRealtimeL11Functions, RTL_NUMBER_OF(Kb_CoreRealtimeL11Functions) },
    { L"api-ms-win-core-sidebyside-l1-1-0.dll", L"kernelbase.dll", Kb_CoreSidebysideL11Functions, RTL_NUMBER_OF(Kb_CoreSidebysideL11Functions) },
    { L"api-ms-win-core-string-l1-1-0.dll", L"kernelbase.dll", Kb_CoreStringL11Functions, RTL_NUMBER_OF(Kb_CoreStringL11Functions) },
    { L"api-ms-win-core-stringloader-l1-1-0.dll", L"kernelbase.dll", Kb_CoreStringloaderL11Functions, RTL_NUMBER_OF(Kb_CoreStringloaderL11Functions) },
    { L"api-ms-win-core-stringloader-l1-1-1.dll", L"kernelbase.dll", Kb_CoreStringloaderL11Functions, RTL_NUMBER_OF(Kb_CoreStringloaderL11Functions) },
    { L"api-ms-win-core-synch-l1-1-0.dll", L"kernelbase.dll", Kb_CoreSynchL11Functions, RTL_NUMBER_OF(Kb_CoreSynchL11Functions) },
    { L"api-ms-win-core-synch-l1-1-1.dll", L"kernelbase.dll", Kb_CoreSynchL11Functions, RTL_NUMBER_OF(Kb_CoreSynchL11Functions) },
    { L"api-ms-win-core-synch-l1-2-1.dll", L"kernelbase.dll", Kb_CoreSynchL12Functions, RTL_NUMBER_OF(Kb_CoreSynchL12Functions) },
    { L"api-ms-win-core-sysinfo-l1-1-0.dll", L"kernelbase.dll", Kb_CoreSysinfoL11Functions, RTL_NUMBER_OF(Kb_CoreSysinfoL11Functions) },
    { L"api-ms-win-core-sysinfo-l1-1-1.dll", L"kernelbase.dll", Kb_CoreSysinfoL11Functions, RTL_NUMBER_OF(Kb_CoreSysinfoL11Functions) },
    { L"api-ms-win-core-sysinfo-l1-2-0.dll", L"kernelbase.dll", Kb_CoreSysinfoL12Functions, RTL_NUMBER_OF(Kb_CoreSysinfoL12Functions) },
    { L"api-ms-win-core-sysinfo-l1-2-1.dll", L"kernelbase.dll", Kb_CoreSysinfoL12Functions, RTL_NUMBER_OF(Kb_CoreSysinfoL12Functions) },
    { L"api-ms-win-core-sysinfo-l1-2-6.dll", L"kernelbase.dll", Kb_CoreSysinfoL12Functions, RTL_NUMBER_OF(Kb_CoreSysinfoL12Functions) },
    { L"api-ms-win-core-systemtopology-l1-1-0.dll", L"kernelbase.dll", Kb_CoreSystemtopologyL11Functions, RTL_NUMBER_OF(Kb_CoreSystemtopologyL11Functions) },
    { L"api-ms-win-core-systemtopology-l1-1-1.dll", L"kernelbase.dll", Kb_CoreSystemtopologyL11Functions, RTL_NUMBER_OF(Kb_CoreSystemtopologyL11Functions) },
    { L"api-ms-win-core-threadpool-l1-1-0.dll", L"kernelbase.dll", Kb_CoreThreadpoolL11Functions, RTL_NUMBER_OF(Kb_CoreThreadpoolL11Functions) },
    { L"api-ms-win-core-threadpool-l1-2-0.dll", L"kernelbase.dll", Kb_CoreThreadpoolL12Functions, RTL_NUMBER_OF(Kb_CoreThreadpoolL12Functions) },
    { L"api-ms-win-core-threadpool-legacy-l1-1-0.dll", L"kernelbase.dll", Kb_CoreThreadpoolLegacyL11Functions, RTL_NUMBER_OF(Kb_CoreThreadpoolLegacyL11Functions) },
    { L"api-ms-win-core-threadpool-private-l1-1-0.dll", L"kernelbase.dll", Kb_CoreThreadpoolPrivateL11Functions, RTL_NUMBER_OF(Kb_CoreThreadpoolPrivateL11Functions) },
    { L"api-ms-win-core-timezone-l1-1-0.dll", L"kernelbase.dll", Kb_CoreTimezoneL11Functions, RTL_NUMBER_OF(Kb_CoreTimezoneL11Functions) },
    { L"api-ms-win-core-timezone-l1-1-1.dll", L"kernelbase.dll", Kb_CoreTimezoneL11Functions, RTL_NUMBER_OF(Kb_CoreTimezoneL11Functions) },
    { L"api-ms-win-core-windowserrorreporting-l1-1-0.dll", L"kernelbase.dll", Kb_CoreWindowserrorreportingL11Functions, RTL_NUMBER_OF(Kb_CoreWindowserrorreportingL11Functions) },
    { L"api-ms-win-core-windowserrorreporting-l1-1-3.dll", L"kernelbase.dll", Kb_CoreWindowserrorreportingL11Functions, RTL_NUMBER_OF(Kb_CoreWindowserrorreportingL11Functions) },
    { L"api-ms-win-core-wow64-l1-1-0.dll", L"kernelbase.dll", Kb_CoreWow64L11Functions, RTL_NUMBER_OF(Kb_CoreWow64L11Functions) },
    { L"api-ms-win-core-wow64-l1-1-3.dll", L"kernelbase.dll", Kb_CoreWow64L11Functions, RTL_NUMBER_OF(Kb_CoreWow64L11Functions) },
    { L"api-ms-win-core-xstate-l2-1-0.dll", L"kernelbase.dll", Kb_CoreXstateL21Functions, RTL_NUMBER_OF(Kb_CoreXstateL21Functions) },
    { L"api-ms-win-core-xstate-l2-1-1.dll", L"kernelbase.dll", Kb_CoreXstateL21Functions, RTL_NUMBER_OF(Kb_CoreXstateL21Functions) },
    { L"api-ms-win-downlevel-advapi32-l1-1-0.dll", L"kernelbase.dll", Kb_DownlevelAdvapi32L11Functions, RTL_NUMBER_OF(Kb_DownlevelAdvapi32L11Functions) },
    { L"api-ms-win-downlevel-advapi32-l1-1-1.dll", L"kernelbase.dll", Kb_DownlevelAdvapi32L11Functions, RTL_NUMBER_OF(Kb_DownlevelAdvapi32L11Functions) },
    { L"api-ms-win-downlevel-normaliz-l1-1-0.dll", L"kernelbase.dll", Kb_DownlevelNormalizL11Functions, RTL_NUMBER_OF(Kb_DownlevelNormalizL11Functions) },
    { L"api-ms-win-downlevel-shlwapi-l1-1-1.dll", L"kernelbase.dll", Kb_DownlevelShlwapiL11Functions, RTL_NUMBER_OF(Kb_DownlevelShlwapiL11Functions) },
    { L"api-ms-win-downlevel-user32-l1-1-0.dll", L"kernelbase.dll", Kb_DownlevelUser32L11Functions, RTL_NUMBER_OF(Kb_DownlevelUser32L11Functions) },
    { L"api-ms-win-downlevel-user32-l1-1-1.dll", L"kernelbase.dll", Kb_DownlevelUser32L11Functions, RTL_NUMBER_OF(Kb_DownlevelUser32L11Functions) },
    { L"api-ms-win-downlevel-version-l1-1-0.dll", L"kernelbase.dll", Kb_DownlevelVersionL11Functions, RTL_NUMBER_OF(Kb_DownlevelVersionL11Functions) },
    { L"api-ms-win-security-appcontainer-l1-1-0.dll", L"kernelbase.dll", Kb_SecurityAppcontainerL11Functions, RTL_NUMBER_OF(Kb_SecurityAppcontainerL11Functions) },
    { L"api-ms-win-security-base-l1-2-2.dll", L"kernelbase.dll", Kb_SecurityBaseL12Functions, RTL_NUMBER_OF(Kb_SecurityBaseL12Functions) },
    { L"api-ms-win-security-base-private-l1-1-0.dll", L"kernelbase.dll", Kb_SecurityBasePrivateL11Functions, RTL_NUMBER_OF(Kb_SecurityBasePrivateL11Functions) },
    { L"api-ms-win-security-base-private-l1-1-1.dll", L"kernelbase.dll", Kb_SecurityBasePrivateL11Functions, RTL_NUMBER_OF(Kb_SecurityBasePrivateL11Functions) },
};

static void
test_contract(const CONTRACT_HOST *Entry)
{
    HMODULE Contract, Host;
    ULONG Index, Missing = 0;

    winetest_push_context("%S", Entry->Contract);

    Host = LoadLibraryW(Entry->Host);
    ok(Host != NULL, "LoadLibrary(%S) failed: %lu\n", Entry->Host, GetLastError());

    Contract = LoadLibraryW(Entry->Contract);
    ok(Contract != NULL, "LoadLibrary failed: %lu\n", GetLastError());
    if (Contract == NULL)
    {
        winetest_pop_context();
        if (Host)
            FreeLibrary(Host);
        return;
    }

    ok(Contract == Host, "resolved to %p, expected %S at %p\n", Contract, Entry->Host, Host);

    for (Index = 0; Index < Entry->Count; Index++)
    {
        if (GetProcAddress(Contract, Entry->Functions[Index]) == NULL)
        {
            ok(0, "%s is not exported\n", Entry->Functions[Index]);
            Missing++;
        }
    }
    ok(Missing == 0, "%lu of %lu functions are not exported\n", Missing, Entry->Count);

    FreeLibrary(Contract);
    if (Host)
        FreeLibrary(Host);
    winetest_pop_context();
}

typedef LPSTR (WINAPI *PCHARLOWERA)(LPSTR);
typedef INT (WINAPI *PSHANSITOUNICODE)(LPCSTR, LPWSTR, INT);

static void
test_calls(void)
{
    HMODULE Module;
    PCHARLOWERA pCharLowerA;
    PSHANSITOUNICODE pSHAnsiToUnicode;
    CHAR Text[] = "OROSI";
    WCHAR Wide[8];

    Module = LoadLibraryW(L"api-ms-win-core-stringansi-l1-1-0.dll");
    pCharLowerA = Module ? (PCHARLOWERA)GetProcAddress(Module, "CharLowerA") : NULL;
    if (pCharLowerA)
    {
        pCharLowerA(Text);
        ok(lstrcmpA(Text, "orosi") == 0, "CharLowerA gave %s\n", Text);
    }
    else
    {
        skip("CharLowerA is not available\n");
    }
    if (Module)
        FreeLibrary(Module);

    Module = LoadLibraryW(L"api-ms-win-shcore-unicodeansi-l1-1-0.dll");
    pSHAnsiToUnicode = Module ? (PSHANSITOUNICODE)GetProcAddress(Module, "SHAnsiToUnicode") : NULL;
    if (pSHAnsiToUnicode)
    {
        ok(pSHAnsiToUnicode("Orosi", Wide, RTL_NUMBER_OF(Wide)) == 6, "SHAnsiToUnicode returned the wrong length\n");
        ok(lstrcmpW(Wide, L"Orosi") == 0, "SHAnsiToUnicode gave %S\n", Wide);
    }
    else
    {
        skip("SHAnsiToUnicode is not available\n");
    }
    if (Module)
        FreeLibrary(Module);
}

START_TEST(apisethosts)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(Contracts); Index++)
        test_contract(&Contracts[Index]);
    test_calls();
}
