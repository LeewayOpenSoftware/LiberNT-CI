/*
 * PROJECT:     LiberNT PnP Utility
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Manage driver packages and devices from the command line
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <objbase.h>
#include <reason.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <cfg.h>
#include <newdev.h>
#include <regstr.h>
#include <umpnpmgr/drvinst.h>
#include <initguid.h>
#include <devpkey.h>

#include <conutils.h>

#include "resource.h"

#define PROBLEM_NAME(x) { x, L"" #x }

typedef struct _ADD_CONTEXT
{
    BOOL Install;
    BOOL SubDirs;
    DWORD Total;
    DWORD Added;
    BOOL NeedReboot;
    DWORD Error;
} ADD_CONTEXT, *PADD_CONTEXT;

typedef struct _ENUM_FILTER
{
    BOOL Connected;
    BOOL Disconnected;
    BOOL Problem;
    BOOL HasProblemCode;
    ULONG ProblemCode;
    BOOL Ids;
    BOOL Relations;
    BOOL Services;
    BOOL Stack;
    BOOL Location;
    BOOL Interfaces;
    BOOL Resources;
    PCWSTR InstanceId;
    PCWSTR Class;
} ENUM_FILTER, *PENUM_FILTER;

static const struct
{
    ULONG Code;
    PCWSTR Name;
} ProblemNames[] =
{
    PROBLEM_NAME(CM_PROB_NOT_CONFIGURED),
    PROBLEM_NAME(CM_PROB_DEVLOADER_FAILED),
    PROBLEM_NAME(CM_PROB_OUT_OF_MEMORY),
    PROBLEM_NAME(CM_PROB_ENTRY_IS_WRONG_TYPE),
    PROBLEM_NAME(CM_PROB_LACKED_ARBITRATOR),
    PROBLEM_NAME(CM_PROB_BOOT_CONFIG_CONFLICT),
    PROBLEM_NAME(CM_PROB_FAILED_FILTER),
    PROBLEM_NAME(CM_PROB_DEVLOADER_NOT_FOUND),
    PROBLEM_NAME(CM_PROB_INVALID_DATA),
    PROBLEM_NAME(CM_PROB_FAILED_START),
    PROBLEM_NAME(CM_PROB_LIAR),
    PROBLEM_NAME(CM_PROB_NORMAL_CONFLICT),
    PROBLEM_NAME(CM_PROB_NOT_VERIFIED),
    PROBLEM_NAME(CM_PROB_NEED_RESTART),
    PROBLEM_NAME(CM_PROB_REENUMERATION),
    PROBLEM_NAME(CM_PROB_PARTIAL_LOG_CONF),
    PROBLEM_NAME(CM_PROB_UNKNOWN_RESOURCE),
    PROBLEM_NAME(CM_PROB_REINSTALL),
    PROBLEM_NAME(CM_PROB_REGISTRY),
    PROBLEM_NAME(CM_PROB_VXDLDR),
    PROBLEM_NAME(CM_PROB_WILL_BE_REMOVED),
    PROBLEM_NAME(CM_PROB_DISABLED),
    PROBLEM_NAME(CM_PROB_DEVLOADER_NOT_READY),
    PROBLEM_NAME(CM_PROB_DEVICE_NOT_THERE),
    PROBLEM_NAME(CM_PROB_MOVED),
    PROBLEM_NAME(CM_PROB_TOO_EARLY),
    PROBLEM_NAME(CM_PROB_NO_VALID_LOG_CONF),
    PROBLEM_NAME(CM_PROB_FAILED_INSTALL),
    PROBLEM_NAME(CM_PROB_HARDWARE_DISABLED),
    PROBLEM_NAME(CM_PROB_CANT_SHARE_IRQ),
    PROBLEM_NAME(CM_PROB_FAILED_ADD),
    PROBLEM_NAME(CM_PROB_DISABLED_SERVICE),
    PROBLEM_NAME(CM_PROB_TRANSLATION_FAILED),
    PROBLEM_NAME(CM_PROB_NO_SOFTCONFIG),
    PROBLEM_NAME(CM_PROB_BIOS_TABLE),
    PROBLEM_NAME(CM_PROB_IRQ_TRANSLATION_FAILED),
    PROBLEM_NAME(CM_PROB_FAILED_DRIVER_ENTRY),
    PROBLEM_NAME(CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD),
    PROBLEM_NAME(CM_PROB_DRIVER_FAILED_LOAD),
    PROBLEM_NAME(CM_PROB_DRIVER_SERVICE_KEY_INVALID),
    PROBLEM_NAME(CM_PROB_LEGACY_SERVICE_NO_DEVICES),
    PROBLEM_NAME(CM_PROB_DUPLICATE_DEVICE),
    PROBLEM_NAME(CM_PROB_FAILED_POST_START),
    PROBLEM_NAME(CM_PROB_HALTED),
    PROBLEM_NAME(CM_PROB_PHANTOM),
    PROBLEM_NAME(CM_PROB_SYSTEM_SHUTDOWN),
    PROBLEM_NAME(CM_PROB_HELD_FOR_EJECT),
    PROBLEM_NAME(CM_PROB_DRIVER_BLOCKED),
    PROBLEM_NAME(CM_PROB_REGISTRY_TOO_LARGE),
    PROBLEM_NAME(CM_PROB_SETPROPERTIES_FAILED),
    PROBLEM_NAME(CM_PROB_WAITING_ON_DEPENDENCY),
    PROBLEM_NAME(CM_PROB_UNSIGNED_DRIVER),
};

static const WCHAR UsageText[] =
    L"PNPUTIL [/add-driver <...> | /delete-driver <...> |\n"
    L"         /export-driver <...> | /enum-drivers | /enum-devices [<...>] |\n"
    L"         /enum-classes [<...>] | /enum-interfaces [<...>] |\n"
    L"         /enum-devicetree [<...>] | /enum-containers [<...>] |\n"
    L"         /disable-device <...> | /enable-device <...> |\n"
    L"         /restart-device <...> | /remove-device <...> |\n"
    L"         /rollback-device <...> | /scan-devices | /?]\n"
    L"\n"
    L"Commands:\n"
    L"\n"
    L"  /add-driver <filename.inf | *.inf> [/subdirs] [/install] [/reboot]\n"
    L"\n"
    L"    Add driver package(s) into the driver store.\n"
    L"      /subdirs - traverse sub directories for driver packages.\n"
    L"      /install - install/update drivers on any matching devices.\n"
    L"      /reboot - reboot system if needed to complete the operation.\n"
    L"\n"
    L"  /delete-driver <oem#.inf> [/uninstall] [/force] [/reboot]\n"
    L"\n"
    L"    Delete driver package from the driver store.\n"
    L"      /uninstall - uninstall driver package from any devices using it.\n"
    L"      /force - delete driver package even when it is in use by devices.\n"
    L"      /reboot - reboot system if needed to complete the operation.\n"
    L"\n"
    L"  /export-driver <oem#.inf | *> <target directory>\n"
    L"\n"
    L"    Export driver package(s) from the driver store into a target directory.\n"
    L"\n"
    L"  /enum-drivers\n"
    L"\n"
    L"    Enumerate all 3rd party driver packages in the driver store.\n"
    L"\n"
    L"  /enum-devices [/connected | /disconnected] [/instanceid <instance ID>]\n"
    L"                [/class <name | GUID>] [/problem [<code>]] [/ids]\n"
    L"\n"
    L"    Enumerate all devices on the system.\n"
    L"      /connected | /disconnected - filter by connected or disconnected devices.\n"
    L"      /instanceid <instance ID> - filter by device instance ID.\n"
    L"      /class <name | GUID> - filter by device class name or GUID.\n"
    L"      /problem [<code>] - filter by devices with problems or specific problem code.\n"
    L"      /ids - display hardware IDs and compatible IDs.\n"
    L"\n"
    L"  /enum-containers [/containerid <container ID>] [/connected | /disconnected] [/problem]\n"
    L"                      [/devices] [/format <txt | xml | csv>] [/output-file [<filename>]]\n"
    L"\n"
    L"    Enumerate all device containers on the system.\n"
    L"      /containerid <container ID> - filter by container ID.\n"
    L"      /connected | /disconnected - filter by connected or disconnected containers.\n"
    L"      /problem - filter by containers with devices that have problems.\n"
    L"      /devices - display the devices of each container.\n"
    L"\n"
    L"  /enum-devicetree [<root instance ID>] [/connected] [/services] [/interfaces]\n"
    L"\n"
    L"    Enumerate the device tree, optionally below a root device.\n"
    L"      /connected - show connected devices only.\n"
    L"      /services - show the service and filter drivers of each device.\n"
    L"      /interfaces - show the device interfaces of each device.\n"
    L"\n"
    L"  /enum-interfaces [/enabled | /disabled] [/class <GUID>] [/instanceid <instance ID>]\n"
    L"                    [/format <txt | xml | csv>] [/output-file [<filename>]]\n"
    L"\n"
    L"    Enumerate all device interfaces on the system.\n"
    L"      /enabled | /disabled - filter by enabled or disabled interfaces.\n"
    L"      /class <GUID> - filter by interface class GUID.\n"
    L"      /instanceid <instance ID> - filter by device instance ID.\n"
    L"\n"
    L"  /enum-classes [/class <name | GUID>] [/services] [/format <txt | xml | csv>]\n"
    L"                [/output-file [<filename>]]\n"
    L"\n"
    L"    Enumerate all device setup classes on the system.\n"
    L"      /class <name | GUID> - filter by class name or GUID.\n"
    L"      /services - display the class default service and filter drivers.\n"
    L"\n"
    L"  /disable-device <instance ID> [/reboot]\n"
    L"  /enable-device <instance ID> [/reboot]\n"
    L"  /restart-device <instance ID> [/reboot]\n"
    L"  /remove-device <instance ID> [/reboot]\n"
    L"  /rollback-device <instance ID>\n"
    L"\n"
    L"    Disable, enable, restart, remove or roll back a device.\n"
    L"      /reboot - reboot system if needed to complete the operation.\n"
    L"\n"
    L"  /scan-devices\n"
    L"\n"
    L"    Scan the system for any device hardware changes.\n"
    L"\n"
    L"Legacy Commands:\n"
    L"\n"
    L"  [-i] -a <filename.inf>  ==>  /add-driver <filename.inf> [/install]\n"
    L"  [-f] -d <oem#.inf>      ==>  /delete-driver <oem#.inf> [/force]\n"
    L"  -e                      ==>  /enum-drivers\n";

static BOOL
IsOption(
    _In_ PCWSTR Argument,
    _In_ PCWSTR Name)
{
    return (Argument[0] == L'/' || Argument[0] == L'-') && _wcsicmp(Argument + 1, Name) == 0;
}

static VOID
PrintError(
    _In_ PCWSTR Prefix,
    _In_ DWORD Error)
{
    WCHAR Message[512];
    DWORD Length;

    Length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            Error,
                            0,
                            Message,
                            _countof(Message),
                            NULL);
    while (Length > 0 && (Message[Length - 1] == L'\r' || Message[Length - 1] == L'\n' || Message[Length - 1] == L' '))
        Message[--Length] = UNICODE_NULL;

    if (Length > 0)
        ConPrintf(StdOut, L"%ls: %ls\n", Prefix, Message);
    else
        ConPrintf(StdOut, L"%ls: error 0x%08lx\n", Prefix, Error);
}

static BOOL
RebootSystem(VOID)
{
    TOKEN_PRIVILEGES Privileges;
    HANDLE hToken;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return FALSE;

    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &Privileges.Privileges[0].Luid))
        AdjustTokenPrivileges(hToken, FALSE, &Privileges, 0, NULL, NULL);
    CloseHandle(hToken);

    return ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_HARDWARE | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED);
}

static INT
FinishOperation(
    _In_ DWORD Error,
    _In_ BOOL NeedReboot,
    _In_ BOOL Reboot)
{
    if (Error != ERROR_SUCCESS)
        return (INT)Error;

    if (!NeedReboot)
        return ERROR_SUCCESS;

    if (Reboot)
    {
        ConPuts(StdOut, L"System reboot is required to complete the operation. Rebooting.\n");
        if (RebootSystem())
            return ERROR_SUCCESS_REBOOT_INITIATED;
    }
    else
    {
        ConPuts(StdOut, L"System reboot is needed to complete the operation!\n");
    }

    return ERROR_SUCCESS_REBOOT_REQUIRED;
}

static BOOL
GetDriverKeyString(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ PCWSTR ValueName,
    _Out_writes_(BufferSize) PWSTR Buffer,
    _In_ DWORD BufferSize)
{
    DWORD Size = (BufferSize - 1) * sizeof(WCHAR);
    DWORD Type;
    HKEY hKey;
    LONG Result;

    Buffer[0] = UNICODE_NULL;
    hKey = SetupDiOpenDevRegKey(DeviceInfoSet, DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    if (hKey == INVALID_HANDLE_VALUE)
        return FALSE;

    Result = RegQueryValueExW(hKey, ValueName, NULL, &Type, (LPBYTE)Buffer, &Size);
    RegCloseKey(hKey);
    if (Result != ERROR_SUCCESS || Type != REG_SZ)
    {
        Buffer[0] = UNICODE_NULL;
        return FALSE;
    }

    Buffer[Size / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

static VOID
PrintHeader(VOID)
{
    ConPuts(StdOut, L"LiberNT PnP Utility\n\n");
}

static BOOL
IsUsingInf(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ PCWSTR PublishedName)
{
    WCHAR InfPath[MAX_PATH];

    return GetDriverKeyString(DeviceInfoSet, DeviceInfoData, REGSTR_VAL_INFPATH, InfPath, _countof(InfPath)) &&
           _wcsicmp(InfPath, PublishedName) == 0;
}

static PWSTR
GetDevicesUsingInf(
    _In_ PCWSTR PublishedName)
{
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    PWSTR List, NewList;
    SIZE_T Used = 0, Size = 1024;
    DWORD Index;

    List = malloc(Size * sizeof(WCHAR));
    if (!List)
        return NULL;
    List[0] = UNICODE_NULL;

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return List;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        WCHAR InstanceId[MAX_DEVICE_ID_LEN + 1];
        SIZE_T Length;

        if (!IsUsingInf(DeviceInfoSet, &DeviceInfoData, PublishedName) ||
            !SetupDiGetDeviceInstanceIdW(DeviceInfoSet, &DeviceInfoData, InstanceId, _countof(InstanceId), NULL))
        {
            continue;
        }

        Length = wcslen(InstanceId) + 1;
        if (Used + Length + 1 > Size)
        {
            Size = (Used + Length + 1) * 2;
            NewList = realloc(List, Size * sizeof(WCHAR));
            if (!NewList)
                break;
            List = NewList;
        }

        wcscpy(List + Used, InstanceId);
        Used += Length;
        List[Used] = UNICODE_NULL;
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    return List;
}

static BOOL
ListContains(
    _In_opt_ PCWSTR List,
    _In_ PCWSTR InstanceId)
{
    if (!List)
        return FALSE;

    for (; *List; List += wcslen(List) + 1)
    {
        if (_wcsicmp(List, InstanceId) == 0)
            return TRUE;
    }
    return FALSE;
}

static VOID
RemoveDevicesUsingInf(
    _In_ PCWSTR PublishedName)
{
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    DWORD Index;

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        if (IsUsingInf(DeviceInfoSet, &DeviceInfoData, PublishedName))
            SetupDiCallClassInstaller(DIF_REMOVE, DeviceInfoSet, &DeviceInfoData);
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
}

static VOID
ScanDevices(VOID)
{
    DEVINST Root;

    if (CM_Locate_DevNodeW(&Root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        CM_Reenumerate_DevNode(Root, CM_REENUMERATE_SYNCHRONOUS);
}

static BOOL
IsSystemInf(
    _In_ PCWSTR InfPath)
{
    WCHAR Directory[MAX_PATH];
    SIZE_T Length;
    UINT Result;

    Result = GetSystemWindowsDirectoryW(Directory, _countof(Directory));
    if (Result == 0 || Result + 6 >= _countof(Directory))
        return FALSE;
    wcscat(Directory, L"\\inf\\");

    Length = wcslen(Directory);
    return _wcsnicmp(InfPath, Directory, Length) == 0 && !wcschr(InfPath + Length, L'\\');
}

/* The DrvInst worker installs devices through the job service when the
 * policy says so; then an /add-driver /install drives those jobs instead of
 * installing directly. */
static BOOL
DrvInstWorkerEnabled(VOID)
{
    HKEY hKey;
    DWORD Value = 0, Size = sizeof(Value), Type;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DrvInst",
                      0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return FALSE;
    if (RegQueryValueExW(hKey, L"UseWorker", NULL, &Type, (PBYTE)&Value, &Size) != ERROR_SUCCESS ||
        Type != REG_DWORD)
        Value = 0;
    RegCloseKey(hKey);
    return Value != 0;
}

static BOOL
SubmitInfThroughJobs(
    _In_ PCWSTR PublishedName,
    _Inout_ PADD_CONTEXT Context)
{
    HDEVINFO DeviceInfoSet;
    SP_DEVINFO_DATA DeviceInfoData;
    SP_DRVINFO_DATA_W DriverInfo;
    DWORD MemberIndex;
    DWORD Installed = 0, Current = 0, Failed = 0;
    WCHAR DeviceId[MAX_DEVICE_ID_LEN];
    PCWSTR Component;

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        Context->Error = GetLastError();
        PrintError(L"Failed to enumerate devices", Context->Error);
        return FALSE;
    }

    Component = wcsrchr(PublishedName, L'\\');
    Component = Component ? Component + 1 : PublishedName;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (MemberIndex = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, MemberIndex, &DeviceInfoData); MemberIndex++)
    {
        BOOL Serves = FALSE;
        WCHAR MatchedInf[MAX_PATH];

        MatchedInf[0] = UNICODE_NULL;
        if (!SetupDiBuildDriverInfoList(DeviceInfoSet, &DeviceInfoData, SPDIT_COMPATDRIVER))
            continue;
        DriverInfo.cbSize = sizeof(DriverInfo);
        for (DWORD DriverIndex = 0;
             SetupDiEnumDriverInfoW(DeviceInfoSet, &DeviceInfoData, SPDIT_COMPATDRIVER, DriverIndex, &DriverInfo);
             DriverIndex++)
        {
            SP_DRVINFO_DETAIL_DATA_W Detail;

            Detail.cbSize = sizeof(Detail);
            if (SetupDiGetDriverInfoDetailW(DeviceInfoSet, &DeviceInfoData,
                                            &DriverInfo, &Detail, sizeof(Detail), NULL) &&
                Detail.InfFileName[0])
            {
                PCWSTR Name = wcsrchr(Detail.InfFileName, L'\\');
                Name = Name ? Name + 1 : Detail.InfFileName;
                if (!lstrcmpiW(Name, Component))
                {
                    Serves = TRUE;
                    lstrcpynW(MatchedInf, Detail.InfFileName, RTL_NUMBER_OF(MatchedInf));
                    break;
                }
            }
        }
        SetupDiDestroyDriverInfoList(DeviceInfoSet, &DeviceInfoData, SPDIT_COMPATDRIVER);

        if (!Serves)
            continue;
        if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, &DeviceInfoData, DeviceId, RTL_NUMBER_OF(DeviceId), NULL))
            continue;

        {
            DRVINST_JOB_ID JobId = 0;
            DRVINST_JOB_INFO Info;
            PCWSTR FullInfPath = MatchedInf[0] ? MatchedInf : PublishedName;
            DWORD Error = DrvInstSubmitInf(DeviceId, FullInfPath, 0, &JobId);
            if (Error == ERROR_SUCCESS)
                Error = DrvInstWait(JobId, 600000, &Info);
            if (Error == ERROR_SUCCESS && Info.State == DrvInstJobSucceeded && Info.Result.Win32 == ERROR_SUCCESS)
            {
                ConPrintf(StdOut, L"Driver package installed on device: %ls\n", DeviceId);
                Installed++;
            }
            else if (Error == ERROR_SUCCESS && Info.State == DrvInstJobSucceeded)
            {
                ConPrintf(StdOut, L"Driver package is up-to-date on device: %ls\n", DeviceId);
                Current++;
            }
            else
            {
                DWORD FailCode = Error;

                if (FailCode == ERROR_SUCCESS)
                    FailCode = Info.Result.Win32;
                if (FailCode == ERROR_SUCCESS && Info.State == DrvInstJobTimedOut)
                    FailCode = ERROR_TIMEOUT;
                if (FailCode == ERROR_SUCCESS)
                    FailCode = ERROR_GEN_FAILURE;
                ConPrintf(StdOut, L"Failed to install driver package on device: %ls (error %lu)\n",
                          DeviceId, FailCode);
                if (Context->Error == ERROR_SUCCESS)
                    Context->Error = FailCode;
                Failed++;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    if (Installed == 0 && Current != 0 && Failed == 0)
    {
        Context->Error = ERROR_NO_MORE_ITEMS;
        return FALSE;
    }
    return Installed > 0;
}

static VOID
InstallDriverPackage(
    _In_ PCWSTR InfPath,
    _In_ PCWSTR PublishedName,
    _In_ BOOL SystemInf,
    _Inout_ PADD_CONTEXT Context,
    _Out_ PBOOL Succeeded)
{
    PWSTR Before, After, Device;
    BOOL NeedReboot = FALSE;
    DWORD Installed = 0, Current = 0;

    *Succeeded = TRUE;
    if (DrvInstWorkerEnabled())
    {
        *Succeeded = SubmitInfThroughJobs(PublishedName, Context);
        return;
    }

    Before = GetDevicesUsingInf(PublishedName);

    if (!DiInstallDriverW(NULL, InfPath, SystemInf ? DIIRFLAG_INF_ALREADY_COPIED : 0, &NeedReboot))
    {
        Context->Error = GetLastError();
        PrintError(L"Failed to install driver package", Context->Error);
        *Succeeded = FALSE;
        free(Before);
        return;
    }

    After = GetDevicesUsingInf(PublishedName);
    for (Device = After; Device && *Device; Device += wcslen(Device) + 1)
    {
        if (ListContains(Before, Device))
        {
            ConPrintf(StdOut, L"Driver package is up-to-date on device: %ls\n", Device);
            Current++;
        }
        else
        {
            ConPrintf(StdOut, L"Driver package installed on device: %ls\n", Device);
            Installed++;
        }
    }

    if (Installed == 0 && Current != 0)
    {
        Context->Error = ERROR_NO_MORE_ITEMS;
        *Succeeded = FALSE;
    }

    if (NeedReboot)
        Context->NeedReboot = TRUE;

    free(Before);
    free(After);
}

static VOID
AddDriverPackage(
    _In_ PCWSTR InfPath,
    _In_ PCWSTR DisplayName,
    _Inout_ PADD_CONTEXT Context)
{
    WCHAR Published[MAX_PATH];
    PCWSTR Component;
    BOOL SystemInf = IsSystemInf(InfPath);
    BOOL Existed = FALSE;
    BOOL Succeeded = TRUE;

    Context->Total++;
    ConPrintf(StdOut, L"Adding driver package:  %ls\n", DisplayName);

    if (SystemInf)
    {
        wcscpy(Published, InfPath);
    }
    else if (!SetupCopyOEMInfW(InfPath, NULL, SPOST_NONE, SP_COPY_NOOVERWRITE, Published, _countof(Published), NULL, NULL))
    {
        if (GetLastError() != ERROR_FILE_EXISTS)
        {
            Context->Error = GetLastError();
            PrintError(L"Failed to add driver package", Context->Error);
            ConPuts(StdOut, L"\n");
            return;
        }
        Existed = TRUE;
    }

    Component = wcsrchr(Published, L'\\');
    Component = Component ? Component + 1 : Published;

    if (!SystemInf)
    {
        ConPrintf(StdOut, L"Driver package added successfully.%ls\n",
                  Existed ? L" (Already exists in the system)" : L"");
    }
    ConPrintf(StdOut, L"Published Name:         %ls\n", Component);

    if (Context->Install)
        InstallDriverPackage(InfPath, Component, SystemInf, Context, &Succeeded);

    if (Succeeded)
        Context->Added++;

    ConPuts(StdOut, L"\n");
}

static VOID
AddDriverPackages(
    _In_ PCWSTR Specification,
    _In_ SIZE_T BaseLength,
    _Inout_ PADD_CONTEXT Context)
{
    WCHAR Pattern[MAX_PATH], Directory[MAX_PATH], Path[MAX_PATH], Name[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    PWSTR FilePart = NULL;
    HANDLE hFind;
    DWORD Length;

    Length = GetFullPathNameW(Specification, _countof(Pattern), Pattern, &FilePart);
    if (Length == 0 || Length >= _countof(Pattern) || !FilePart)
        return;

    wcscpy(Name, FilePart);
    wcsncpy(Directory, Pattern, FilePart - Pattern);
    Directory[FilePart - Pattern] = UNICODE_NULL;

    if (BaseLength == 0)
        BaseLength = wcslen(Directory);

    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (wcslen(Directory) + wcslen(FindData.cFileName) >= _countof(Path))
                continue;

            wcscpy(Path, Directory);
            wcscat(Path, FindData.cFileName);
            AddDriverPackage(Path, Path + BaseLength, Context);
        } while (FindNextFileW(hFind, &FindData));
        FindClose(hFind);
    }

    if (!Context->SubDirs || wcslen(Directory) + 1 >= _countof(Path))
        return;

    wcscpy(Path, Directory);
    wcscat(Path, L"*");
    hFind = FindFirstFileW(Path, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (!(FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !wcscmp(FindData.cFileName, L".") ||
            !wcscmp(FindData.cFileName, L".."))
        {
            continue;
        }
        if (wcslen(Directory) + wcslen(FindData.cFileName) + 1 + wcslen(Name) >= _countof(Path))
            continue;

        wcscpy(Path, Directory);
        wcscat(Path, FindData.cFileName);
        wcscat(Path, L"\\");
        wcscat(Path, Name);
        AddDriverPackages(Path, BaseLength, Context);
    } while (FindNextFileW(hFind, &FindData));
    FindClose(hFind);
}

static INT
CommandAddDriver(
    _In_ INT argc,
    _In_ WCHAR **argv,
    _In_ BOOL LegacyInstall)
{
    ADD_CONTEXT Context = {0};
    PCWSTR Specification = NULL;
    BOOL Reboot = FALSE;
    INT i;

    Context.Install = LegacyInstall;
    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"subdirs"))
            Context.SubDirs = TRUE;
        else if (IsOption(argv[i], L"install") || IsOption(argv[i], L"i"))
            Context.Install = TRUE;
        else if (IsOption(argv[i], L"reboot"))
            Reboot = TRUE;
        else if (!Specification)
            Specification = argv[i];
        else
            return ERROR_INVALID_PARAMETER;
    }

    if (!Specification)
        return ERROR_INVALID_PARAMETER;

    PrintHeader();
    AddDriverPackages(Specification, 0, &Context);

    if (Context.Total == 0)
    {
        Context.Error = ERROR_FILE_NOT_FOUND;
        ConPuts(StdOut, L"Failed to add driver package: Missing or invalid driver package specified.\n\n");
    }

    ConPrintf(StdOut, L"Total driver packages:  %lu\n", Context.Total);
    ConPrintf(StdOut, L"Added driver packages:  %lu\n", Context.Added);

    return FinishOperation(Context.Error, Context.NeedReboot, Reboot);
}

static INT
CommandDeleteDriver(
    _In_ INT argc,
    _In_ WCHAR **argv,
    _In_ BOOL LegacyForce)
{
    PCWSTR Name = NULL;
    BOOL Uninstall = FALSE;
    BOOL Force = LegacyForce;
    BOOL Reboot = FALSE;
    DWORD Error = ERROR_SUCCESS;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"uninstall"))
            Uninstall = TRUE;
        else if (IsOption(argv[i], L"force") || IsOption(argv[i], L"f"))
            Force = TRUE;
        else if (IsOption(argv[i], L"reboot"))
            Reboot = TRUE;
        else if (!Name)
            Name = argv[i];
        else
            return ERROR_INVALID_PARAMETER;
    }

    if (!Name)
        return ERROR_INVALID_PARAMETER;

    PrintHeader();

    if (Uninstall && Force)
    {
        ConPuts(StdOut, L"Ignoring /force when used with /uninstall to delete driver package.\n");
        Force = FALSE;
    }

    if (Uninstall)
    {
        RemoveDevicesUsingInf(Name);
        ConPuts(StdOut, L"Driver package uninstalled.\n");
    }

    if (SetupUninstallOEMInfW(Name, Force ? SUOI_FORCEDELETE : 0, NULL))
    {
        ConPuts(StdOut, L"Driver package deleted successfully.\n");
    }
    else
    {
        Error = GetLastError();
        if (Error == ERROR_FILE_NOT_FOUND)
            Error = ERROR_NOT_AN_INSTALLED_OEM_INF;

        if (Error == ERROR_INF_IN_USE_BY_DEVICES)
            ConPuts(StdOut, L"Failed to delete driver package: One or more devices are presently installed using the specified INF.\n");
        else if (Error == ERROR_NOT_AN_INSTALLED_OEM_INF)
            ConPuts(StdOut, L"Failed to delete driver package: The specified file is not an installed OEM INF.\n");
        else
            PrintError(L"Failed to delete driver package", Error);
    }

    if (Uninstall)
        ScanDevices();

    return FinishOperation(Error, FALSE, Reboot);
}

static VOID
GetInfVersionValue(
    _In_ HINF hInf,
    _In_ PCWSTR Key,
    _Out_writes_(BufferSize) PWSTR Buffer,
    _In_ DWORD BufferSize)
{
    if (!SetupGetLineTextW(NULL, hInf, L"Version", Key, Buffer, BufferSize, NULL))
        Buffer[0] = UNICODE_NULL;
}

static VOID
PrintDeviceProperty(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Property,
    _In_ PCWSTR Label,
    _In_opt_ PCWSTR Default)
{
    WCHAR Buffer[1024];
    DWORD Type;
    PCWSTR Value;

    if (!SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet,
                                           DeviceInfoData,
                                           Property,
                                           &Type,
                                           (PBYTE)Buffer,
                                           sizeof(Buffer) - 2 * sizeof(WCHAR),
                                           NULL))
    {
        if (Default)
            ConPrintf(StdOut, L"%-28ls%ls\n", Label, Default);
        return;
    }

    if (Type == REG_SZ)
    {
        if (Property == SPDRP_CLASSGUID)
        {
            CharLowerW(Buffer);
            if (Default && wcscmp(Buffer, L"{4d36e97e-e325-11ce-bfc1-08002be10318}") == 0)
                wcscpy(Buffer, Default);
        }
        ConPrintf(StdOut, L"%-28ls%ls\n", Label, Buffer);
    }
    else if (Type == REG_MULTI_SZ)
    {
        for (Value = Buffer; *Value; Value += wcslen(Value) + 1)
        {
            ConPrintf(StdOut, L"%-28ls%ls\n", Label, Value);
            Label = L"";
        }
    }
}

static BOOL
MatchesClass(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ PCWSTR Class)
{
    WCHAR Buffer[MAX_PATH];

    if (SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, DeviceInfoData, SPDRP_CLASS, NULL,
                                          (PBYTE)Buffer, sizeof(Buffer), NULL) &&
        _wcsicmp(Buffer, Class) == 0)
    {
        return TRUE;
    }

    return SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, DeviceInfoData, SPDRP_CLASSGUID, NULL,
                                             (PBYTE)Buffer, sizeof(Buffer), NULL) &&
           _wcsicmp(Buffer, Class) == 0;
}

typedef enum _OUTPUT_FORMAT
{
    OutputText = 0,
    OutputXml,
    OutputCsv
} OUTPUT_FORMAT;

typedef struct _OUTPUT_CONTEXT
{
    OUTPUT_FORMAT Format;
    HANDLE File;        /* INVALID_HANDLE_VALUE => standard output */
} OUTPUT_CONTEXT;

static VOID OutputPutsRaw(_In_ OUTPUT_CONTEXT *Output, _In_ PCWSTR Text);

static VOID
GetOutputVersion(
    _Out_writes_(Cch) PWSTR Buffer,
    _In_ DWORD Cch)
{
    OSVERSIONINFOW Info = { sizeof(Info) };

    if (GetVersionExW(&Info))
        _snwprintf(Buffer, Cch, L"%lu.%lu.%lu", Info.dwMajorVersion, Info.dwMinorVersion, Info.dwBuildNumber);
    else
        lstrcpynW(Buffer, L"0.0.0", Cch);
    Buffer[Cch - 1] = UNICODE_NULL;
}

/* Default /output-file name: pnputil_YYYYMMDD_HHMMSSmmm.<ext> in the current
   directory, from the UTC time (GetSystemTime), as Windows does. */
static VOID
BuildDefaultOutputName(
    _In_ OUTPUT_FORMAT Format,
    _Out_writes_(Cch) PWSTR Buffer,
    _In_ DWORD Cch)
{
    SYSTEMTIME Time;
    PCWSTR Extension = (Format == OutputXml) ? L"xml" : (Format == OutputCsv) ? L"csv" : L"txt";

    GetSystemTime(&Time);
    _snwprintf(Buffer, Cch, L"pnputil_%04u%02u%02u_%02u%02u%02u%03u.%ls",
               Time.wYear, Time.wMonth, Time.wDay,
               Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds, Extension);
    Buffer[Cch - 1] = UNICODE_NULL;
}

/* One "Label<pad>Value" text line to the sink (CRLF; the console path keeps "\n"). */
static VOID
OutputTxtLine(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Label,
    _In_ PCWSTR Value)
{
    WCHAR Line[2048];

    _snwprintf(Line, _countof(Line) - 1, L"%-28ls%ls\r\n", Label, Value);
    Line[_countof(Line) - 1] = UNICODE_NULL;
    OutputPutsRaw(Output, Line);
}

/* Emit a REG_MULTI_SZ property as one text line per value (label blank after the
   first), matching the console PrintDeviceProperty; absent property emits nothing. */
static VOID
OutputTxtMultiSz(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Property,
    _In_ PCWSTR Label)
{
    WCHAR Buffer[1024];
    DWORD Type;
    PCWSTR Value;

    if (!SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, DeviceInfoData, Property, &Type,
                                           (PBYTE)Buffer, sizeof(Buffer) - 2 * sizeof(WCHAR), NULL) ||
        Type != REG_MULTI_SZ)
    {
        return;
    }
    Buffer[_countof(Buffer) - 1] = UNICODE_NULL;
    Buffer[_countof(Buffer) - 2] = UNICODE_NULL;
    for (Value = Buffer; *Value; Value += wcslen(Value) + 1)
    {
        OutputTxtLine(Output, Label, Value);
        Label = L"";
    }
}

/* Write one string to the active sink. For /output-file this uses the encoding
   Windows writes for the format -- ANSI for txt, UTF-16LE for csv, UTF-8 for xml,
   all without a BOM -- converting the whole string at once; otherwise stdout. */
static VOID
OutputPutsRaw(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Text)
{
    INT Bytes, Length;
    PSTR Buffer;
    DWORD Written;
    UINT CodePage;

    if (Output->File == INVALID_HANDLE_VALUE)
    {
        ConPuts(StdOut, Text);
        return;
    }

    if (Output->Format == OutputCsv)
    {
        Length = lstrlenW(Text);
        if (Length > 0)
            WriteFile(Output->File, Text, Length * sizeof(WCHAR), &Written, NULL);
        return;
    }

    CodePage = (Output->Format == OutputText) ? CP_ACP : CP_UTF8;
    Bytes = WideCharToMultiByte(CodePage, 0, Text, -1, NULL, 0, NULL, NULL);
    if (Bytes <= 1)
        return;
    Buffer = HeapAlloc(GetProcessHeap(), 0, Bytes);
    if (!Buffer)
        return;
    WideCharToMultiByte(CodePage, 0, Text, -1, Buffer, Bytes, NULL, NULL);
    WriteFile(Output->File, Buffer, Bytes - 1, &Written, NULL);
    HeapFree(GetProcessHeap(), 0, Buffer);
}

/* XML element/attribute text escaping, built once. Control characters below 0x20
   (except tab, CR, LF) are dropped: they are not legal in XML 1.0. */
static VOID
OutputXmlText(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Text)
{
    PCWSTR p, Rep;
    PWSTR Buffer;
    SIZE_T Used = 0;

    Buffer = HeapAlloc(GetProcessHeap(), 0, (wcslen(Text) * 6 + 1) * sizeof(WCHAR));
    if (!Buffer)
        return;

    for (p = Text; *p; p++)
    {
        switch (*p)
        {
            case L'&': Rep = L"&amp;"; break;
            case L'<': Rep = L"&lt;"; break;
            case L'>': Rep = L"&gt;"; break;
            case L'"': Rep = L"&quot;"; break;
            case L'\'': Rep = L"&apos;"; break;
            default:
                if (*p < 0x20 && *p != L'\t' && *p != L'\n' && *p != L'\r')
                    continue;
                Buffer[Used++] = *p;
                continue;
        }
        while (*Rep)
            Buffer[Used++] = *Rep++;
    }
    Buffer[Used] = UNICODE_NULL;
    OutputPutsRaw(Output, Buffer);
    HeapFree(GetProcessHeap(), 0, Buffer);
}

/* One RFC-4180 CSV field, built once: always quoted, embedded quotes doubled. */
static VOID
OutputCsvField(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Text,
    _In_ BOOL Last)
{
    PCWSTR p;
    PWSTR Buffer;
    SIZE_T Used = 0;

    /* Worst case: open-quote + 2n (every char a doubled quote) + close-quote + CRLF + NUL. */
    Buffer = HeapAlloc(GetProcessHeap(), 0, (wcslen(Text) * 2 + 5) * sizeof(WCHAR));
    if (!Buffer)
        return;

    Buffer[Used++] = L'"';
    for (p = Text; *p; p++)
    {
        Buffer[Used++] = *p;
        if (*p == L'"')
            Buffer[Used++] = L'"';
    }
    Buffer[Used++] = L'"';
    if (Last)
    {
        Buffer[Used++] = L'\r';
        Buffer[Used++] = L'\n';
    }
    else
    {
        Buffer[Used++] = L',';
    }
    Buffer[Used] = UNICODE_NULL;
    OutputPutsRaw(Output, Buffer);
    HeapFree(GetProcessHeap(), 0, Buffer);
}

/* Read a device registry property as a string; returns Default (or empty) when
   absent. For the class GUID, lower-case it and map the default class as the txt
   path does. */
static PCWSTR
GetDevicePropertyString(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Property,
    _Out_writes_(Cch) PWSTR Buffer,
    _In_ DWORD Cch,
    _In_opt_ PCWSTR Default)
{
    DWORD Type;

    if (Cch == 0)
        return L"";
    Buffer[0] = UNICODE_NULL;
    if (!SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, DeviceInfoData, Property, &Type,
                                           (PBYTE)Buffer, Cch * sizeof(WCHAR) - sizeof(WCHAR), NULL) ||
        Type != REG_SZ)
    {
        return Default ? Default : L"";
    }
    Buffer[Cch - 1] = UNICODE_NULL;
    if (Property == SPDRP_CLASSGUID)
    {
        CharLowerW(Buffer);
        if (Default && wcscmp(Buffer, L"{4d36e97e-e325-11ce-bfc1-08002be10318}") == 0)
            return Default;
    }
    return Buffer;
}

/* Read a REG_MULTI_SZ device property into Buffer as a double-null-terminated
   list; on absence or wrong type Buffer is an empty list. */
static VOID
ReadSpdrpMultiSz(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Property,
    _Out_writes_(Cch) PWSTR Buffer,
    _In_ DWORD Cch)
{
    DWORD Type;

    if (Cch < 2)
    {
        if (Cch)
            Buffer[0] = UNICODE_NULL;
        return;
    }
    ZeroMemory(Buffer, Cch * sizeof(WCHAR));
    if (!SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, DeviceInfoData, Property, &Type,
                                           (PBYTE)Buffer, Cch * sizeof(WCHAR) - 2 * sizeof(WCHAR), NULL) ||
        Type != REG_MULTI_SZ)
    {
        Buffer[0] = UNICODE_NULL;
        Buffer[1] = UNICODE_NULL;
    }
}

/* Read a DEVPROP_TYPE_STRING_LIST device property into Buffer as a double-null
   list (same shape as REG_MULTI_SZ); empty list on absence or wrong type. */
static VOID
ReadDevpropStringList(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ const DEVPROPKEY *Key,
    _Out_writes_(Cch) PWSTR Buffer,
    _In_ DWORD Cch)
{
    DEVPROPTYPE PropType = 0;

    if (Cch < 2)
    {
        if (Cch)
            Buffer[0] = UNICODE_NULL;
        return;
    }
    ZeroMemory(Buffer, Cch * sizeof(WCHAR));
    if (!SetupDiGetDevicePropertyW(DeviceInfoSet, DeviceInfoData, Key, &PropType,
                                   (PBYTE)Buffer, Cch * sizeof(WCHAR) - 2 * sizeof(WCHAR), NULL, 0) ||
        PropType != DEVPROP_TYPE_STRING_LIST)
    {
        Buffer[0] = UNICODE_NULL;
        Buffer[1] = UNICODE_NULL;
    }
}

/* Reduce one list entry to the text after its last backslash when Leaf is set;
   DEVPKEY_Device_Stack holds driver object names (\Driver\partmgr) that Windows 11
   pnputil prints as the bare leaf (partmgr). */
static PCWSTR
ListEntryText(
    _In_ PCWSTR Entry,
    _In_ BOOL Leaf)
{
    PCWSTR Slash;

    if (!Leaf)
        return Entry;
    Slash = wcsrchr(Entry, L'\\');
    return (Slash && Slash[1]) ? Slash + 1 : Entry;
}

/* Join a double-null list into Dest with ';' separators (the in-cell separator
   Windows 11 pnputil uses for multi-value csv fields). */
static VOID
JoinMultiSz(
    _In_ PCWSTR MultiSz,
    _Out_writes_(Cch) PWSTR Dest,
    _In_ DWORD Cch,
    _In_ BOOL Leaf)
{
    PCWSTR Value;
    SIZE_T Used = 0;

    if (Cch == 0)
        return;
    Dest[0] = UNICODE_NULL;
    for (Value = MultiSz; *Value; Value += wcslen(Value) + 1)
    {
        PCWSTR Emit = ListEntryText(Value, Leaf);

        if (Used && Used + 1 < Cch)
            Dest[Used++] = L';';
        while (*Emit && Used + 1 < Cch)
            Dest[Used++] = *Emit++;
        Dest[Used] = UNICODE_NULL;
        if (Used + 1 >= Cch)
            break;
    }
}

/* Parent instance id via the configuration manager; empty when the node has no
   reachable parent. */
static VOID
GetParentInstanceId(
    _In_ DEVINST DevInst,
    _Out_writes_(Cch) PWSTR Buffer,
    _In_ DWORD Cch)
{
    DEVINST Parent = 0;

    if (Cch == 0)
        return;
    Buffer[0] = UNICODE_NULL;
    if (CM_Get_Parent(&Parent, DevInst, 0) != CR_SUCCESS)
        return;
    if (CM_Get_Device_IDW(Parent, Buffer, Cch, 0) != CR_SUCCESS)
        Buffer[0] = UNICODE_NULL;
}

/* Child instance ids (first child then its siblings) as a double-null list, so
   csv can ';'-join them and xml can emit one <Child> per entry. */
static VOID
GetChildrenMultiSz(
    _In_ DEVINST DevInst,
    _Out_writes_(Cch) PWSTR Buffer,
    _In_ DWORD Cch)
{
    DEVINST Child = 0;
    SIZE_T Used = 0;
    WCHAR Id[MAX_DEVICE_ID_LEN + 1];

    if (Cch < 2)
    {
        if (Cch)
            Buffer[0] = UNICODE_NULL;
        return;
    }
    Buffer[0] = UNICODE_NULL;
    Buffer[1] = UNICODE_NULL;
    if (CM_Get_Child(&Child, DevInst, 0) != CR_SUCCESS)
        return;
    do
    {
        PCWSTR p;
        SIZE_T Len;

        if (CM_Get_Device_IDW(Child, Id, _countof(Id), 0) != CR_SUCCESS)
            continue;
        Len = wcslen(Id);
        if (Used + Len + 2 > Cch)
            break;
        for (p = Id; *p; p++)
            Buffer[Used++] = *p;
        Buffer[Used++] = UNICODE_NULL;
        Buffer[Used] = UNICODE_NULL;
    } while (CM_Get_Sibling(&Child, Child, 0) == CR_SUCCESS);
}

/* Emit a double-null list as a nested XML block: <Outer> with one <Inner> child
   per entry, 8/12-space indent. Nothing is emitted when the list is empty. */
static VOID
EmitXmlList(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR OuterTag,
    _In_ PCWSTR InnerTag,
    _In_ PCWSTR MultiSz,
    _In_ BOOL Leaf)
{
    PCWSTR Value;

    if (!*MultiSz)
        return;
    OutputPutsRaw(Output, L"        <");
    OutputPutsRaw(Output, OuterTag);
    OutputPutsRaw(Output, L">\r\n");
    for (Value = MultiSz; *Value; Value += wcslen(Value) + 1)
    {
        OutputPutsRaw(Output, L"            <");
        OutputPutsRaw(Output, InnerTag);
        OutputPutsRaw(Output, L">");
        OutputXmlText(Output, ListEntryText(Value, Leaf));
        OutputPutsRaw(Output, L"</");
        OutputPutsRaw(Output, InnerTag);
        OutputPutsRaw(Output, L">\r\n");
    }
    OutputPutsRaw(Output, L"        </");
    OutputPutsRaw(Output, OuterTag);
    OutputPutsRaw(Output, L">\r\n");
}

/* Emit one XML child element <Tag>text</Tag> at 8-space indent, only when text
   is non-empty. */
static VOID
EmitXmlElement(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Tag,
    _In_ PCWSTR Text)
{
    if (!*Text)
        return;
    OutputPutsRaw(Output, L"        <");
    OutputPutsRaw(Output, Tag);
    OutputPutsRaw(Output, L">");
    OutputXmlText(Output, Text);
    OutputPutsRaw(Output, L"</");
    OutputPutsRaw(Output, Tag);
    OutputPutsRaw(Output, L">\r\n");
}

/* Emit one device as a CSV row or an XML <Device> element (txt uses its own path).
   Column/element order matches Windows 11 pnputil exactly. */
static VOID
EmitDeviceRecord(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR InstanceId,
    _In_ PCWSTR Description,
    _In_ PCWSTR ClassName,
    _In_ PCWSTR ClassGuid,
    _In_ PCWSTR Manufacturer,
    _In_ PCWSTR Status,
    _In_ PCWSTR ProblemCode,
    _In_ PCWSTR DriverName,
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ const ENUM_FILTER *Filter)
{
    BOOL AnyView = Filter->Ids || Filter->Relations || Filter->Stack || Filter->Location;

    if (Output->Format == OutputCsv)
    {
        WCHAR List[2048], Cell[2048], Cell2[2048];
        BOOL LastIds = Filter->Ids && !Filter->Relations && !Filter->Stack && !Filter->Location;
        BOOL LastRel = Filter->Relations && !Filter->Stack && !Filter->Location;
        BOOL LastStk = Filter->Stack && !Filter->Location;

        OutputCsvField(Output, InstanceId, FALSE);
        OutputCsvField(Output, Description, FALSE);
        OutputCsvField(Output, ClassName, FALSE);
        OutputCsvField(Output, ClassGuid, FALSE);
        OutputCsvField(Output, Manufacturer, FALSE);
        OutputCsvField(Output, Status, FALSE);
        OutputCsvField(Output, ProblemCode, FALSE);
        OutputCsvField(Output, L"", FALSE);          /* ProblemStatus */
        OutputCsvField(Output, DriverName, FALSE);
        OutputCsvField(Output, L"", !AnyView);       /* ExtensionDriverNames */

        if (Filter->Ids)
        {
            ReadSpdrpMultiSz(DeviceInfoSet, DeviceInfoData, SPDRP_HARDWAREID, List, _countof(List));
            JoinMultiSz(List, Cell, _countof(Cell), FALSE);
            OutputCsvField(Output, Cell, FALSE);
            ReadSpdrpMultiSz(DeviceInfoSet, DeviceInfoData, SPDRP_COMPATIBLEIDS, List, _countof(List));
            JoinMultiSz(List, Cell2, _countof(Cell2), FALSE);
            OutputCsvField(Output, Cell2, LastIds);
        }
        if (Filter->Relations)
        {
            GetParentInstanceId(DeviceInfoData->DevInst, Cell, _countof(Cell));
            OutputCsvField(Output, Cell, FALSE);
            GetChildrenMultiSz(DeviceInfoData->DevInst, List, _countof(List));
            JoinMultiSz(List, Cell2, _countof(Cell2), FALSE);
            OutputCsvField(Output, Cell2, LastRel);
        }
        if (Filter->Stack)
        {
            ReadDevpropStringList(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_Stack, List, _countof(List));
            JoinMultiSz(List, Cell, _countof(Cell), TRUE);
            OutputCsvField(Output, Cell, LastStk);
        }
        if (Filter->Location)
        {
            OutputCsvField(Output,
                           GetDevicePropertyString(DeviceInfoSet, DeviceInfoData, SPDRP_LOCATION_INFORMATION, Cell, _countof(Cell), NULL),
                           FALSE);
            ReadDevpropStringList(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_LocationPaths, List, _countof(List));
            JoinMultiSz(List, Cell2, _countof(Cell2), FALSE);
            OutputCsvField(Output, Cell2, TRUE);
        }
    }
    else /* OutputXml */
    {
        WCHAR List[2048], Elem[2048];

        OutputPutsRaw(Output, L"    <Device InstanceId=\"");
        OutputXmlText(Output, InstanceId);
        OutputPutsRaw(Output, L"\">\r\n");
        if (Description[0])
        {
            OutputPutsRaw(Output, L"        <DeviceDescription>");
            OutputXmlText(Output, Description);
            OutputPutsRaw(Output, L"</DeviceDescription>\r\n");
        }
        if (ClassName[0])
        {
            OutputPutsRaw(Output, L"        <ClassName>");
            OutputXmlText(Output, ClassName);
            OutputPutsRaw(Output, L"</ClassName>\r\n");
        }
        if (ClassGuid[0])
        {
            OutputPutsRaw(Output, L"        <ClassGuid>");
            OutputXmlText(Output, ClassGuid);
            OutputPutsRaw(Output, L"</ClassGuid>\r\n");
        }
        if (Manufacturer[0])
        {
            OutputPutsRaw(Output, L"        <ManufacturerName>");
            OutputXmlText(Output, Manufacturer);
            OutputPutsRaw(Output, L"</ManufacturerName>\r\n");
        }
        OutputPutsRaw(Output, L"        <Status>");
        OutputXmlText(Output, Status);
        OutputPutsRaw(Output, L"</Status>\r\n");
        if (ProblemCode[0])
        {
            OutputPutsRaw(Output, L"        <ProblemCode>");
            OutputXmlText(Output, ProblemCode);
            OutputPutsRaw(Output, L"</ProblemCode>\r\n");
        }
        if (DriverName[0])
        {
            OutputPutsRaw(Output, L"        <DriverName>");
            OutputXmlText(Output, DriverName);
            OutputPutsRaw(Output, L"</DriverName>\r\n");
        }

        if (Filter->Ids)
        {
            ReadSpdrpMultiSz(DeviceInfoSet, DeviceInfoData, SPDRP_HARDWAREID, List, _countof(List));
            EmitXmlList(Output, L"HardwareIds", L"HardwareId", List, FALSE);
            ReadSpdrpMultiSz(DeviceInfoSet, DeviceInfoData, SPDRP_COMPATIBLEIDS, List, _countof(List));
            EmitXmlList(Output, L"CompatibleIds", L"CompatibleId", List, FALSE);
        }
        if (Filter->Relations)
        {
            GetParentInstanceId(DeviceInfoData->DevInst, Elem, _countof(Elem));
            EmitXmlElement(Output, L"Parent", Elem);
            GetChildrenMultiSz(DeviceInfoData->DevInst, List, _countof(List));
            EmitXmlList(Output, L"Children", L"Child", List, FALSE);
        }
        if (Filter->Stack)
        {
            ReadDevpropStringList(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_Stack, List, _countof(List));
            EmitXmlList(Output, L"Stack", L"Service", List, TRUE);
        }
        if (Filter->Location)
        {
            EmitXmlElement(Output, L"LocationInfo",
                           GetDevicePropertyString(DeviceInfoSet, DeviceInfoData, SPDRP_LOCATION_INFORMATION, Elem, _countof(Elem), NULL));
            ReadDevpropStringList(DeviceInfoSet, DeviceInfoData, &DEVPKEY_Device_LocationPaths, List, _countof(List));
            EmitXmlList(Output, L"LocationPaths", L"LocationPath", List, FALSE);
        }

        OutputPutsRaw(Output, L"    </Device>\r\n");
    }
}

/* Windows prints the banner, "Failed to open output file: <system text>" and exits with
   the Win32 error code. */
static INT
ReportOutputOpenFailure(
    _In_ DWORD Error)
{
    WCHAR Text[256] = L"";

    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error, 0,
                   Text, _countof(Text) - 1, NULL);
    PrintHeader();
    ConPrintf(StdOut, L"Failed to open output file: %ls", Text);
    if (!Text[0])
        ConPuts(StdOut, L"\n");
    return (INT)Error;
}

/* DriverVer comes as written in the INF (m/d/yyyy); print it zero padded as MM/DD/YYYY. */
static VOID
PadDriverVersionDate(
    _Inout_updates_(Cch) PWSTR Version,
    _In_ DWORD Cch)
{
    WCHAR Rest[MAX_PATH];
    PWSTR Start = Version, End;
    ULONG Month, Day, Year;

    Month = wcstoul(Start, &End, 10);
    if (End == Start || *End != L'/')
        return;
    Start = End + 1;
    Day = wcstoul(Start, &End, 10);
    if (End == Start || *End != L'/')
        return;
    Start = End + 1;
    Year = wcstoul(Start, &End, 10);
    if (End == Start)
        return;

    lstrcpynW(Rest, End, _countof(Rest));
    _snwprintf(Version, Cch, L"%02lu/%02lu/%04lu%ls", Month, Day, Year, Rest);
    Version[Cch - 1] = UNICODE_NULL;
}

/* The "nothing found" line Windows writes in every format: it goes to the file too,
   after the csv header or inside the xml root (xml-escaped), with CRLF in a file. */
static VOID
OutputEmptyMessage(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ UINT MessageId)
{
    WCHAR Message[256];

    if (!LoadStringW(GetModuleHandleW(NULL), MessageId, Message, _countof(Message)))
        return;

    if (Output->Format == OutputXml)
        OutputXmlText(Output, Message);
    else
        OutputPutsRaw(Output, Message);
    OutputPutsRaw(Output, Output->File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n");
}

static INT
CommandEnumDrivers(
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    OUTPUT_CONTEXT Output = { OutputText, INVALID_HANDLE_VALUE };
    WCHAR Pattern[MAX_PATH], Path[MAX_PATH], Store[MAX_PATH], Provider[MAX_PATH], GuidText[64];
    WCHAR Version[MAX_PATH], ClassName[MAX_CLASS_NAME_LEN], DefaultName[MAX_PATH], Line[MAX_PATH + 32];
    WCHAR XmlVersion[64], XmlCommand[512];
    PCWSTR OutputFile = NULL, ClassFilter = NULL;
    BOOL OutputFileGiven = FALSE;
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    UINT Length;
    DWORD Count = 0;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"class") && i + 1 < argc)
        {
            ClassFilter = argv[++i];
        }
        else if (IsOption(argv[i], L"format") && i + 1 < argc)
        {
            i++;
            if (_wcsicmp(argv[i], L"txt") == 0)
                Output.Format = OutputText;
            else if (_wcsicmp(argv[i], L"xml") == 0)
                Output.Format = OutputXml;
            else if (_wcsicmp(argv[i], L"csv") == 0)
                Output.Format = OutputCsv;
            else
                return ERROR_INVALID_PARAMETER;
        }
        else if (IsOption(argv[i], L"output-file"))
        {
            OutputFileGiven = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-')
                OutputFile = argv[++i];
        }
        else
        {
            return ERROR_INVALID_PARAMETER;
        }
    }

    if (OutputFileGiven)
    {
        if (!OutputFile)
        {
            BuildDefaultOutputName(Output.Format, DefaultName, _countof(DefaultName));
            OutputFile = DefaultName;
        }
        Output.File = CreateFileW(OutputFile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (Output.File == INVALID_HANDLE_VALUE)
            return ReportOutputOpenFailure(GetLastError());
    }

    if (Output.Format == OutputText || Output.File != INVALID_HANDLE_VALUE)
        PrintHeader();

    if (Output.Format == OutputCsv)
    {
        OutputPutsRaw(&Output, L"DriverName,OriginalName,ProviderName,ClassName,ClassGuid,ClassVersion,"
                               L"ExtensionId,DriverVersion,SignerName,CatalogAttributes,WhcpVersion\r\n");
    }
    else if (Output.Format == OutputXml)
    {
        GetOutputVersion(XmlVersion, _countof(XmlVersion));
        lstrcpynW(XmlCommand, L"/enum-drivers", _countof(XmlCommand));
        for (i = 0; i < argc; i++)
        {
            if (lstrlenW(XmlCommand) + lstrlenW(argv[i]) + 2 >= (INT)_countof(XmlCommand))
                break;
            lstrcatW(XmlCommand, L" ");
            lstrcatW(XmlCommand, argv[i]);
        }
        OutputPutsRaw(&Output, L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
        OutputPutsRaw(&Output, L"<PnpUtil Version=\"");
        OutputXmlText(&Output, XmlVersion);
        OutputPutsRaw(&Output, L"\" Command=\"");
        OutputXmlText(&Output, XmlCommand);
        OutputPutsRaw(&Output, L"\">\r\n");
    }

    Length = GetSystemWindowsDirectoryW(Pattern, _countof(Pattern));
    if (Length == 0 || Length + 16 >= _countof(Pattern))
    {
        if (Output.File != INVALID_HANDLE_VALUE)
            CloseHandle(Output.File);
        return ERROR_GEN_FAILURE;
    }
    wcscat(Pattern, L"\\inf\\");
    Length = (UINT)wcslen(Pattern);
    wcscat(Pattern, L"oem*.inf");

    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            PCWSTR Original = FindData.cFileName;
            GUID ClassGuid;
            HINF hInf;

            if (Length + wcslen(FindData.cFileName) >= _countof(Path))
                continue;

            wcsncpy(Path, Pattern, Length);
            Path[Length] = UNICODE_NULL;
            wcscat(Path, FindData.cFileName);

            hInf = SetupOpenInfFileW(Path, NULL, INF_STYLE_WIN4, NULL);
            if (hInf == INVALID_HANDLE_VALUE)
                continue;

            if (SetupGetInfDriverStoreLocationW(Path, NULL, NULL, Store, _countof(Store), NULL) &&
                wcsrchr(Store, L'\\'))
            {
                Original = wcsrchr(Store, L'\\') + 1;
            }

            GetInfVersionValue(hInf, L"Provider", Provider, _countof(Provider));
            GetInfVersionValue(hInf, L"ClassGUID", GuidText, _countof(GuidText));
            GetInfVersionValue(hInf, L"Class", ClassName, _countof(ClassName));
            if (!ClassName[0] && CLSIDFromString(GuidText, &ClassGuid) == S_OK)
                SetupDiClassNameFromGuidW(&ClassGuid, ClassName, _countof(ClassName), NULL);
            CharLowerW(GuidText);
            GetInfVersionValue(hInf, L"DriverVer", Version, _countof(Version));
            if (wcschr(Version, L','))
                *wcschr(Version, L',') = L' ';
            PadDriverVersionDate(Version, _countof(Version));
            SetupCloseInfFile(hInf);

            if (ClassFilter && _wcsicmp(ClassFilter, ClassName) != 0 && _wcsicmp(ClassFilter, GuidText) != 0)
                continue;

            if (Output.Format == OutputCsv)
            {
                OutputCsvField(&Output, FindData.cFileName, FALSE);
                OutputCsvField(&Output, Original, FALSE);
                OutputCsvField(&Output, Provider, FALSE);
                OutputCsvField(&Output, ClassName, FALSE);
                OutputCsvField(&Output, GuidText, FALSE);
                OutputCsvField(&Output, L"", FALSE);
                OutputCsvField(&Output, L"", FALSE);
                OutputCsvField(&Output, Version, FALSE);
                OutputCsvField(&Output, L"", FALSE);
                OutputCsvField(&Output, L"", FALSE);
                OutputCsvField(&Output, L"", TRUE);
            }
            else if (Output.Format == OutputXml)
            {
                OutputPutsRaw(&Output, L"    <Driver DriverName=\"");
                OutputXmlText(&Output, FindData.cFileName);
                OutputPutsRaw(&Output, L"\">\r\n        <OriginalName>");
                OutputXmlText(&Output, Original);
                OutputPutsRaw(&Output, L"</OriginalName>\r\n        <ProviderName>");
                OutputXmlText(&Output, Provider);
                OutputPutsRaw(&Output, L"</ProviderName>\r\n        <ClassName>");
                OutputXmlText(&Output, ClassName);
                OutputPutsRaw(&Output, L"</ClassName>\r\n        <ClassGuid>");
                OutputXmlText(&Output, GuidText);
                OutputPutsRaw(&Output, L"</ClassGuid>\r\n        <DriverVersion>");
                OutputXmlText(&Output, Version);
                OutputPutsRaw(&Output, L"</DriverVersion>\r\n        <SignerName></SignerName>\r\n"
                                       L"        <CatalogAttributes></CatalogAttributes>\r\n"
                                       L"        <WhcpVersion></WhcpVersion>\r\n    </Driver>\r\n");
            }
            else
            {
                static const PCWSTR Fmt[] = { L"Published Name:     %ls", L"Original Name:      %ls",
                                              L"Provider Name:      %ls", L"Class Name:         %ls",
                                              L"Class GUID:         %ls", L"Driver Version:     %ls" };
                PCWSTR Vals[6];
                INT k;

                Vals[0] = FindData.cFileName;
                Vals[1] = Original;
                Vals[2] = Provider;
                Vals[3] = ClassName;
                Vals[4] = GuidText;
                Vals[5] = Version;
                for (k = 0; k < 6; k++)
                {
                    _snwprintf(Line, _countof(Line) - 1, Fmt[k], Vals[k]);
                    Line[_countof(Line) - 1] = UNICODE_NULL;
                    OutputPutsRaw(&Output, Line);
                    OutputPutsRaw(&Output, Output.File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n");
                }
                OutputPutsRaw(&Output, Output.File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n");
            }
            Count++;
        } while (FindNextFileW(hFind, &FindData));
        FindClose(hFind);
    }

    if (Count == 0)
        OutputEmptyMessage(&Output, IDS_NO_DRIVERS);

    if (Output.Format == OutputXml)
        OutputPutsRaw(&Output, L"</PnpUtil>\r\n");

    if (Output.File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Output.File);
        Output.File = INVALID_HANDLE_VALUE;
        ConResPrintf(StdOut, IDS_OUTPUT_WRITTEN, OutputFile);
        ConPuts(StdOut, L"\n");
    }

    return ERROR_SUCCESS;
}

static INT
CommandEnumDevices(
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    ENUM_FILTER Filter = {0};
    OUTPUT_CONTEXT Output = { OutputText, INVALID_HANDLE_VALUE };
    PCWSTR OutputFile = NULL;
    WCHAR DefaultName[MAX_PATH];
    BOOL OutputFileGiven = FALSE;
    HMODULE hInst = GetModuleHandleW(NULL);
    WCHAR LblInstance[64] = {0}, LblDesc[64] = {0}, LblClass[64] = {0}, LblGuid[64] = {0}, LblMfg[64] = {0};
    WCHAR LblStatus[64] = {0}, LblProblem[64] = {0}, LblDriver[64] = {0}, LblHwIds[64] = {0}, LblCompat[64] = {0};
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    DWORD Index, Count = 0;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"connected"))
        {
            Filter.Connected = TRUE;
        }
        else if (IsOption(argv[i], L"disconnected"))
        {
            Filter.Disconnected = TRUE;
        }
        else if (IsOption(argv[i], L"ids") || IsOption(argv[i], L"deviceids"))
        {
            Filter.Ids = TRUE;
        }
        else if (IsOption(argv[i], L"relations"))
        {
            Filter.Relations = TRUE;
        }
        else if (IsOption(argv[i], L"services"))
        {
            Filter.Services = TRUE;
        }
        else if (IsOption(argv[i], L"stack"))
        {
            Filter.Stack = TRUE;
        }
        else if (IsOption(argv[i], L"location"))
        {
            Filter.Location = TRUE;
        }
        else if (IsOption(argv[i], L"interfaces"))
        {
            Filter.Interfaces = TRUE;
        }
        else if (IsOption(argv[i], L"resources"))
        {
            Filter.Resources = TRUE;
        }
        else if (IsOption(argv[i], L"instanceid") && i + 1 < argc)
        {
            Filter.InstanceId = argv[++i];
        }
        else if (IsOption(argv[i], L"class") && i + 1 < argc)
        {
            Filter.Class = argv[++i];
        }
        else if (IsOption(argv[i], L"problem"))
        {
            Filter.Problem = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-')
            {
                Filter.HasProblemCode = TRUE;
                Filter.ProblemCode = wcstoul(argv[++i], NULL, 0);
            }
        }
        else if (IsOption(argv[i], L"format") && i + 1 < argc)
        {
            i++;
            if (_wcsicmp(argv[i], L"txt") == 0)
                Output.Format = OutputText;
            else if (_wcsicmp(argv[i], L"xml") == 0)
                Output.Format = OutputXml;
            else if (_wcsicmp(argv[i], L"csv") == 0)
                Output.Format = OutputCsv;
            else
                return ERROR_INVALID_PARAMETER;
        }
        else if (IsOption(argv[i], L"output-file"))
        {
            OutputFileGiven = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-')
                OutputFile = argv[++i];
        }
        else
        {
            return ERROR_INVALID_PARAMETER;
        }
    }

    if (OutputFileGiven)
    {
        if (!OutputFile)
        {
            BuildDefaultOutputName(Output.Format, DefaultName, _countof(DefaultName));
            OutputFile = DefaultName;
        }
        Output.File = CreateFileW(OutputFile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (Output.File == INVALID_HANDLE_VALUE)
            return ReportOutputOpenFailure(GetLastError());
    }

    /* The banner goes to stdout for the text format and whenever a file is written;
       csv and xml to the console carry no banner. */
    if (Output.Format == OutputText || Output.File != INVALID_HANDLE_VALUE)
        PrintHeader();

    if (Output.Format == OutputCsv)
    {
        OutputPutsRaw(&Output, L"InstanceId,DeviceDescription,ClassName,ClassGuid,"
                               L"ManufacturerName,Status,ProblemCode,ProblemStatus,"
                               L"DriverName,ExtensionDriverNames");
        if (Filter.Ids)
            OutputPutsRaw(&Output, L",HardwareIds,CompatibleIds");
        if (Filter.Relations)
            OutputPutsRaw(&Output, L",Parent,Children");
        if (Filter.Stack)
            OutputPutsRaw(&Output, L",Stack");
        if (Filter.Location)
            OutputPutsRaw(&Output, L",LocationInfo,LocationPaths");
        /* TODO: /services (Service,UpperFilters,LowerFilters -- xml filter element
           shape not yet captured), /interfaces (Interface,InterfaceStatus) and
           /resources columns land with their emission in the #2a continuation. */
        OutputPutsRaw(&Output, L"\r\n");
    }
    else if (Output.Format == OutputXml)
    {
        WCHAR Version[64], Command[512];

        GetOutputVersion(Version, _countof(Version));
        lstrcpynW(Command, L"/enum-devices", _countof(Command));
        for (i = 0; i < argc; i++)
        {
            if (lstrlenW(Command) + lstrlenW(argv[i]) + 2 >= (INT)_countof(Command))
                break;
            lstrcatW(Command, L" ");
            lstrcatW(Command, argv[i]);
        }
        OutputPutsRaw(&Output, L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
        OutputPutsRaw(&Output, L"<PnpUtil Version=\"");
        OutputXmlText(&Output, Version);
        OutputPutsRaw(&Output, L"\" Command=\"");
        OutputXmlText(&Output, Command);
        OutputPutsRaw(&Output, L"\">\r\n");
    }

    LoadStringW(hInst, IDS_INSTANCE_ID, LblInstance, _countof(LblInstance));
    LoadStringW(hInst, IDS_DEVICE_DESC, LblDesc, _countof(LblDesc));
    LoadStringW(hInst, IDS_CLASS_NAME, LblClass, _countof(LblClass));
    LoadStringW(hInst, IDS_CLASS_GUID, LblGuid, _countof(LblGuid));
    LoadStringW(hInst, IDS_MFG_NAME, LblMfg, _countof(LblMfg));
    LoadStringW(hInst, IDS_STATUS, LblStatus, _countof(LblStatus));
    LoadStringW(hInst, IDS_PROBLEM_CODE, LblProblem, _countof(LblProblem));
    LoadStringW(hInst, IDS_DRIVER_NAME, LblDriver, _countof(LblDriver));
    LoadStringW(hInst, IDS_HARDWARE_IDS, LblHwIds, _countof(LblHwIds));
    LoadStringW(hInst, IDS_COMPATIBLE_IDS, LblCompat, _countof(LblCompat));

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        if (Output.File != INVALID_HANDLE_VALUE)
            CloseHandle(Output.File);
        return (INT)GetLastError();
    }

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        WCHAR InstanceId[MAX_DEVICE_ID_LEN + 1], InfPath[MAX_PATH];
        ULONG Status = 0, Problem = 0;
        BOOL Present;
        PCWSTR State;
        UINT n;

        if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, &DeviceInfoData, InstanceId, _countof(InstanceId), NULL))
            continue;

        Present = CM_Get_DevNode_Status(&Status, &Problem, DeviceInfoData.DevInst, 0) == CR_SUCCESS;

        if ((Filter.Connected && !Present) ||
            (Filter.Disconnected && Present) ||
            (Filter.InstanceId && _wcsicmp(Filter.InstanceId, InstanceId) != 0) ||
            (Filter.Class && !MatchesClass(DeviceInfoSet, &DeviceInfoData, Filter.Class)) ||
            (Filter.Problem && (!Present || !(Status & DN_HAS_PROBLEM))) ||
            (Filter.HasProblemCode && Problem != Filter.ProblemCode))
        {
            continue;
        }

        if (!Present)
            State = L"Disconnected";
        else if (Status & DN_HAS_PROBLEM)
            State = Problem == CM_PROB_DISABLED ? L"Disabled" : L"Problem";
        else if (Status & DN_STARTED)
            State = L"Started";
        else
            State = L"Stopped";

        if (Output.Format == OutputText && Output.File == INVALID_HANDLE_VALUE)
        {
            ConPrintf(StdOut, L"%-28ls%ls\n", LblInstance, InstanceId);
            PrintDeviceProperty(DeviceInfoSet, &DeviceInfoData, SPDRP_DEVICEDESC, LblDesc, NULL);
            PrintDeviceProperty(DeviceInfoSet, &DeviceInfoData, SPDRP_CLASS, LblClass, L"Unknown");
            PrintDeviceProperty(DeviceInfoSet, &DeviceInfoData, SPDRP_CLASSGUID, LblGuid, L"Unknown");
            PrintDeviceProperty(DeviceInfoSet, &DeviceInfoData, SPDRP_MFG, LblMfg, L"Unknown");
            ConPrintf(StdOut, L"%-28ls%ls\n", LblStatus, State);

            if (Present && (Status & DN_HAS_PROBLEM) && Problem != CM_PROB_DISABLED)
            {
                PCWSTR Name = L"";

                for (n = 0; n < _countof(ProblemNames); n++)
                {
                    if (ProblemNames[n].Code == Problem)
                        Name = ProblemNames[n].Name;
                }
                ConPrintf(StdOut, L"%-28ls%lu (0x%02lX) [%ls]\n", LblProblem, Problem, Problem, Name);
            }

            if (GetDriverKeyString(DeviceInfoSet, &DeviceInfoData, REGSTR_VAL_INFPATH, InfPath, _countof(InfPath)))
                ConPrintf(StdOut, L"%-28ls%ls\n", LblDriver, InfPath);

            if (Filter.Ids)
            {
                PrintDeviceProperty(DeviceInfoSet, &DeviceInfoData, SPDRP_HARDWAREID, LblHwIds, NULL);
                PrintDeviceProperty(DeviceInfoSet, &DeviceInfoData, SPDRP_COMPATIBLEIDS, LblCompat, NULL);
            }

            ConPuts(StdOut, L"\n");
        }
        else if (Output.Format == OutputText)
        {
            WCHAR Buffer[1024], Class[256], Guid[64], Mfg[256], Line[2048];
            DWORD Type;

            OutputTxtLine(&Output, LblInstance, InstanceId);
            if (SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet, &DeviceInfoData, SPDRP_DEVICEDESC, &Type,
                                                  (PBYTE)Buffer, sizeof(Buffer) - sizeof(WCHAR), NULL) && Type == REG_SZ)
            {
                Buffer[_countof(Buffer) - 1] = UNICODE_NULL;
                OutputTxtLine(&Output, LblDesc, Buffer);
            }
            OutputTxtLine(&Output, LblClass, GetDevicePropertyString(DeviceInfoSet, &DeviceInfoData, SPDRP_CLASS, Class, _countof(Class), L"Unknown"));
            OutputTxtLine(&Output, LblGuid, GetDevicePropertyString(DeviceInfoSet, &DeviceInfoData, SPDRP_CLASSGUID, Guid, _countof(Guid), L"Unknown"));
            OutputTxtLine(&Output, LblMfg, GetDevicePropertyString(DeviceInfoSet, &DeviceInfoData, SPDRP_MFG, Mfg, _countof(Mfg), L"Unknown"));
            OutputTxtLine(&Output, LblStatus, State);

            if (Present && (Status & DN_HAS_PROBLEM) && Problem != CM_PROB_DISABLED)
            {
                PCWSTR Name = L"";

                for (n = 0; n < _countof(ProblemNames); n++)
                {
                    if (ProblemNames[n].Code == Problem)
                        Name = ProblemNames[n].Name;
                }
                _snwprintf(Line, _countof(Line) - 1, L"%-28ls%lu (0x%02lX) [%ls]\r\n", LblProblem, Problem, Problem, Name);
                Line[_countof(Line) - 1] = UNICODE_NULL;
                OutputPutsRaw(&Output, Line);
            }

            if (GetDriverKeyString(DeviceInfoSet, &DeviceInfoData, REGSTR_VAL_INFPATH, InfPath, _countof(InfPath)))
                OutputTxtLine(&Output, LblDriver, InfPath);

            if (Filter.Ids)
            {
                OutputTxtMultiSz(&Output, DeviceInfoSet, &DeviceInfoData, SPDRP_HARDWAREID, LblHwIds);
                OutputTxtMultiSz(&Output, DeviceInfoSet, &DeviceInfoData, SPDRP_COMPATIBLEIDS, LblCompat);
            }

            OutputPutsRaw(&Output, L"\r\n");
        }
        else
        {
            WCHAR Desc[1024], Class[256], Guid[64], Mfg[256], ProblemText[16];

            if (!GetDriverKeyString(DeviceInfoSet, &DeviceInfoData, REGSTR_VAL_INFPATH, InfPath, _countof(InfPath)))
                InfPath[0] = UNICODE_NULL;
            ProblemText[0] = UNICODE_NULL;
            if (Present && (Status & DN_HAS_PROBLEM) && Problem != CM_PROB_DISABLED)
                _snwprintf(ProblemText, _countof(ProblemText), L"%lu", Problem);

            EmitDeviceRecord(&Output,
                             InstanceId,
                             GetDevicePropertyString(DeviceInfoSet, &DeviceInfoData, SPDRP_DEVICEDESC, Desc, _countof(Desc), NULL),
                             GetDevicePropertyString(DeviceInfoSet, &DeviceInfoData, SPDRP_CLASS, Class, _countof(Class), L"Unknown"),
                             GetDevicePropertyString(DeviceInfoSet, &DeviceInfoData, SPDRP_CLASSGUID, Guid, _countof(Guid), L"Unknown"),
                             GetDevicePropertyString(DeviceInfoSet, &DeviceInfoData, SPDRP_MFG, Mfg, _countof(Mfg), L"Unknown"),
                             State,
                             ProblemText,
                             InfPath,
                             DeviceInfoSet,
                             &DeviceInfoData,
                             &Filter);
        }
        Count++;
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    if (Count == 0)
        OutputEmptyMessage(&Output, IDS_NO_DEVICES);

    if (Output.Format == OutputXml)
        OutputPutsRaw(&Output, L"</PnpUtil>\r\n");

    if (Output.File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Output.File);
        Output.File = INVALID_HANDLE_VALUE;
        ConResPrintf(StdOut, IDS_OUTPUT_WRITTEN, OutputFile);
        ConPuts(StdOut, L"\n");
    }

    return ERROR_SUCCESS;
}

static INT
CommandDevice(
    _In_ DI_FUNCTION Function,
    _In_ DWORD StateChange,
    _In_ PCWSTR Progress,
    _In_ PCWSTR Success,
    _In_ PCWSTR Failure,
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    SP_DEVINFO_DATA DeviceInfoData;
    SP_DEVINSTALL_PARAMS_W InstallParams;
    HDEVINFO DeviceInfoSet;
    PCWSTR InstanceId = NULL;
    BOOL Reboot = FALSE;
    BOOL NeedReboot = FALSE;
    DWORD Error = ERROR_SUCCESS;
    BOOL Result;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"reboot"))
            Reboot = TRUE;
        else if (IsOption(argv[i], L"subtree"))
            continue;
        else if (!InstanceId)
            InstanceId = argv[i];
        else
            return ERROR_INVALID_PARAMETER;
    }

    if (!InstanceId)
        return ERROR_INVALID_PARAMETER;

    PrintHeader();

    DeviceInfoSet = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return (INT)GetLastError();

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    if (!SetupDiOpenDeviceInfoW(DeviceInfoSet, InstanceId, NULL, 0, &DeviceInfoData))
    {
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);
        return ERROR_SUCCESS;
    }

    ConPrintf(StdOut, L"%ls%ls\n", Progress, InstanceId);

    Result = TRUE;
    if (Function == DIF_PROPERTYCHANGE)
    {
        SP_PROPCHANGE_PARAMS PropChange;

        PropChange.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        PropChange.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        PropChange.StateChange = StateChange;
        PropChange.Scope = StateChange == DICS_PROPCHANGE ? DICS_FLAG_CONFIGSPECIFIC : DICS_FLAG_GLOBAL;
        PropChange.HwProfile = 0;
        Result = SetupDiSetClassInstallParamsW(DeviceInfoSet,
                                               &DeviceInfoData,
                                               &PropChange.ClassInstallHeader,
                                               sizeof(PropChange));
    }
    if (Result)
        Result = SetupDiCallClassInstaller(Function, DeviceInfoSet, &DeviceInfoData);

    if (Result)
    {
        InstallParams.cbSize = sizeof(InstallParams);
        if (Function != DIF_REMOVE &&
            SetupDiGetDeviceInstallParamsW(DeviceInfoSet, &DeviceInfoData, &InstallParams))
        {
            NeedReboot = (InstallParams.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)) != 0;
        }
        if (!NeedReboot && Function == DIF_PROPERTYCHANGE && StateChange != DICS_DISABLE)
        {
            ULONG Status = 0, Problem = 0;

            if (CM_Get_DevNode_Status(&Status, &Problem, DeviceInfoData.DevInst, 0) != CR_SUCCESS ||
                !(Status & DN_STARTED))
            {
                Result = FALSE;
                SetLastError(ERROR_GEN_FAILURE);
            }
        }
    }

    if (!Result)
    {
        Error = GetLastError();
        PrintError(Failure, Error);
    }
    else if (!NeedReboot)
    {
        ConPrintf(StdOut, L"%ls\n", Success);
    }
    ConPuts(StdOut, L"\n");

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    return FinishOperation(Error, NeedReboot, Reboot);
}

static INT
CommandScanDevices(VOID)
{
    PrintHeader();
    ConPuts(StdOut, L"Scanning for device hardware changes.\n");
    ScanDevices();
    ConPuts(StdOut, L"Scan complete.\n\n");
    return ERROR_SUCCESS;
}

static INT
CommandRollbackDevice(_In_ INT argc, _In_ WCHAR **argv)
{
    PCWSTR InstanceId = NULL;
    DWORD Error;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"reboot"))
            continue;
        else if (!InstanceId)
            InstanceId = argv[i];
        else
            return ERROR_INVALID_PARAMETER;
    }

    if (!InstanceId)
        return ERROR_INVALID_PARAMETER;

    PrintHeader();
    ConPrintf(StdOut, L"Rolling back device:       %s\n", InstanceId);

    Error = DrvInstRollback(InstanceId);
    if (Error != ERROR_SUCCESS)
    {
        ConPrintf(StdErr, L"Failed to roll back the device (error %lu).\n", Error);
        return (INT)Error;
    }

    ConPuts(StdOut, L"Device rolled back successfully.\n");
    return ERROR_SUCCESS;
}

/* /export-driver ***********************************************************/

#define EXPORT_MAX_DEPTH 8
#ifndef ERROR_NOT_AN_INSTALLED_OEM_INF
#define ERROR_NOT_AN_INSTALLED_OEM_INF ((DWORD)0xE000023C)
#endif

/* Case-insensitive "oem<digits>.inf" check, the only names the store publishes. */
static BOOL
IsPublishedInfName(
    _In_ PCWSTR Name)
{
    PCWSTR p = Name;

    if (_wcsnicmp(Name, L"oem", 3) != 0)
        return FALSE;
    p += 3;
    if (*p < L'0' || *p > L'9')
        return FALSE;
    while (*p >= L'0' && *p <= L'9')
        p++;
    return _wcsicmp(p, L".inf") == 0;
}

/* True when Path is Root followed by a separator (so Path is strictly inside Root). */
static BOOL
IsInsideDirectory(
    _In_ PCWSTR Path,
    _In_ PCWSTR Root)
{
    SIZE_T Length = wcslen(Root);

    if (Length != 0 && Root[Length - 1] == L'\\')
        Length--;
    return _wcsnicmp(Path, Root, Length) == 0 && Path[Length] == L'\\' && Path[Length + 1] != UNICODE_NULL;
}

/* Every create below is relative to an already open parent directory handle, so no path
   component is resolved a second time: a junction or symlink swapped into the target
   after the first open cannot redirect the export. */
typedef struct _EXPORT_USTR
{
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} EXPORT_USTR;

typedef struct _EXPORT_OA
{
    ULONG Length;
    HANDLE RootDirectory;
    EXPORT_USTR *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} EXPORT_OA;

typedef struct _EXPORT_IOSB
{
    LONG Status;
    ULONG_PTR Information;
} EXPORT_IOSB;

typedef LONG (NTAPI *PEXPORT_NTCREATEFILE)(PHANDLE, ACCESS_MASK, EXPORT_OA *, EXPORT_IOSB *, PLARGE_INTEGER,
                                           ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef ULONG (NTAPI *PEXPORT_STATUSTODOS)(LONG);

#define EXPORT_FILE_CREATE              0x00000002
#define EXPORT_FILE_OPEN_IF             0x00000003
#define EXPORT_FILE_DIRECTORY_FILE      0x00000001
#define EXPORT_FILE_NON_DIRECTORY_FILE  0x00000040
#define EXPORT_FILE_SYNC_IO             0x00000020
#define EXPORT_FILE_OPEN_REPARSE_POINT  0x00200000
#define EXPORT_OBJ_CASE_INSENSITIVE     0x00000040
#define EXPORT_NAME_COLLISION           ((LONG)0xC0000035)

/* Create (or open, for a directory) Name inside Parent without following a reparse point.
   *Existed is set when a file was already there; such a file is left alone. */
static DWORD
ExportCreateChild(
    _In_ HANDLE Parent,
    _In_ PCWSTR Name,
    _In_ BOOL Directory,
    _Out_ PHANDLE Handle,
    _Out_ PBOOL Existed)
{
    static PEXPORT_NTCREATEFILE CreateFn;
    static PEXPORT_STATUSTODOS ToDos;
    EXPORT_USTR UName;
    EXPORT_OA Oa;
    EXPORT_IOSB Iosb;
    BY_HANDLE_FILE_INFORMATION Info;
    ACCESS_MASK Access;
    LONG Status;

    *Handle = NULL;
    *Existed = FALSE;

    if (!CreateFn)
    {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");

        CreateFn = (PEXPORT_NTCREATEFILE)GetProcAddress(hNtdll, "NtCreateFile");
        ToDos = (PEXPORT_STATUSTODOS)GetProcAddress(hNtdll, "RtlNtStatusToDosError");
    }
    if (!CreateFn || !ToDos)
        return ERROR_PROC_NOT_FOUND;

    UName.Buffer = (PWSTR)Name;
    UName.Length = (USHORT)(wcslen(Name) * sizeof(WCHAR));
    UName.MaximumLength = UName.Length;
    Oa.Length = sizeof(Oa);
    Oa.RootDirectory = Parent;
    Oa.ObjectName = &UName;
    Oa.Attributes = EXPORT_OBJ_CASE_INSENSITIVE;
    Oa.SecurityDescriptor = NULL;
    Oa.SecurityQualityOfService = NULL;

    if (Directory)
    {
        Access = FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        Status = CreateFn(Handle, Access, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, EXPORT_FILE_OPEN_IF,
                          EXPORT_FILE_DIRECTORY_FILE | EXPORT_FILE_SYNC_IO | EXPORT_FILE_OPEN_REPARSE_POINT,
                          NULL, 0);
    }
    else
    {
        Access = FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE;
        Status = CreateFn(Handle, Access, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          EXPORT_FILE_CREATE,
                          EXPORT_FILE_NON_DIRECTORY_FILE | EXPORT_FILE_SYNC_IO | EXPORT_FILE_OPEN_REPARSE_POINT,
                          NULL, 0);
    }

    if (Status == EXPORT_NAME_COLLISION)
    {
        *Existed = TRUE;
        return ERROR_SUCCESS;
    }
    if (Status < 0)
    {
        *Handle = NULL;
        return ToDos(Status);
    }

    if (Directory)
    {
        if (!GetFileInformationByHandle(*Handle, &Info) ||
            !(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        {
            CloseHandle(*Handle);
            *Handle = NULL;
            return ERROR_ACCESS_DENIED;
        }
    }
    return ERROR_SUCCESS;
}

/* Mark the file behind Handle for deletion on close (used to drop a partial copy). */
static VOID
ExportDeleteOnClose(
    _In_ HANDLE Handle)
{
    typedef LONG (NTAPI *PSETINFO)(HANDLE, EXPORT_IOSB *, PVOID, ULONG, ULONG);
    PSETINFO SetInfo = (PSETINFO)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile");
    EXPORT_IOSB Iosb;
    BOOLEAN DeleteFlag = TRUE;

    if (SetInfo)
        SetInfo(Handle, &Iosb, &DeleteFlag, sizeof(DeleteFlag), 13 /* FileDispositionInformation */);
}

/* Copy one regular file into Parent. An existing destination entry is left as it is. */
static DWORD
ExportCopyFile(
    _In_ PCWSTR SourcePath,
    _In_ HANDLE Parent,
    _In_ PCWSTR Name)
{
    BYTE Buffer[16384];
    FILETIME Created, Accessed, Written;
    HANDLE hSource, hDestination;
    DWORD Read, Written2, Error;
    BOOL Existed;

    Error = ExportCreateChild(Parent, Name, FALSE, &hDestination, &Existed);
    if (Error != ERROR_SUCCESS || Existed)
        return Error;

    hSource = CreateFileW(SourcePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (hSource == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        CloseHandle(hDestination);
        return Error;
    }

    for (;;)
    {
        if (!ReadFile(hSource, Buffer, sizeof(Buffer), &Read, NULL))
        {
            Error = GetLastError();
            if (Error == ERROR_SUCCESS)
                Error = ERROR_READ_FAULT;
            break;
        }
        if (Read == 0)
            break;
        if (!WriteFile(hDestination, Buffer, Read, &Written2, NULL) || Written2 != Read)
        {
            Error = GetLastError();
            if (Error == ERROR_SUCCESS)
                Error = ERROR_WRITE_FAULT;
            break;
        }
    }
    if (Error == ERROR_SUCCESS && GetFileTime(hSource, &Created, &Accessed, &Written))
        SetFileTime(hDestination, &Created, &Accessed, &Written);

    if (Error != ERROR_SUCCESS)
        ExportDeleteOnClose(hDestination);

    CloseHandle(hSource);
    CloseHandle(hDestination);
    return Error;
}

/* Copy a package directory tree into the open directory handle Destination. Reparse points
   in the source are skipped and names that carry a separator or colon are refused. */
static DWORD
ExportCopyTree(
    _In_ PCWSTR Source,
    _In_ HANDLE Destination,
    _In_ DWORD Depth)
{
    WCHAR Pattern[MAX_PATH], SourceChild[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFind, hChild;
    DWORD Error = ERROR_SUCCESS;
    BOOL Existed;

    if (Depth > EXPORT_MAX_DEPTH)
        return ERROR_BAD_PATHNAME;
    if (wcslen(Source) + 3 >= _countof(Pattern))
        return ERROR_FILENAME_EXCED_RANGE;
    wcscpy(Pattern, Source);
    wcscat(Pattern, L"\\*");

    hFind = FindFirstFileW(Pattern, &FindData);
    if (hFind == INVALID_HANDLE_VALUE)
        return GetLastError();

    do
    {
        PCWSTR Name = FindData.cFileName;

        if (wcscmp(Name, L".") == 0 || wcscmp(Name, L"..") == 0)
            continue;
        if (wcspbrk(Name, L"\\/:") != NULL)
            continue;
        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        if (wcslen(Source) + wcslen(Name) + 2 >= _countof(SourceChild))
        {
            Error = ERROR_FILENAME_EXCED_RANGE;
            break;
        }

        wcscpy(SourceChild, Source);
        wcscat(SourceChild, L"\\");
        wcscat(SourceChild, Name);

        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            Error = ExportCreateChild(Destination, Name, TRUE, &hChild, &Existed);
            if (Error == ERROR_SUCCESS && hChild)
            {
                Error = ExportCopyTree(SourceChild, hChild, Depth + 1);
                CloseHandle(hChild);
            }
        }
        else
        {
            Error = ExportCopyFile(SourceChild, Destination, Name);
        }
    } while (Error == ERROR_SUCCESS && FindNextFileW(hFind, &FindData));

    FindClose(hFind);
    return Error;
}

/* Export one published package into the open Root directory. Returns a Win32/setupapi error;
   Flat copies the package files straight into Root, otherwise into Root\<store directory name>. */
static DWORD
ExportOnePackage(
    _In_ PCWSTR Published,
    _In_ HANDLE Root,
    _In_ BOOL Flat,
    _Out_writes_(OriginalCch) PWSTR Original,
    _In_ DWORD OriginalCch)
{
    WCHAR InfPath[MAX_PATH], Store[MAX_PATH], Repository[MAX_PATH];
    HANDLE hPackage;
    PWSTR Leaf;
    UINT Length;
    DWORD Error;
    BOOL Existed;

    Original[0] = UNICODE_NULL;

    if (!IsPublishedInfName(Published))
        return ERROR_NOT_AN_INSTALLED_OEM_INF;

    Length = GetSystemWindowsDirectoryW(InfPath, _countof(InfPath));
    if (Length == 0 || Length + 64 + wcslen(Published) >= _countof(InfPath))
        return ERROR_BAD_PATHNAME;
    wcscpy(Repository, InfPath);
    wcscat(Repository, L"\\System32\\DriverStore\\FileRepository");
    wcscat(InfPath, L"\\inf\\");
    wcscat(InfPath, Published);

    if (GetFileAttributesW(InfPath) == INVALID_FILE_ATTRIBUTES ||
        !SetupGetInfDriverStoreLocationW(InfPath, NULL, NULL, Store, _countof(Store), NULL))
    {
        return ERROR_NOT_AN_INSTALLED_OEM_INF;
    }

    /* Store is <Repository>\<package dir>\<original>.inf; refuse anything else. */
    Leaf = wcsrchr(Store, L'\\');
    if (!Leaf || !IsInsideDirectory(Store, Repository))
        return ERROR_NOT_AN_INSTALLED_OEM_INF;
    lstrcpynW(Original, Leaf + 1, OriginalCch);
    *Leaf = UNICODE_NULL;

    if (Flat)
        return ExportCopyTree(Store, Root, 0);

    Leaf = wcsrchr(Store, L'\\');
    if (!Leaf || Leaf[1] == UNICODE_NULL)
        return ERROR_BAD_PATHNAME;
    Error = ExportCreateChild(Root, Leaf + 1, TRUE, &hPackage, &Existed);
    if (Error != ERROR_SUCCESS || !hPackage)
        return Error;
    Error = ExportCopyTree(Store, hPackage, 1);
    CloseHandle(hPackage);
    return Error;
}

static INT
CommandExportDriver(
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    WCHAR Target[MAX_PATH], Pattern[MAX_PATH], Original[MAX_PATH];
    WCHAR (*Names)[32] = NULL, (*Grown)[32];
    DWORD Capacity = 0;
    HANDLE hRoot = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW FindData;
    HANDLE hFind;
    DWORD Attributes, Error, Total = 0, Exported = 0, Result = ERROR_SUCCESS, Count = 0, k;
    UINT Length;
    SIZE_T TargetLength;
    BOOL All;

    if (argc != 2)
        return ERROR_INVALID_PARAMETER;

    PrintHeader();
    All = wcscmp(argv[0], L"*") == 0;

    if (!GetFullPathNameW(argv[1], _countof(Target), Target, NULL))
        Error = GetLastError();
    else
        Error = ERROR_SUCCESS;

    if (Error == ERROR_SUCCESS)
    {
        TargetLength = wcslen(Target);
        while (TargetLength > 3 && Target[TargetLength - 1] == L'\\')
            Target[--TargetLength] = UNICODE_NULL;
        Attributes = GetFileAttributesW(Target);
        if (Attributes == INVALID_FILE_ATTRIBUTES)
            Error = GetLastError();
        else if (!(Attributes & FILE_ATTRIBUTE_DIRECTORY))
            Error = ERROR_BAD_PATHNAME;
    }
    if (Error == ERROR_SUCCESS)
    {
        /* One open of the target; everything below is created relative to this handle. */
        hRoot = CreateFileW(Target, FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hRoot == INVALID_HANDLE_VALUE)
        {
            Error = GetLastError();
        }
        else
        {
            BY_HANDLE_FILE_INFORMATION RootInfo;

            if (!GetFileInformationByHandle(hRoot, &RootInfo) ||
                !(RootInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                CloseHandle(hRoot);
                hRoot = INVALID_HANDLE_VALUE;
                Error = ERROR_BAD_PATHNAME;
            }
        }
    }
    if (Error != ERROR_SUCCESS)
    {
        ConPuts(StdOut, L"Failed to export driver package: The destination directory is missing or invalid.\n");
        ConPrintf(StdOut, L"Total driver packages:      0\nExported driver packages:   0\n");
        return (INT)Error;
    }

    if (!All)
    {
        Total = 1;
        Error = ExportOnePackage(argv[0], hRoot, TRUE, Original, _countof(Original));
        if (Error == ERROR_SUCCESS)
        {
            ConPrintf(StdOut, L"Exporting driver package:   %ls (%ls)\n", argv[0], Original);
            ConPuts(StdOut, L"Driver package exported successfully.\n\n");
            Exported = 1;
        }
        else
        {
            if (Error == ERROR_NOT_AN_INSTALLED_OEM_INF)
                ConPuts(StdOut, L"Failed to export driver package: The specified file is not an installed OEM INF.\n\n");
            else
                ConPrintf(StdOut, L"Failed to export driver package: error %lu.\n\n", Error);
            Total = 0;
            Result = Error;
        }
    }
    else
    {
        Length = GetSystemWindowsDirectoryW(Pattern, _countof(Pattern));
        if (Length == 0 || Length + 16 >= _countof(Pattern))
        {
            CloseHandle(hRoot);
            return ERROR_GEN_FAILURE;
        }
        wcscat(Pattern, L"\\inf\\oem*.inf");
        hFind = FindFirstFileW(Pattern, &FindData);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!IsPublishedInfName(FindData.cFileName))
                    continue;
                if (Count == Capacity)
                {
                    Capacity = Capacity ? Capacity * 2 : 256;
                    Grown = Names ? HeapReAlloc(GetProcessHeap(), 0, Names, Capacity * sizeof(*Names))
                                  : HeapAlloc(GetProcessHeap(), 0, Capacity * sizeof(*Names));
                    if (!Grown)
                        break;
                    Names = Grown;
                }
                lstrcpynW(Names[Count++], FindData.cFileName, _countof(Names[0]));
            } while (FindNextFileW(hFind, &FindData));
            FindClose(hFind);
        }

        for (k = 0; k < Count; k++)
        {
            Total++;
            Error = ExportOnePackage(Names[k], hRoot, FALSE, Original, _countof(Original));
            if (Error == ERROR_SUCCESS)
            {
                ConPrintf(StdOut, L"Exporting driver package:   %ls (%ls)\n", Names[k], Original);
                ConPuts(StdOut, L"Driver package exported successfully.\n\n");
                Exported++;
            }
            else
            {
                ConPrintf(StdOut, L"Failed to export driver package: %ls: error %lu.\n\n", Names[k], Error);
                Result = Error;
            }
        }
    }

    CloseHandle(hRoot);
    if (Names)
        HeapFree(GetProcessHeap(), 0, Names);
    ConPrintf(StdOut, L"Total driver packages:      %lu\n", Total);
    ConPrintf(StdOut, L"Exported driver packages:   %lu\n", Exported);
    return (INT)Result;
}

/* /enum-classes ************************************************************/

typedef struct _CLASS_ROW
{
    GUID Guid;
    WCHAR Name[MAX_CLASS_NAME_LEN];
    WCHAR Description[LINE_LEN];
    PWSTR Service;
    PWSTR Upper;
    PWSTR Lower;
} CLASS_ROW;

/* Shared empty double-NUL list used when a class value is absent. */
static WCHAR ClassEmpty[2];

/* Read a class registry value (REG_SZ / REG_MULTI_SZ) into a heap buffer of the size the
   registry reports, as a double-NUL list. Absent or unreadable values give ClassEmpty. */
static PWSTR
ReadClassValue(
    _In_ HKEY hKey,
    _In_ PCWSTR Name)
{
    DWORD Type, Size = 0, Error;
    PWSTR Buffer;

    if (RegQueryValueExW(hKey, Name, NULL, &Type, NULL, &Size) != ERROR_SUCCESS ||
        (Type != REG_SZ && Type != REG_MULTI_SZ))
    {
        return ClassEmpty;
    }

    for (;;)
    {
        Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + 2 * sizeof(WCHAR));
        if (!Buffer)
            return ClassEmpty;
        Error = RegQueryValueExW(hKey, Name, NULL, &Type, (PBYTE)Buffer, &Size);
        if (Error == ERROR_SUCCESS)
            return Buffer;
        HeapFree(GetProcessHeap(), 0, Buffer);
        /* The value grew between the two calls: retry with the new size. */
        if (Error != ERROR_MORE_DATA)
            return ClassEmpty;
    }
}

static VOID
FreeClassValue(
    _In_ PWSTR Value)
{
    if (Value != ClassEmpty)
        HeapFree(GetProcessHeap(), 0, Value);
}

/* One "Label<pad>Value" text line, value starting at column 28. */
static VOID
ClassTxtOne(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Label,
    _In_ PCWSTR Value)
{
    WCHAR Line[2048];

    _snwprintf(Line, _countof(Line) - 1, L"%-28ls%ls%ls", Label, Value,
               Output->File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n");
    Line[_countof(Line) - 1] = UNICODE_NULL;
    OutputPutsRaw(Output, Line);
}

/* A double-NUL list as text lines; extra values get blank-label lines. */
static VOID
ClassTxtList(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Label,
    _In_ PCWSTR List)
{
    WCHAR Line[2048];
    PCWSTR Eol = Output->File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n";

    for (; *List; List += wcslen(List) + 1)
    {
        _snwprintf(Line, _countof(Line) - 1, L"%-28ls%ls%ls", Label, List, Eol);
        Line[_countof(Line) - 1] = UNICODE_NULL;
        OutputPutsRaw(Output, Line);
        Label = L"";
    }
}

static VOID
ClassCsvList(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR List,
    _In_ BOOL Last)
{
    WCHAR Joined[2048] = L"";
    PCWSTR p;

    for (p = List; *p; p += wcslen(p) + 1)
    {
        if (Joined[0] && wcslen(Joined) + 1 < _countof(Joined))
            wcscat(Joined, L";");
        if (wcslen(Joined) + wcslen(p) + 1 < _countof(Joined))
            wcscat(Joined, p);
    }
    OutputCsvField(Output, Joined, Last);
}

static VOID
ClassXmlList(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ PCWSTR Element,
    _In_ PCWSTR List)
{
    PCWSTR p;

    if (!*List)
        return;
    OutputPutsRaw(Output, L"        <");
    OutputPutsRaw(Output, Element);
    OutputPutsRaw(Output, L">\r\n");
    for (p = List; *p; p += wcslen(p) + 1)
    {
        OutputPutsRaw(Output, L"            <Service>");
        OutputXmlText(Output, p);
        OutputPutsRaw(Output, L"</Service>\r\n");
    }
    OutputPutsRaw(Output, L"        </");
    OutputPutsRaw(Output, Element);
    OutputPutsRaw(Output, L">\r\n");
}

static INT
CommandEnumClasses(
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    OUTPUT_CONTEXT Output = { OutputText, INVALID_HANDLE_VALUE };
    WCHAR DefaultName[MAX_PATH], GuidText[64], XmlVersion[64], XmlCommand[512];
    WCHAR Failure[256] = L"";
    PCWSTR OutputFile = NULL, ClassFilter = NULL;
    BOOL OutputFileGiven = FALSE, Services = FALSE;
    GUID FilterGuid, *Guids = NULL;
    BOOL FilterIsGuid = FALSE;
    CLASS_ROW *Row = NULL;
    DWORD Capacity = 64, Required = 0, Index, Count = 0, Error = ERROR_SUCCESS;
    PCWSTR Eol;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"class") && i + 1 < argc)
        {
            ClassFilter = argv[++i];
        }
        else if (IsOption(argv[i], L"services"))
        {
            Services = TRUE;
        }
        else if (IsOption(argv[i], L"format") && i + 1 < argc)
        {
            i++;
            if (_wcsicmp(argv[i], L"txt") == 0)
                Output.Format = OutputText;
            else if (_wcsicmp(argv[i], L"xml") == 0)
                Output.Format = OutputXml;
            else if (_wcsicmp(argv[i], L"csv") == 0)
                Output.Format = OutputCsv;
            else
                return ERROR_INVALID_PARAMETER;
        }
        else if (IsOption(argv[i], L"output-file"))
        {
            OutputFileGiven = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-')
                OutputFile = argv[++i];
        }
        else
        {
            return ERROR_INVALID_PARAMETER;
        }
    }

    if (OutputFileGiven)
    {
        if (!OutputFile)
        {
            BuildDefaultOutputName(Output.Format, DefaultName, _countof(DefaultName));
            OutputFile = DefaultName;
        }
        Output.File = CreateFileW(OutputFile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (Output.File == INVALID_HANDLE_VALUE)
            return ReportOutputOpenFailure(GetLastError());
    }
    Eol = Output.File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n";

    if (Output.Format == OutputText || Output.File != INVALID_HANDLE_VALUE)
        PrintHeader();

    if (ClassFilter && ClassFilter[0] == L'{' && CLSIDFromString(ClassFilter, &FilterGuid) == S_OK)
        FilterIsGuid = TRUE;

    if (Output.Format == OutputCsv)
    {
        OutputPutsRaw(&Output, L"ClassName,ClassGuid,ClassDescription");
        if (Services)
            OutputPutsRaw(&Output, L",DefaultService,UpperFilters,LowerFilters");
        OutputPutsRaw(&Output, L"\r\n");
    }
    else if (Output.Format == OutputXml)
    {
        GetOutputVersion(XmlVersion, _countof(XmlVersion));
        lstrcpynW(XmlCommand, L"/enum-classes", _countof(XmlCommand));
        for (i = 0; i < argc; i++)
        {
            if (lstrlenW(XmlCommand) + lstrlenW(argv[i]) + 2 >= (INT)_countof(XmlCommand))
                break;
            lstrcatW(XmlCommand, L" ");
            lstrcatW(XmlCommand, argv[i]);
        }
        OutputPutsRaw(&Output, L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
        OutputPutsRaw(&Output, L"<PnpUtil Version=\"");
        OutputXmlText(&Output, XmlVersion);
        OutputPutsRaw(&Output, L"\" Command=\"");
        OutputXmlText(&Output, XmlCommand);
        OutputPutsRaw(&Output, L"\">\r\n");
    }

    Row = HeapAlloc(GetProcessHeap(), 0, sizeof(*Row));
    for (;;)
    {
        Guids = HeapAlloc(GetProcessHeap(), 0, Capacity * sizeof(GUID));
        if (!Guids || !Row)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        if (SetupDiBuildClassInfoList(0, Guids, Capacity, &Required))
            break;
        Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Guids);
        Guids = NULL;
        if (Error != ERROR_INSUFFICIENT_BUFFER || Required <= Capacity)
            break;
        Capacity = Required;
        Error = ERROR_SUCCESS;
    }

    if (Error == ERROR_SUCCESS)
    {
        for (Index = 0; Index < Required; Index++)
        {
            HKEY hKey;

            if (!SetupDiClassNameFromGuidW(&Guids[Index], Row->Name, _countof(Row->Name), NULL))
                continue;
            if (ClassFilter)
            {
                if (FilterIsGuid ? !IsEqualGUID(&Guids[Index], &FilterGuid) : _wcsicmp(ClassFilter, Row->Name) != 0)
                    continue;
            }
            Row->Guid = Guids[Index];
            Row->Description[0] = UNICODE_NULL;
            SetupDiGetClassDescriptionW(&Guids[Index], Row->Description, _countof(Row->Description), NULL);
            Row->Service = Row->Upper = Row->Lower = ClassEmpty;
            if (Services)
            {
                hKey = SetupDiOpenClassRegKey(&Guids[Index], KEY_READ);
                if (hKey != INVALID_HANDLE_VALUE)
                {
                    Row->Service = ReadClassValue(hKey, L"Service");
                    Row->Upper = ReadClassValue(hKey, L"UpperFilters");
                    Row->Lower = ReadClassValue(hKey, L"LowerFilters");
                    RegCloseKey(hKey);
                }
            }

            StringFromGUID2(&Row->Guid, GuidText, _countof(GuidText));
            CharLowerW(GuidText);

            if (Output.Format == OutputCsv)
            {
                OutputCsvField(&Output, Row->Name, FALSE);
                OutputCsvField(&Output, GuidText, FALSE);
                OutputCsvField(&Output, Row->Description, !Services);
                if (Services)
                {
                    OutputCsvField(&Output, Row->Service, FALSE);
                    ClassCsvList(&Output, Row->Upper, FALSE);
                    ClassCsvList(&Output, Row->Lower, TRUE);
                }
            }
            else if (Output.Format == OutputXml)
            {
                OutputPutsRaw(&Output, L"    <SetupClass ClassName=\"");
                OutputXmlText(&Output, Row->Name);
                OutputPutsRaw(&Output, L"\" ClassGuid=\"");
                OutputXmlText(&Output, GuidText);
                OutputPutsRaw(&Output, L"\">\r\n");
                if (Row->Description[0])
                {
                    OutputPutsRaw(&Output, L"        <Description>");
                    OutputXmlText(&Output, Row->Description);
                    OutputPutsRaw(&Output, L"</Description>\r\n");
                }
                if (Row->Service[0])
                {
                    OutputPutsRaw(&Output, L"        <DefaultService>");
                    OutputXmlText(&Output, Row->Service);
                    OutputPutsRaw(&Output, L"</DefaultService>\r\n");
                }
                ClassXmlList(&Output, L"UpperFilters", Row->Upper);
                ClassXmlList(&Output, L"LowerFilters", Row->Lower);
                OutputPutsRaw(&Output, L"    </SetupClass>\r\n");
            }
            else
            {
                ClassTxtOne(&Output, L"Class Name:", Row->Name);
                ClassTxtOne(&Output, L"Class GUID:", GuidText);
                if (Row->Description[0])
                    ClassTxtOne(&Output, L"Class Description:", Row->Description);
                if (Services)
                {
                    ClassTxtList(&Output, L"Default Service:", Row->Service);
                    ClassTxtList(&Output, L"Upper Filters:", Row->Upper);
                    ClassTxtList(&Output, L"Lower Filters:", Row->Lower);
                }
                OutputPutsRaw(&Output, Eol);
            }
            FreeClassValue(Row->Service);
            FreeClassValue(Row->Upper);
            FreeClassValue(Row->Lower);
            Count++;
        }
        if (Count == 0)
            Error = ERROR_FILE_NOT_FOUND;
    }

    if (Error != ERROR_SUCCESS)
    {
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error, 0,
                       Failure, _countof(Failure) - 1, NULL);
        {
            WCHAR Text[512];

            _snwprintf(Text, _countof(Text) - 1, L"Failed to enumerate classes: %ls", Failure);
            Text[_countof(Text) - 1] = UNICODE_NULL;
            if (Output.Format == OutputXml)
                OutputXmlText(&Output, Text);
            else
                OutputPutsRaw(&Output, Text);
            if (!Failure[0])
                OutputPutsRaw(&Output, Eol);
            OutputPutsRaw(&Output, Eol);
        }
    }

    if (Output.Format == OutputXml)
        OutputPutsRaw(&Output, L"</PnpUtil>\r\n");

    if (Guids)
        HeapFree(GetProcessHeap(), 0, Guids);
    if (Row)
        HeapFree(GetProcessHeap(), 0, Row);

    if (Output.File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Output.File);
        Output.File = INVALID_HANDLE_VALUE;
        ConResPrintf(StdOut, IDS_OUTPUT_WRITTEN, OutputFile);
        ConPuts(StdOut, L"\n");
    }

    return (INT)Error;
}

/* /enum-interfaces *********************************************************/

typedef struct _INTERFACE_ROW
{
    PWSTR Path;
    WCHAR Description[LINE_LEN];
    WCHAR ClassGuid[64];
    PCWSTR Reference;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    BOOL Enabled;
} INTERFACE_ROW;

static VOID
EmitInterface(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ const INTERFACE_ROW *Row)
{
    PCWSTR Status = Row->Enabled ? L"Enabled" : L"Disabled";

    if (Output->Format == OutputCsv)
    {
        OutputCsvField(Output, Row->Path, FALSE);
        OutputCsvField(Output, Row->Description, FALSE);
        OutputCsvField(Output, Row->ClassGuid, FALSE);
        OutputCsvField(Output, Row->Reference, FALSE);
        OutputCsvField(Output, Row->InstanceId, FALSE);
        OutputCsvField(Output, Status, TRUE);
    }
    else if (Output->Format == OutputXml)
    {
        OutputPutsRaw(Output, L"    <Interface Path=\"");
        OutputXmlText(Output, Row->Path);
        OutputPutsRaw(Output, L"\">\r\n        <Description>");
        OutputXmlText(Output, Row->Description);
        OutputPutsRaw(Output, L"</Description>\r\n        <ClassGuid>");
        OutputXmlText(Output, Row->ClassGuid);
        OutputPutsRaw(Output, L"</ClassGuid>\r\n");
        if (Row->Reference[0])
        {
            OutputPutsRaw(Output, L"        <ReferenceString>");
            OutputXmlText(Output, Row->Reference);
            OutputPutsRaw(Output, L"</ReferenceString>\r\n");
        }
        OutputPutsRaw(Output, L"        <DeviceInstanceId>");
        OutputXmlText(Output, Row->InstanceId);
        OutputPutsRaw(Output, L"</DeviceInstanceId>\r\n        <Status>");
        OutputXmlText(Output, Status);
        OutputPutsRaw(Output, L"</Status>\r\n    </Interface>\r\n");
    }
    else
    {
        WCHAR Line[2048];
        PCWSTR Eol = Output->File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n";
        PCWSTR Labels[6], Values[6];
        INT k, n = 0;

        Labels[n] = L"Interface Path:";
        Values[n++] = Row->Path;
        Labels[n] = L"Interface Description:";
        Values[n++] = Row->Description;
        Labels[n] = L"Interface Class GUID:";
        Values[n++] = Row->ClassGuid;
        if (Row->Reference[0])
        {
            Labels[n] = L"Reference String:";
            Values[n++] = Row->Reference;
        }
        Labels[n] = L"Device Instance ID:";
        Values[n++] = Row->InstanceId;
        Labels[n] = L"Interface Status:";
        Values[n++] = Status;

        for (k = 0; k < n; k++)
        {
            _snwprintf(Line, _countof(Line) - 1, L"%-24ls%ls%ls", Labels[k], Values[k], Eol);
            Line[_countof(Line) - 1] = UNICODE_NULL;
            OutputPutsRaw(Output, Line);
        }
        OutputPutsRaw(Output, Eol);
    }
}

/* Classes tried, classes that could not be opened and the last such error. */
typedef struct _INTERFACE_STATS
{
    DWORD Tried;
    DWORD Failed;
    DWORD LastError;
} INTERFACE_STATS;

/* Enumerate the interfaces of one interface class; returns the number emitted. */
static DWORD
EnumInterfaceClass(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ const GUID *ClassGuid,
    _In_ INT State,
    _In_opt_ PCWSTR InstanceFilter,
    _Inout_ INTERFACE_ROW *Row,
    _Inout_ INTERFACE_STATS *Stats)
{
    SP_DEVICE_INTERFACE_DATA Interface;
    SP_DEVINFO_DATA DeviceInfo;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail;
    HDEVINFO Set;
    DWORD Index, Size, Count = 0, Type;
    PWSTR Ref;

    Stats->Tried++;
    Set = SetupDiGetClassDevsW(ClassGuid, NULL, NULL, DIGCF_DEVICEINTERFACE);
    if (Set == INVALID_HANDLE_VALUE)
    {
        Stats->Failed++;
        Stats->LastError = GetLastError();
        return 0;
    }

    for (Index = 0; ; Index++)
    {
        Interface.cbSize = sizeof(Interface);
        if (!SetupDiEnumDeviceInterfaces(Set, NULL, ClassGuid, Index, &Interface))
            break;

        Row->Enabled = (Interface.Flags & SPINT_ACTIVE) && !(Interface.Flags & SPINT_REMOVED);
        if ((State > 0 && !Row->Enabled) || (State < 0 && Row->Enabled))
            continue;

        Size = 0;
        SetupDiGetDeviceInterfaceDetailW(Set, &Interface, NULL, 0, &Size, NULL);
        if (Size < sizeof(*Detail))
            continue;
        Detail = HeapAlloc(GetProcessHeap(), 0, Size);
        if (!Detail)
            break;
        Detail->cbSize = sizeof(*Detail);
        DeviceInfo.cbSize = sizeof(DeviceInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(Set, &Interface, Detail, Size, NULL, &DeviceInfo))
        {
            HeapFree(GetProcessHeap(), 0, Detail);
            continue;
        }

        Row->InstanceId[0] = UNICODE_NULL;
        SetupDiGetDeviceInstanceIdW(Set, &DeviceInfo, Row->InstanceId, _countof(Row->InstanceId), NULL);
        if (InstanceFilter && _wcsicmp(InstanceFilter, Row->InstanceId) != 0)
        {
            HeapFree(GetProcessHeap(), 0, Detail);
            continue;
        }

        Row->Path = HeapAlloc(GetProcessHeap(), 0, (wcslen(Detail->DevicePath) + 1) * sizeof(WCHAR));
        if (!Row->Path)
        {
            HeapFree(GetProcessHeap(), 0, Detail);
            break;
        }
        wcscpy(Row->Path, Detail->DevicePath);
        HeapFree(GetProcessHeap(), 0, Detail);
        StringFromGUID2(ClassGuid, Row->ClassGuid, _countof(Row->ClassGuid));
        CharLowerW(Row->ClassGuid);

        /* Find "#" + this class GUID (case-insensitive); the reference follows the next backslash. */
        Row->Reference = L"";
        {
            WCHAR Needle[80];
            PWSTR Lower = HeapAlloc(GetProcessHeap(), 0, (wcslen(Row->Path) + 1) * sizeof(WCHAR));

            if (Lower)
            {
                PWSTR Found;

                wcscpy(Lower, Row->Path);
                CharLowerW(Lower);
                Needle[0] = L'#';
                lstrcpynW(Needle + 1, Row->ClassGuid, _countof(Needle) - 1);
                CharLowerW(Needle);
                /* An instance id may embed the same GUID; take the hit that ends the interface id. */
                for (Found = wcsstr(Lower, Needle); Found; Found = wcsstr(Found + 1, Needle))
                {
                    Ref = Row->Path + (Found - Lower) + wcslen(Needle);
                    if (Ref[0] == L'\\' || Ref[0] == UNICODE_NULL)
                    {
                        if (Ref[0] == L'\\')
                            Row->Reference = Ref + 1;
                        break;
                    }
                }
                HeapFree(GetProcessHeap(), 0, Lower);
            }
        }

        lstrcpynW(Row->Description, L"Unknown", _countof(Row->Description));
        {
            WCHAR Friendly[LINE_LEN];

            Size = sizeof(Friendly) - sizeof(WCHAR);
            Type = 0;
            if (SetupDiGetDeviceInterfacePropertyW(Set, &Interface, &DEVPKEY_DeviceInterface_FriendlyName,
                                                   &Type, (PBYTE)Friendly, Size, NULL, 0) &&
                Type == DEVPROP_TYPE_STRING && Friendly[0])
            {
                lstrcpynW(Row->Description, Friendly, _countof(Row->Description));
            }
        }

        EmitInterface(Output, Row);
        HeapFree(GetProcessHeap(), 0, Row->Path);
        Row->Path = NULL;
        Count++;
    }

    SetupDiDestroyDeviceInfoList(Set);
    return Count;
}

static INT
CommandEnumInterfaces(
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    OUTPUT_CONTEXT Output = { OutputText, INVALID_HANDLE_VALUE };
    WCHAR DefaultName[MAX_PATH], XmlVersion[64], XmlCommand[512], Failure[256] = L"";
    PCWSTR OutputFile = NULL, ClassFilter = NULL, InstanceFilter = NULL;
    BOOL OutputFileGiven = FALSE, WantProperties = FALSE;
    INT State = 0;
    GUID FilterGuid;
    INTERFACE_ROW *Row;
    INTERFACE_STATS Stats = { 0, 0, ERROR_SUCCESS };
    DWORD Count = 0, Error = ERROR_SUCCESS, Index;
    HKEY hClasses;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"enabled"))
        {
            State = 1;
        }
        else if (IsOption(argv[i], L"disabled"))
        {
            State = -1;
        }
        else if (IsOption(argv[i], L"properties"))
        {
            WantProperties = TRUE;
        }
        else if (IsOption(argv[i], L"class") && i + 1 < argc)
        {
            ClassFilter = argv[++i];
        }
        else if (IsOption(argv[i], L"instanceid") && i + 1 < argc)
        {
            InstanceFilter = argv[++i];
        }
        else if (IsOption(argv[i], L"format") && i + 1 < argc)
        {
            i++;
            if (_wcsicmp(argv[i], L"txt") == 0)
                Output.Format = OutputText;
            else if (_wcsicmp(argv[i], L"xml") == 0)
                Output.Format = OutputXml;
            else if (_wcsicmp(argv[i], L"csv") == 0)
                Output.Format = OutputCsv;
            else
                return ERROR_INVALID_PARAMETER;
        }
        else if (IsOption(argv[i], L"output-file"))
        {
            OutputFileGiven = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-')
                OutputFile = argv[++i];
        }
        else
        {
            return ERROR_INVALID_PARAMETER;
        }
    }

    if (OutputFileGiven)
    {
        if (!OutputFile)
        {
            BuildDefaultOutputName(Output.Format, DefaultName, _countof(DefaultName));
            OutputFile = DefaultName;
        }
        Output.File = CreateFileW(OutputFile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (Output.File == INVALID_HANDLE_VALUE)
            return ReportOutputOpenFailure(GetLastError());
    }

    if (Output.Format == OutputText || Output.File != INVALID_HANDLE_VALUE)
        PrintHeader();

    if (Output.Format == OutputCsv)
    {
        OutputPutsRaw(&Output, L"Path,Description,ClassGuid,ReferenceString,InstanceId,InterfaceStatus\r\n");
    }
    else if (Output.Format == OutputXml)
    {
        GetOutputVersion(XmlVersion, _countof(XmlVersion));
        lstrcpynW(XmlCommand, L"/enum-interfaces", _countof(XmlCommand));
        for (i = 0; i < argc; i++)
        {
            if (lstrlenW(XmlCommand) + lstrlenW(argv[i]) + 2 >= (INT)_countof(XmlCommand))
                break;
            lstrcatW(XmlCommand, L" ");
            lstrcatW(XmlCommand, argv[i]);
        }
        OutputPutsRaw(&Output, L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
        OutputPutsRaw(&Output, L"<PnpUtil Version=\"");
        OutputXmlText(&Output, XmlVersion);
        OutputPutsRaw(&Output, L"\" Command=\"");
        OutputXmlText(&Output, XmlCommand);
        OutputPutsRaw(&Output, L"\">\r\n");
    }

    Row = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Row));
    if (!Row)
        Error = ERROR_NOT_ENOUGH_MEMORY;
    else if (WantProperties)
    {
        /* TODO: /properties needs the shared property emitter (Jim2 #2b); interim error. */
        Error = ERROR_NOT_SUPPORTED;
    }
    else if (ClassFilter)
    {
        if (ClassFilter[0] != L'{' || CLSIDFromString(ClassFilter, &FilterGuid) != S_OK)
            Error = ERROR_INVALID_PARAMETER;
        else
            Count = EnumInterfaceClass(&Output, &FilterGuid, State, InstanceFilter, Row, &Stats);
    }
    else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses", 0,
                           KEY_READ, &hClasses) == ERROR_SUCCESS)
    {
        WCHAR KeyName[64];

        for (Index = 0; ; Index++)
        {
            DWORD Length = _countof(KeyName);

            if (RegEnumKeyExW(hClasses, Index, KeyName, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (CLSIDFromString(KeyName, &FilterGuid) == S_OK)
                Count += EnumInterfaceClass(&Output, &FilterGuid, State, InstanceFilter, Row, &Stats);
        }
        RegCloseKey(hClasses);
    }

    if (Error == ERROR_SUCCESS && Count == 0 && Stats.Tried != 0 && Stats.Failed == Stats.Tried)
        Error = Stats.LastError ? Stats.LastError : ERROR_GEN_FAILURE;

    if (Error != ERROR_SUCCESS)
    {
        WCHAR Text[512];
        PCWSTR Eol = Output.File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n";

        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, Error, 0,
                       Failure, _countof(Failure) - 1, NULL);
        _snwprintf(Text, _countof(Text) - 1, L"Failed to enumerate interfaces: %ls", Failure);
        Text[_countof(Text) - 1] = UNICODE_NULL;
        if (Output.Format == OutputXml)
            OutputXmlText(&Output, Text);
        else
            OutputPutsRaw(&Output, Text);
        if (!Failure[0])
            OutputPutsRaw(&Output, Eol);
        OutputPutsRaw(&Output, Eol);
    }
    else if (Count == 0)
    {
        OutputEmptyMessage(&Output, IDS_NO_INTERFACES);
    }

    if (Output.Format == OutputXml)
        OutputPutsRaw(&Output, L"</PnpUtil>\r\n");

    if (Row)
        HeapFree(GetProcessHeap(), 0, Row);

    if (Output.File != INVALID_HANDLE_VALUE)
    {
        CloseHandle(Output.File);
        Output.File = INVALID_HANDLE_VALUE;
        ConResPrintf(StdOut, IDS_OUTPUT_WRITTEN, OutputFile);
        ConPuts(StdOut, L"\n");
    }

    return (INT)Error;
}

/* /enum-devicetree *********************************************************/

#define TREE_MAX_DEPTH 96

typedef struct _TREE_IFACE
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    PWSTR Path;
    BOOL Enabled;
} TREE_IFACE;

typedef struct _TREE_CONTEXT
{
    BOOL Connected;
    BOOL Services;
    BOOL Interfaces;
    TREE_IFACE *Ifaces;
    DWORD IfaceCount;
    DWORD IfaceCapacity;
} TREE_CONTEXT;

/* Collect every device interface (all classes) once, so each node can list its own. */
static VOID
TreeCollectInterfaces(
    _Inout_ TREE_CONTEXT *Ctx)
{
    HKEY hClasses;
    WCHAR KeyName[64];
    DWORD Index;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceClasses", 0,
                      KEY_READ, &hClasses) != ERROR_SUCCESS)
    {
        return;
    }

    for (Index = 0; ; Index++)
    {
        DWORD Length = _countof(KeyName), Item, Size;
        GUID Guid;
        HDEVINFO Set;

        if (RegEnumKeyExW(hClasses, Index, KeyName, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (CLSIDFromString(KeyName, &Guid) != S_OK)
            continue;
        Set = SetupDiGetClassDevsW(&Guid, NULL, NULL, DIGCF_DEVICEINTERFACE);
        if (Set == INVALID_HANDLE_VALUE)
            continue;

        for (Item = 0; ; Item++)
        {
            SP_DEVICE_INTERFACE_DATA Interface;
            SP_DEVINFO_DATA DeviceInfo;
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W Detail;
            TREE_IFACE *Entry;

            Interface.cbSize = sizeof(Interface);
            if (!SetupDiEnumDeviceInterfaces(Set, NULL, &Guid, Item, &Interface))
                break;
            Size = 0;
            SetupDiGetDeviceInterfaceDetailW(Set, &Interface, NULL, 0, &Size, NULL);
            if (Size < sizeof(*Detail))
                continue;
            Detail = HeapAlloc(GetProcessHeap(), 0, Size);
            if (!Detail)
                break;
            Detail->cbSize = sizeof(*Detail);
            DeviceInfo.cbSize = sizeof(DeviceInfo);
            if (!SetupDiGetDeviceInterfaceDetailW(Set, &Interface, Detail, Size, NULL, &DeviceInfo))
            {
                HeapFree(GetProcessHeap(), 0, Detail);
                continue;
            }

            if (Ctx->IfaceCount == Ctx->IfaceCapacity)
            {
                DWORD Capacity = Ctx->IfaceCapacity ? Ctx->IfaceCapacity * 2 : 256;
                TREE_IFACE *Grown = Ctx->Ifaces ? HeapReAlloc(GetProcessHeap(), 0, Ctx->Ifaces, Capacity * sizeof(*Grown))
                                                : HeapAlloc(GetProcessHeap(), 0, Capacity * sizeof(*Grown));

                if (!Grown)
                {
                    HeapFree(GetProcessHeap(), 0, Detail);
                    break;
                }
                Ctx->Ifaces = Grown;
                Ctx->IfaceCapacity = Capacity;
            }
            Entry = &Ctx->Ifaces[Ctx->IfaceCount];
            Entry->InstanceId[0] = UNICODE_NULL;
            SetupDiGetDeviceInstanceIdW(Set, &DeviceInfo, Entry->InstanceId, _countof(Entry->InstanceId), NULL);
            Entry->Enabled = (Interface.Flags & SPINT_ACTIVE) && !(Interface.Flags & SPINT_REMOVED);
            Entry->Path = HeapAlloc(GetProcessHeap(), 0, (wcslen(Detail->DevicePath) + 1) * sizeof(WCHAR));
            if (Entry->Path)
            {
                wcscpy(Entry->Path, Detail->DevicePath);
                Ctx->IfaceCount++;
            }
            HeapFree(GetProcessHeap(), 0, Detail);
        }
        SetupDiDestroyDeviceInfoList(Set);
    }
    RegCloseKey(hClasses);
}

/* One property of a devnode as a heap buffer (double NUL terminated), or NULL. */
static PWSTR
TreeProperty(
    _In_ DEVINST DevInst,
    _In_ ULONG Property)
{
    ULONG Type, Size = 0;
    PWSTR Buffer;

    if (CM_Get_DevNode_Registry_PropertyW(DevInst, Property, &Type, NULL, &Size, 0) != CR_BUFFER_SMALL ||
        Size == 0 || (Type != REG_SZ && Type != REG_MULTI_SZ))
    {
        return NULL;
    }
    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + 2 * sizeof(WCHAR));
    if (!Buffer)
        return NULL;
    if (CM_Get_DevNode_Registry_PropertyW(DevInst, Property, &Type, Buffer, &Size, 0) != CR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return NULL;
    }
    return Buffer;
}

/* With /connected a node that is not present is not shown, so the tree columns are drawn
   against the visible siblings and children only. */
static BOOL
TreeVisible(
    _In_ const TREE_CONTEXT *Ctx,
    _In_ DEVINST Node)
{
    ULONG Status, Problem;

    return !Ctx->Connected || CM_Get_DevNode_Status(&Status, &Problem, Node, 0) == CR_SUCCESS;
}

static BOOL
TreeNextVisible(
    _In_ const TREE_CONTEXT *Ctx,
    _In_ DEVINST Node,
    _Out_ DEVINST *Next)
{
    DEVINST Sibling = Node;

    while (CM_Get_Sibling(&Sibling, Sibling, 0) == CR_SUCCESS)
    {
        if (TreeVisible(Ctx, Sibling))
        {
            *Next = Sibling;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL
TreeFirstVisibleChild(
    _In_ const TREE_CONTEXT *Ctx,
    _In_ DEVINST Node,
    _Out_ DEVINST *Child)
{
    if (CM_Get_Child(Child, Node, 0) != CR_SUCCESS)
        return FALSE;
    return TreeVisible(Ctx, *Child) || TreeNextVisible(Ctx, *Child, Child);
}

static VOID
TreeDetail(
    _In_ PCWSTR Prefix,
    _In_ PCWSTR Indent,
    _In_ PCWSTR Text)
{
    ConPrintf(StdOut, L"%ls%ls%ls\n", Prefix, Indent, Text);
}

static VOID
TreeNode(
    _In_ TREE_CONTEXT *Ctx,
    _In_ DEVINST Node,
    _In_ PCWSTR Prefix,
    _In_ BOOL HasNext,
    _In_ DWORD Depth)
{
    WCHAR Id[MAX_DEVICE_ID_LEN], ChildPrefix[2 * TREE_MAX_DEPTH + 4], DetailPrefix[2 * TREE_MAX_DEPTH + 8];
    WCHAR Line[2048];
    ULONG Status = 0, Problem = 0;
    PWSTR Description;
    DEVINST Child, Next;
    PCWSTR State;
    BOOL Present, HasChild;
    CONFIGRET Cr;

    Cr = CM_Get_DevNode_Status(&Status, &Problem, Node, 0);
    Present = Cr == CR_SUCCESS;
    if (Ctx->Connected && !Present)
        return;

    Id[0] = UNICODE_NULL;
    CM_Get_Device_IDW(Node, Id, _countof(Id), 0);
    Description = TreeProperty(Node, CM_DRP_FRIENDLYNAME);
    if (!Description || !Description[0])
    {
        if (Description)
            HeapFree(GetProcessHeap(), 0, Description);
        Description = TreeProperty(Node, CM_DRP_DEVICEDESC);
    }
    State = !Present ? L"Disconnected" : (Status & DN_STARTED) ? L"Started" : L"Stopped";

    _snwprintf(Line, _countof(Line) - 1, L"%ls+-%ls (%ls) [%ls]", Prefix, Id,
               Description && Description[0] ? Description : L"Unknown", State);
    Line[_countof(Line) - 1] = UNICODE_NULL;
    ConPrintf(StdOut, L"%ls\n", Line);
    if (Description)
        HeapFree(GetProcessHeap(), 0, Description);

    HasChild = TreeFirstVisibleChild(Ctx, Node, &Child);

    if (Ctx->Services || Ctx->Interfaces)
    {
        PCWSTR NextCol = HasNext ? L"| " : L"  ";
        PCWSTR ChildCol = HasChild ? L"| " : L"  ";
        WCHAR Indent[8];

        if (wcslen(Prefix) + 4 < _countof(DetailPrefix))
        {
            wcscpy(DetailPrefix, Prefix);
            wcscat(DetailPrefix, NextCol);
            wcscat(DetailPrefix, ChildCol);
        }
        else
        {
            DetailPrefix[0] = UNICODE_NULL;
        }
        wcscpy(Indent, L"");

        if (Ctx->Services)
        {
            PWSTR Service = TreeProperty(Node, CM_DRP_SERVICE);
            PWSTR Upper = TreeProperty(Node, CM_DRP_UPPERFILTERS);
            PWSTR Lower = TreeProperty(Node, CM_DRP_LOWERFILTERS);
            PCWSTR p;

            if (Service && Service[0])
            {
                _snwprintf(Line, _countof(Line) - 1, L"Service: %ls", Service);
                Line[_countof(Line) - 1] = UNICODE_NULL;
                TreeDetail(DetailPrefix, Indent, Line);
            }
            if (Upper && Upper[0])
            {
                TreeDetail(DetailPrefix, Indent, L"Upper Filters:");
                for (p = Upper; *p; p += wcslen(p) + 1)
                    TreeDetail(DetailPrefix, L"  ", p);
            }
            if (Lower && Lower[0])
            {
                TreeDetail(DetailPrefix, Indent, L"Lower Filters:");
                for (p = Lower; *p; p += wcslen(p) + 1)
                    TreeDetail(DetailPrefix, L"  ", p);
            }
            if (Service)
                HeapFree(GetProcessHeap(), 0, Service);
            if (Upper)
                HeapFree(GetProcessHeap(), 0, Upper);
            if (Lower)
                HeapFree(GetProcessHeap(), 0, Lower);
        }

        if (Ctx->Interfaces)
        {
            DWORD i;
            BOOL Header = FALSE;

            /* Win11 orders a node's interfaces by no rule found (not class key, GUID or key write time); ours follow the class key order. */
            for (i = 0; i < Ctx->IfaceCount; i++)
            {
                if (_wcsicmp(Ctx->Ifaces[i].InstanceId, Id) != 0)
                    continue;
                if (!Header)
                {
                    TreeDetail(DetailPrefix, Indent, L"Interfaces:");
                    Header = TRUE;
                }
                ConPrintf(StdOut, L"%ls  %ls [%ls]\n", DetailPrefix, Ctx->Ifaces[i].Path,
                          Ctx->Ifaces[i].Enabled ? L"Enabled" : L"Disabled");
            }
        }
    }

    if (HasChild && Depth < TREE_MAX_DEPTH)
    {
        wcscpy(ChildPrefix, Prefix);
        wcscat(ChildPrefix, HasNext ? L"| " : L"  ");
        for (;;)
        {
            HasNext = TreeNextVisible(Ctx, Child, &Next);
            TreeNode(Ctx, Child, ChildPrefix, HasNext, Depth + 1);
            if (!HasNext)
                break;
            Child = Next;
        }
    }
}

static INT
CommandEnumDeviceTree(
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    TREE_CONTEXT Ctx = { 0 };
    PCWSTR RootId = NULL;
    DEVINST Root;
    DWORD i;
    INT a;

    for (a = 0; a < argc; a++)
    {
        if (IsOption(argv[a], L"connected"))
            Ctx.Connected = TRUE;
        else if (IsOption(argv[a], L"services"))
            Ctx.Services = TRUE;
        else if (IsOption(argv[a], L"interfaces"))
            Ctx.Interfaces = TRUE;
        else if (!RootId && argv[a][0] != L'/' && argv[a][0] != L'-')
            RootId = argv[a];
        else
            return ERROR_INVALID_PARAMETER;
    }

    PrintHeader();

    if (CM_Locate_DevNodeW(&Root, (DEVINSTID_W)RootId, CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
    {
        ConPuts(StdOut, L"Failed to enumerate the device tree: The root device was not found.\n\n");
        return ERROR_SUCCESS;
    }

    if (Ctx.Interfaces)
        TreeCollectInterfaces(&Ctx);

    TreeNode(&Ctx, Root, L"", FALSE, 0);

    for (i = 0; i < Ctx.IfaceCount; i++)
        HeapFree(GetProcessHeap(), 0, Ctx.Ifaces[i].Path);
    if (Ctx.Ifaces)
        HeapFree(GetProcessHeap(), 0, Ctx.Ifaces);
    return ERROR_SUCCESS;
}

/* /enum-containers *********************************************************/

typedef struct _CONT_DEVICE
{
    WCHAR Id[MAX_DEVICE_ID_LEN];
    WCHAR Description[LINE_LEN];
    GUID Container;
    PCWSTR State;
    BOOL Present;
    BOOL Problem;
} CONT_DEVICE;

typedef struct _CONT_ENTRY
{
    GUID Id;
    BOOL Connected;
    BOOL HasProblem;
} CONT_ENTRY;

/* Container metadata (description, categories, manufacturer, model) is not available through
   our cfgmgr32 (DevObjectTypeDeviceContainer is unsupported); only membership and state are. */
static VOID
ContainerEmit(
    _In_ OUTPUT_CONTEXT *Output,
    _In_ const CONT_ENTRY *Entry,
    _In_ const CONT_DEVICE *Devices,
    _In_ DWORD DeviceCount,
    _In_ BOOL ShowDevices)
{
    WCHAR IdText[64], Line[2048];
    PCWSTR Status = Entry->Connected ? L"Connected" : L"Disconnected";
    PCWSTR Eol = Output->File != INVALID_HANDLE_VALUE ? L"\r\n" : L"\n";
    DWORD i;
    BOOL Opened = FALSE;

    StringFromGUID2(&Entry->Id, IdText, _countof(IdText));
    CharLowerW(IdText);

    if (Output->Format == OutputCsv)
    {
        OutputCsvField(Output, IdText, FALSE);
        OutputCsvField(Output, L"", FALSE);
        OutputCsvField(Output, L"", FALSE);
        OutputCsvField(Output, L"", FALSE);
        OutputCsvField(Output, L"", FALSE);
        OutputCsvField(Output, L"", FALSE);
        OutputCsvField(Output, Status, TRUE);
        if (!ShowDevices)
            return;
        for (i = 0; i < DeviceCount; i++)
        {
            if (!IsEqualGUID(&Devices[i].Container, &Entry->Id))
                continue;
            OutputCsvField(Output, IdText, FALSE);
            OutputCsvField(Output, L"", FALSE);
            OutputCsvField(Output, L"", FALSE);
            OutputCsvField(Output, L"", FALSE);
            OutputCsvField(Output, L"", FALSE);
            OutputCsvField(Output, L"", FALSE);
            OutputCsvField(Output, L"", FALSE);
            OutputCsvField(Output, Devices[i].Id, FALSE);
            OutputCsvField(Output, Devices[i].Description, FALSE);
            OutputCsvField(Output, Devices[i].State, FALSE);
            OutputCsvField(Output, L"", FALSE);
            OutputCsvField(Output, L"", TRUE);
        }
    }
    else if (Output->Format == OutputXml)
    {
        OutputPutsRaw(Output, L"    <Container ContainerId=\"");
        OutputXmlText(Output, IdText);
        OutputPutsRaw(Output, L"\">\r\n        <Status>");
        OutputXmlText(Output, Status);
        OutputPutsRaw(Output, L"</Status>\r\n");
        if (ShowDevices)
        {
            for (i = 0; i < DeviceCount; i++)
            {
                if (!IsEqualGUID(&Devices[i].Container, &Entry->Id))
                    continue;
                if (!Opened)
                {
                    OutputPutsRaw(Output, L"        <Devices>\r\n");
                    Opened = TRUE;
                }
                OutputPutsRaw(Output, L"            <Device InstanceId=\"");
                OutputXmlText(Output, Devices[i].Id);
                OutputPutsRaw(Output, L"\">\r\n                <DeviceDescription>");
                OutputXmlText(Output, Devices[i].Description);
                OutputPutsRaw(Output, L"</DeviceDescription>\r\n                <Status>");
                OutputXmlText(Output, Devices[i].State);
                OutputPutsRaw(Output, L"</Status>\r\n            </Device>\r\n");
            }
            if (Opened)
                OutputPutsRaw(Output, L"        </Devices>\r\n");
        }
        OutputPutsRaw(Output, L"    </Container>\r\n");
    }
    else
    {
        _snwprintf(Line, _countof(Line) - 1, L"%-24ls%ls%ls", L"Container ID:", IdText, Eol);
        Line[_countof(Line) - 1] = UNICODE_NULL;
        OutputPutsRaw(Output, Line);
        _snwprintf(Line, _countof(Line) - 1, L"%-24ls%ls%ls", L"Status:", Status, Eol);
        Line[_countof(Line) - 1] = UNICODE_NULL;
        OutputPutsRaw(Output, Line);
        if (ShowDevices)
        {
            OutputPutsRaw(Output, L"Devices:");
            OutputPutsRaw(Output, Eol);
            for (i = 0; i < DeviceCount; i++)
            {
                if (!IsEqualGUID(&Devices[i].Container, &Entry->Id))
                    continue;
                _snwprintf(Line, _countof(Line) - 1, L"    %-20ls%ls%ls", L"Instance ID:", Devices[i].Id, Eol);
                Line[_countof(Line) - 1] = UNICODE_NULL;
                OutputPutsRaw(Output, Line);
                _snwprintf(Line, _countof(Line) - 1, L"    %-20ls%ls%ls", L"Device Description:",
                           Devices[i].Description, Eol);
                Line[_countof(Line) - 1] = UNICODE_NULL;
                OutputPutsRaw(Output, Line);
                _snwprintf(Line, _countof(Line) - 1, L"    %-20ls%ls%ls", L"Status:", Devices[i].State, Eol);
                Line[_countof(Line) - 1] = UNICODE_NULL;
                OutputPutsRaw(Output, Line);
                OutputPutsRaw(Output, Eol);
            }
        }
        else
        {
            OutputPutsRaw(Output, Eol);
        }
    }
}

static INT
CommandEnumContainers(
    _In_ INT argc,
    _In_ WCHAR **argv)
{
    OUTPUT_CONTEXT Output = { OutputText, INVALID_HANDLE_VALUE };
    WCHAR DefaultName[MAX_PATH], XmlVersion[64], XmlCommand[512];
    PCWSTR OutputFile = NULL, IdFilter = NULL;
    BOOL OutputFileGiven = FALSE, ShowDevices = FALSE, OnlyProblem = FALSE;
    INT State = 0;
    GUID FilterGuid;
    BOOL FilterIsGuid = FALSE;
    CONT_DEVICE *Devices = NULL;
    CONT_ENTRY *Entries = NULL;
    DWORD DeviceCount = 0, DeviceCapacity = 0, EntryCount = 0, Index, Shown = 0, k;
    HDEVINFO Set;
    INT i;

    for (i = 0; i < argc; i++)
    {
        if (IsOption(argv[i], L"connected"))
            State = 1;
        else if (IsOption(argv[i], L"disconnected"))
            State = -1;
        else if (IsOption(argv[i], L"problem"))
            OnlyProblem = TRUE;
        else if (IsOption(argv[i], L"devices"))
            ShowDevices = TRUE;
        else if (IsOption(argv[i], L"containerid") && i + 1 < argc)
            IdFilter = argv[++i];
        else if (IsOption(argv[i], L"format") && i + 1 < argc)
        {
            i++;
            if (_wcsicmp(argv[i], L"txt") == 0)
                Output.Format = OutputText;
            else if (_wcsicmp(argv[i], L"xml") == 0)
                Output.Format = OutputXml;
            else if (_wcsicmp(argv[i], L"csv") == 0)
                Output.Format = OutputCsv;
            else
                return ERROR_INVALID_PARAMETER;
        }
        else if (IsOption(argv[i], L"output-file"))
        {
            OutputFileGiven = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-')
                OutputFile = argv[++i];
        }
        else
        {
            return ERROR_INVALID_PARAMETER;
        }
    }

    if (OutputFileGiven)
    {
        if (!OutputFile)
        {
            BuildDefaultOutputName(Output.Format, DefaultName, _countof(DefaultName));
            OutputFile = DefaultName;
        }
        Output.File = CreateFileW(OutputFile, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (Output.File == INVALID_HANDLE_VALUE)
            return ReportOutputOpenFailure(GetLastError());
    }

    if (Output.Format == OutputText || Output.File != INVALID_HANDLE_VALUE)
        PrintHeader();

    if (Output.Format == OutputCsv)
    {
        OutputPutsRaw(&Output, L"ContainerId,Description,Categories,Manufacturer,ModelName,ModelNumber,Status");
        if (ShowDevices)
            OutputPutsRaw(&Output, L",Device,DeviceDescription,DeviceStatus,ProblemCode,ProblemStatus");
        OutputPutsRaw(&Output, L"\r\n");
    }
    else if (Output.Format == OutputXml)
    {
        GetOutputVersion(XmlVersion, _countof(XmlVersion));
        lstrcpynW(XmlCommand, L"/enum-containers", _countof(XmlCommand));
        for (i = 0; i < argc; i++)
        {
            if (lstrlenW(XmlCommand) + lstrlenW(argv[i]) + 2 >= (INT)_countof(XmlCommand))
                break;
            lstrcatW(XmlCommand, L" ");
            lstrcatW(XmlCommand, argv[i]);
        }
        OutputPutsRaw(&Output, L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
        OutputPutsRaw(&Output, L"<PnpUtil Version=\"");
        OutputXmlText(&Output, XmlVersion);
        OutputPutsRaw(&Output, L"\" Command=\"");
        OutputXmlText(&Output, XmlCommand);
        OutputPutsRaw(&Output, L"\">\r\n");
    }

    if (IdFilter && IdFilter[0] == L'{' && CLSIDFromString(IdFilter, &FilterGuid) == S_OK)
        FilterIsGuid = TRUE;

    /* Every device (present or not) with its container id. */
    Set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (Set != INVALID_HANDLE_VALUE)
    {
        SP_DEVINFO_DATA Info;

        for (Index = 0; ; Index++)
        {
            CONT_DEVICE *Dev;
            DEVPROPTYPE Type;
            DEVINST Node;
            ULONG Status, Problem;
            WCHAR Buffer[LINE_LEN];
            GUID Container;

            Info.cbSize = sizeof(Info);
            if (!SetupDiEnumDeviceInfo(Set, Index, &Info))
                break;
            if (!SetupDiGetDevicePropertyW(Set, &Info, &DEVPKEY_Device_ContainerId, &Type, (PBYTE)&Container,
                                           sizeof(Container), NULL, 0) ||
                Type != DEVPROP_TYPE_GUID)
            {
                continue;
            }
            if (DeviceCount == DeviceCapacity)
            {
                DWORD Capacity = DeviceCapacity ? DeviceCapacity * 2 : 256;
                CONT_DEVICE *Grown = Devices ? HeapReAlloc(GetProcessHeap(), 0, Devices, Capacity * sizeof(*Grown))
                                             : HeapAlloc(GetProcessHeap(), 0, Capacity * sizeof(*Grown));

                if (!Grown)
                    break;
                Devices = Grown;
                DeviceCapacity = Capacity;
            }
            Dev = &Devices[DeviceCount];
            Dev->Container = Container;
            Dev->Id[0] = UNICODE_NULL;
            SetupDiGetDeviceInstanceIdW(Set, &Info, Dev->Id, _countof(Dev->Id), NULL);
            Dev->Description[0] = UNICODE_NULL;
            if (!SetupDiGetDeviceRegistryPropertyW(Set, &Info, SPDRP_FRIENDLYNAME, NULL, (PBYTE)Buffer,
                                                   sizeof(Buffer) - sizeof(WCHAR), NULL) || !Buffer[0])
            {
                if (!SetupDiGetDeviceRegistryPropertyW(Set, &Info, SPDRP_DEVICEDESC, NULL, (PBYTE)Buffer,
                                                       sizeof(Buffer) - sizeof(WCHAR), NULL))
                {
                    Buffer[0] = UNICODE_NULL;
                }
            }
            lstrcpynW(Dev->Description, Buffer[0] ? Buffer : L"Unknown", _countof(Dev->Description));

            Status = Problem = 0;
            Dev->Present = FALSE;
            Dev->Problem = FALSE;
            if (CM_Locate_DevNodeW(&Node, Dev->Id, CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS &&
                CM_Get_DevNode_Status(&Status, &Problem, Node, 0) == CR_SUCCESS)
            {
                Dev->Present = TRUE;
                Dev->Problem = Problem != 0;
            }
            Dev->State = !Dev->Present ? L"Disconnected" : (Status & DN_STARTED) ? L"Started" : L"Stopped";
            DeviceCount++;
        }
        SetupDiDestroyDeviceInfoList(Set);
    }

    /* Group by container, in order of first appearance. */
    if (DeviceCount)
        Entries = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DeviceCount * sizeof(*Entries));
    for (Index = 0; Entries && Index < DeviceCount; Index++)
    {
        for (k = 0; k < EntryCount; k++)
        {
            if (IsEqualGUID(&Entries[k].Id, &Devices[Index].Container))
                break;
        }
        if (k == EntryCount)
        {
            Entries[k].Id = Devices[Index].Container;
            EntryCount++;
        }
        if (Devices[Index].Present)
            Entries[k].Connected = TRUE;
        if (Devices[Index].Problem)
            Entries[k].HasProblem = TRUE;
    }

    {
        BOOL IdMatched = FALSE;

        for (k = 0; k < EntryCount; k++)
        {
            if (IdFilter)
            {
                if (!FilterIsGuid || !IsEqualGUID(&Entries[k].Id, &FilterGuid))
                    continue;
                IdMatched = TRUE;
            }
            if ((State > 0 && !Entries[k].Connected) || (State < 0 && Entries[k].Connected))
                continue;
            if (OnlyProblem && !Entries[k].HasProblem)
                continue;
            ContainerEmit(&Output, &Entries[k], Devices, DeviceCount, ShowDevices);
            Shown++;
        }

        if (Shown == 0 && IdFilter && !IdMatched)
        {
            /* Windows prints nothing for an unknown container id and returns the not-found error. */
        }
        else if (Shown == 0)
        {
            OutputEmptyMessage(&Output, IDS_NO_CONTAINERS);
        }

        if (Output.Format == OutputXml)
            OutputPutsRaw(&Output, L"</PnpUtil>\r\n");
        if (Output.File != INVALID_HANDLE_VALUE)
        {
            CloseHandle(Output.File);
            Output.File = INVALID_HANDLE_VALUE;
            ConResPrintf(StdOut, IDS_OUTPUT_WRITTEN, OutputFile);
            ConPuts(StdOut, L"\n");
        }
        if (Entries)
            HeapFree(GetProcessHeap(), 0, Entries);
        if (Devices)
            HeapFree(GetProcessHeap(), 0, Devices);
        return (Shown == 0 && IdFilter && !IdMatched) ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
    }
}

int
wmain(
    int argc,
    WCHAR **argv)
{
    PCWSTR Command;
    INT Result;

    ConInitStdStreams();

    if (argc < 2 || IsOption(argv[1], L"?") || IsOption(argv[1], L"h") || IsOption(argv[1], L"help"))
    {
        ConPuts(StdOut, UsageText);
        return ERROR_SUCCESS;
    }

    Command = argv[1];
    argc -= 2;
    argv += 2;

    if (IsOption(Command, L"add-driver") || IsOption(Command, L"a"))
    {
        Result = CommandAddDriver(argc, argv, FALSE);
    }
    else if (IsOption(Command, L"i") && argc >= 1 && IsOption(argv[0], L"a"))
    {
        Result = CommandAddDriver(argc - 1, argv + 1, TRUE);
    }
    else if (IsOption(Command, L"delete-driver") || IsOption(Command, L"d"))
    {
        Result = CommandDeleteDriver(argc, argv, FALSE);
    }
    else if (IsOption(Command, L"f") && argc >= 1 && IsOption(argv[0], L"d"))
    {
        Result = CommandDeleteDriver(argc - 1, argv + 1, TRUE);
    }
    else if (IsOption(Command, L"enum-containers"))
    {
        Result = CommandEnumContainers(argc, argv);
    }
    else if (IsOption(Command, L"enum-devicetree"))
    {
        Result = CommandEnumDeviceTree(argc, argv);
    }
    else if (IsOption(Command, L"enum-interfaces"))
    {
        Result = CommandEnumInterfaces(argc, argv);
    }
    else if (IsOption(Command, L"enum-classes"))
    {
        Result = CommandEnumClasses(argc, argv);
    }
    else if (IsOption(Command, L"export-driver"))
    {
        Result = CommandExportDriver(argc, argv);
    }
    else if (IsOption(Command, L"enum-drivers") || IsOption(Command, L"e"))
    {
        Result = CommandEnumDrivers(argc, argv);
    }
    else if (IsOption(Command, L"enum-devices"))
    {
        Result = CommandEnumDevices(argc, argv);
    }
    else if (IsOption(Command, L"scan-devices"))
    {
        Result = CommandScanDevices();
    }
    else if (IsOption(Command, L"rollback-device"))
    {
        Result = CommandRollbackDevice(argc, argv);
    }
    else if (IsOption(Command, L"restart-device"))
    {
        Result = CommandDevice(DIF_PROPERTYCHANGE, DICS_PROPCHANGE,
                               L"Restarting device:         ", L"Device restarted successfully.",
                               L"Failed to restart device", argc, argv);
    }
    else if (IsOption(Command, L"disable-device"))
    {
        Result = CommandDevice(DIF_PROPERTYCHANGE, DICS_DISABLE,
                               L"Disabling device:          ", L"Device disabled successfully.",
                               L"Failed to disable device", argc, argv);
    }
    else if (IsOption(Command, L"enable-device"))
    {
        Result = CommandDevice(DIF_PROPERTYCHANGE, DICS_ENABLE,
                               L"Enabling device:          ", L"Device enabled successfully.",
                               L"Failed to enable device", argc, argv);
    }
    else if (IsOption(Command, L"remove-device"))
    {
        Result = CommandDevice(DIF_REMOVE, 0,
                               L"Removing device:          ", L"Device removed successfully.",
                               L"Failed to remove device", argc, argv);
    }
    else
    {
        Result = ERROR_INVALID_PARAMETER;
    }

    if (Result == ERROR_INVALID_PARAMETER)
    {
        ConPuts(StdOut, UsageText);
        Result = 1;
    }

    return Result;
}
