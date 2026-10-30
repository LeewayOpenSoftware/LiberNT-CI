/*
 * PROJECT:     LiberNT Plug and Play Manager
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Driver install snapshots and rollback of the device state
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* A snapshot copies the registry keys that describe which driver a device uses:
 * the devnode (Enum\<id>), the class driver key it points to (Class\<guid>\<n>)
 * and the service key (Services\<svc>). Restoring makes the real keys identical
 * to the saved copies, deletes what the failed install added, and re-runs the
 * start so the previous driver binds again. The copies live next to the DrvInst
 * policy in the registry because they are registry metadata; keeping them under
 * Control\DrvInst\Snapshots\<encoded id>\<seq> also survives a service restart,
 * which the in-memory job table does not. */

#define SNAP_VAL_HADDRIVER    L"HadDriver"
#define SNAP_VAL_CLASSKEYPATH L"ClassKeyPath"
#define SNAP_VAL_SERVICENAME  L"ServiceName"
#define SNAP_VAL_DEVICENAME   L"DeviceInstanceId"

#define SNAP_KEY_DEVNODE      L"DevNode"
#define SNAP_KEY_CLASSKEY     L"ClassKey"
#define SNAP_KEY_SERVICE      L"Service"

#define ENUM_KEY              L"SYSTEM\\CurrentControlSet\\Enum\\"
#define CLASS_KEY             L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
#define SERVICES_KEY          L"SYSTEM\\CurrentControlSet\\Services\\"

static LONG
OpenPath(
    _In_ HKEY hRoot,
    _In_ PCWSTR Path,
    _In_ REGSAM Sam,
    _Out_ PHKEY phKey);

/* One snapshot sequence counter for the whole service; ExecuteJob serializes
 * installs, but the counter also feeds future manual paths. */
static CRITICAL_SECTION SnapshotLock;

VOID
DrvInstRollbackInitialize(VOID)
{
    InitializeCriticalSection(&SnapshotLock);
}

/* The device id is used as a single registry key name: encode its separators. */
static VOID
EncodeDeviceId(
    _In_ PCWSTR DeviceInstanceId,
    _Out_writes_(DRVINST_DEVICE_ID_CCH) PWSTR Encoded)
{
    for (; *DeviceInstanceId; DeviceInstanceId++)
    {
        *Encoded++ = (*DeviceInstanceId == L'\\') ? L'!' : *DeviceInstanceId;
    }
    *Encoded = UNICODE_NULL;
}

/* TRUE when another devnode under Enum names the same service. */
static BOOL
IsServiceReferencedElsewhere(
    _In_ PCWSTR ServiceName,
    _In_ PCWSTR ExcludeDeviceId)
{
    static const WCHAR EnumPath[] = L"SYSTEM\\CurrentControlSet\\Enum";
    WCHAR EnumName[64], DevName[256], InstName[DRVINST_DEVICE_ID_CCH];
    WCHAR FullId[768], Service[128];
    HKEY hEnum = NULL, hDev = NULL, hInst = NULL;
    DWORD i, j, k, Size, ServiceSize;
    BOOL Referenced = FALSE;

    if (OpenPath(HKEY_LOCAL_MACHINE, EnumPath, KEY_READ, &hEnum) != ERROR_SUCCESS)
        return FALSE;

    for (i = 0; !Referenced; i++)
    {
        Size = RTL_NUMBER_OF(EnumName);
        if (RegEnumKeyExW(hEnum, i, EnumName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        if (OpenPath(hEnum, EnumName, KEY_READ, &hDev) != ERROR_SUCCESS)
            continue;
        for (j = 0; !Referenced; j++)
        {
            Size = RTL_NUMBER_OF(DevName);
            if (RegEnumKeyExW(hDev, j, DevName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                break;
            if (OpenPath(hDev, DevName, KEY_READ, &hInst) != ERROR_SUCCESS)
                continue;
            for (k = 0; !Referenced; k++)
            {
                Size = RTL_NUMBER_OF(InstName);
                if (RegEnumKeyExW(hInst, k, InstName, &Size, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;
                _snwprintf(FullId, RTL_NUMBER_OF(FullId), L"%s\\%s\\%s", EnumName, DevName, InstName);
                FullId[RTL_NUMBER_OF(FullId) - 1] = UNICODE_NULL;
                if (!lstrcmpW(FullId, ExcludeDeviceId))
                    continue;
                ServiceSize = sizeof(Service);
                {
                    LONG SvcError = RegGetValueW(hInst, InstName, L"Service", RRF_RT_REG_SZ, NULL,
                                                 Service, &ServiceSize);
                    if (SvcError == ERROR_SUCCESS && !lstrcmpW(Service, ServiceName))
                        Referenced = TRUE;
                    else if (SvcError != ERROR_FILE_NOT_FOUND && SvcError != ERROR_SUCCESS)
                        Referenced = TRUE; /* a name we cannot read may be the shared one */
                }
            }
            RegCloseKey(hInst);
            hInst = NULL;
        }
        RegCloseKey(hDev);
        hDev = NULL;
    }

    if (hEnum)
        RegCloseKey(hEnum);
    return Referenced;
}

/* A service or key leaf name taken from the registry must not be able to
 * redirect a composed path before it is deleted. */
static BOOL
IsSafeKeyName(
    _In_ PCWSTR Name)
{
    static const WCHAR Allowed[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.";
    PCWSTR p;

    if (!Name[0])
        return FALSE;
    for (; *Name; Name++)
    {
        for (p = Allowed; *p; p++)
        {
            if (*p == *Name)
                break;
        }
        if (!*p)
            return FALSE;
    }
    return TRUE;
}

static LONG
OpenPath(
    _In_ HKEY hRoot,
    _In_ PCWSTR Path,
    _In_ REGSAM Sam,
    _Out_ PHKEY phKey)
{
    return RegOpenKeyExW(hRoot, Path, 0, Sam, phKey);
}

typedef struct _NAME_LIST
{
    PWSTR Names;
    SIZE_T Count;
    SIZE_T Capacity;
    SIZE_T Stride;
} NAME_LIST, *PNAME_LIST;

static VOID
NameListFree(
    _Inout_ PNAME_LIST List)
{
    HeapFree(GetProcessHeap(), 0, List->Names);
    List->Names = NULL;
    List->Count = 0;
    List->Capacity = 0;
    List->Stride = 0;
}

/* Enumerates the value names or subkey names of hKey into a stable list, so
 * entries can be deleted while iterating over the list. */
static LONG
NameListCollect(
    _In_ HKEY hKey,
    _In_ BOOL Values,
    _Out_ PNAME_LIST List)
{
    DWORD Count, MaxLen, i, Size;
    WCHAR StackBuffer[64];
    PWSTR Buffer = StackBuffer;
    DWORD BufferCch = (DWORD)RTL_NUMBER_OF(StackBuffer);
    LONG Error;

    Error = Values
        ? RegQueryInfoKeyW(hKey, NULL, NULL, NULL, NULL, NULL, NULL, &Count, &MaxLen, NULL, NULL, NULL)
        : RegQueryInfoKeyW(hKey, NULL, NULL, NULL, &Count, &MaxLen, NULL, NULL, NULL, NULL, NULL, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (MaxLen + 1 > BufferCch)
    {
        Buffer = HeapAlloc(GetProcessHeap(), 0, (MaxLen + 1) * sizeof(WCHAR));
        if (!Buffer)
            return ERROR_NOT_ENOUGH_MEMORY;
        BufferCch = MaxLen + 1;
    }

    List->Capacity = (Count + 1) * BufferCch;
    List->Stride = BufferCch;
    List->Names = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, List->Capacity * sizeof(WCHAR));
    if (!List->Names)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Cleanup;
    }

    for (i = 0; i < Count; i++)
    {
        PWSTR Next;

        Size = BufferCch;
        Error = Values ? RegEnumValueW(hKey, i, Buffer, &Size, NULL, NULL, NULL, NULL)
                       : RegEnumKeyExW(hKey, i, Buffer, &Size, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;

        Next = List->Names + List->Count * List->Stride;
        if ((List->Count + 1) * List->Stride >= List->Capacity)
        {
            Error = ERROR_BUFFER_OVERFLOW;
            break;
        }
        lstrcpynW(Next, Buffer, (int)List->Stride);
        List->Count++;
    }

Cleanup:
    if (Buffer != StackBuffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    if (Error != ERROR_SUCCESS)
        NameListFree(List);
    return Error;
}

static BOOL
NameListHas(
    _In_ PNAME_LIST List,
    _In_ PCWSTR Name)
{
    SIZE_T i;

    if (!List->Names || List->Count == 0)
        return FALSE;
    for (i = 0; i < List->Count; i++)
    {
        if (!lstrcmpW(List->Names + i * List->Stride, Name))
            return TRUE;
    }
    return FALSE;
}

/* Copies every value and subkey of hSrc into the already open hDst. */
static LONG
CopyTreeIntoKey(
    _In_ HKEY hSrc,
    _In_ HKEY hDst)
{
    WCHAR ValueName[256];
    WCHAR KeyName[256];
    PBYTE Data = NULL;
    DWORD ValueCount, MaxValueLen, MaxDataLen;
    DWORD KeyCount, MaxKeyLen, Type, Size, i;
    HKEY hSrcSub = NULL, hDstSub = NULL;
    LONG Error;

    Error = RegQueryInfoKeyW(hSrc, NULL, NULL, NULL, &KeyCount, &MaxKeyLen, NULL,
                             &ValueCount, &MaxValueLen, &MaxDataLen, NULL, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;
    if (MaxValueLen >= RTL_NUMBER_OF(ValueName) || MaxKeyLen >= RTL_NUMBER_OF(KeyName))
        return ERROR_BUFFER_OVERFLOW;

    for (i = 0; i < ValueCount; i++)
    {
        Size = RTL_NUMBER_OF(ValueName);
        Error = RegEnumValueW(hSrc, i, ValueName, &Size, NULL, &Type, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            return Error;

        if (!Data)
        {
            Data = HeapAlloc(GetProcessHeap(), 0, MaxDataLen ? MaxDataLen : 1);
            if (!Data)
                return ERROR_NOT_ENOUGH_MEMORY;
        }

        Size = MaxDataLen;
        Error = RegQueryValueExW(hSrc, ValueName, NULL, &Type, Data, &Size);
        if (Error == ERROR_SUCCESS)
            Error = RegSetValueExW(hDst, ValueName, 0, Type, Data, Size);
        if (Error != ERROR_SUCCESS)
            break;
    }
    HeapFree(GetProcessHeap(), 0, Data);

    for (i = 0; i < KeyCount && Error == ERROR_SUCCESS; i++)
    {
        Size = RTL_NUMBER_OF(KeyName);
        Error = RegEnumKeyExW(hSrc, i, KeyName, &Size, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;

        Error = OpenPath(hSrc, KeyName, KEY_READ, &hSrcSub);
        if (Error != ERROR_SUCCESS)
            break;
        Error = RegCreateKeyExW(hDst, KeyName, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE, NULL, &hDstSub, NULL);
        if (Error == ERROR_SUCCESS)
        {
            Error = CopyTreeIntoKey(hSrcSub, hDstSub);
            RegCloseKey(hDstSub);
        }
        RegCloseKey(hSrcSub);
    }

    return Error;
}

/* Makes the real key identical to the saved snapshot: values and subkeys the
 * snapshot does not have are deleted, the rest is overwritten. */
static LONG
SyncKeyWithSnapshot(
    _In_ HKEY hSnap,
    _In_ HKEY hReal)
{
    NAME_LIST SnapValues = {0}, RealValues = {0}, SnapKeys = {0}, RealKeys = {0};
    DWORD ValueCount, i, Type, Size;
    HKEY hSnapSub = NULL, hRealSub = NULL;
    LONG Error;

    Error = RegQueryInfoKeyW(hSnap, NULL, NULL, NULL, NULL, NULL, NULL,
                             &ValueCount, NULL, NULL, NULL, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = NameListCollect(hSnap, TRUE, &SnapValues);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = NameListCollect(hReal, TRUE, &RealValues);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = NameListCollect(hSnap, FALSE, &SnapKeys);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = NameListCollect(hReal, FALSE, &RealKeys);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* Overwrite with every value the snapshot has. */
    for (i = 0; i < SnapValues.Count; i++)
    {
        PCWSTR Name = SnapValues.Names + i * SnapValues.Stride;
        PBYTE Data;

        Size = 0;
        RegQueryValueExW(hSnap, Name, NULL, &Type, NULL, &Size);
        Data = HeapAlloc(GetProcessHeap(), 0, Size ? Size : 1);
        if (!Data)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            goto Cleanup;
        }
        Error = RegQueryValueExW(hSnap, Name, NULL, &Type, Data, &Size);
        if (Error == ERROR_SUCCESS)
            Error = RegSetValueExW(hReal, Name, 0, Type, Data, Size);
        HeapFree(GetProcessHeap(), 0, Data);
        if (Error != ERROR_SUCCESS)
            goto Cleanup;
    }

    /* Delete the values only the failed install added. */
    for (i = 0; i < RealValues.Count; i++)
    {
        PCWSTR Name = RealValues.Names + i * RealValues.Stride;

        if (!NameListHas(&SnapValues, Name))
        {
            Error = RegDeleteValueW(hReal, Name);
            if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
                goto Cleanup;
        }
    }

    /* Recurse into the subkeys the snapshot has (create + sync). */
    for (i = 0; i < SnapKeys.Count; i++)
    {
        PCWSTR Name = SnapKeys.Names + i * SnapKeys.Stride;

        Error = OpenPath(hSnap, Name, KEY_READ, &hSnapSub);
        if (Error != ERROR_SUCCESS)
            goto Cleanup;
        Error = RegCreateKeyExW(hReal, Name, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE | KEY_READ, NULL, &hRealSub, NULL);
        if (Error == ERROR_SUCCESS)
        {
            Error = SyncKeyWithSnapshot(hSnapSub, hRealSub);
            RegCloseKey(hRealSub);
        }
        RegCloseKey(hSnapSub);
        hSnapSub = NULL;
        if (Error != ERROR_SUCCESS)
            goto Cleanup;
    }

    /* Delete the subkeys only the failed install added. */
    for (i = 0; i < RealKeys.Count; i++)
    {
        PCWSTR Name = RealKeys.Names + i * RealKeys.Stride;

        if (!NameListHas(&SnapKeys, Name))
        {
            Error = RegDeleteTreeW(hReal, Name);
            if (Error != ERROR_SUCCESS && Error != ERROR_FILE_NOT_FOUND)
                goto Cleanup;
        }
    }

    Error = ERROR_SUCCESS;

Cleanup:
    NameListFree(&SnapValues);
    NameListFree(&RealValues);
    NameListFree(&SnapKeys);
    NameListFree(&RealKeys);
    if (hSnapSub)
        RegCloseKey(hSnapSub);
    if (hRealSub)
        RegCloseKey(hRealSub);
    UNREFERENCED_PARAMETER(ValueCount);
    return Error;
}

/* Deletes every snapshot of the device except the Newest two. */
static VOID
PruneOldSnapshots(
    _In_ HKEY hDeviceKey)
{
    NAME_LIST Keys = {0};
    ULONG Newest[2] = {0, 0};
    SIZE_T i;
    LONG Error;

    Error = NameListCollect(hDeviceKey, FALSE, &Keys);
    if (Error != ERROR_SUCCESS)
        return;
    if (Keys.Count == 0)
        goto Cleanup;

    for (i = 0; i < Keys.Count; i++)
    {
        ULONG Seq = wcstoul(Keys.Names + i * Keys.Stride, NULL, 10);
        if (Seq >= Newest[0])
        {
            Newest[1] = Newest[0];
            Newest[0] = Seq;
        }
        else if (Seq > Newest[1])
        {
            Newest[1] = Seq;
        }
    }

    for (i = 0; i < Keys.Count; i++)
    {
        ULONG Seq = wcstoul(Keys.Names + i * Keys.Stride, NULL, 10);
        if (Seq != Newest[0] && Seq != Newest[1])
            RegDeleteTreeW(hDeviceKey, Keys.Names + i * Keys.Stride);
    }

Cleanup:
    NameListFree(&Keys);
}

/* Saves the devnode, its class driver key and its service key. */
DWORD
DrvInstSnapshotDevice(
    _In_ PCWSTR DeviceInstanceId,
    _Out_opt_ PULONG OutSeq)
{
    WCHAR Encoded[DRVINST_DEVICE_ID_CCH];
    WCHAR Path[512];
    WCHAR ValueBuffer[256];
    HKEY hSnapshots = NULL, hDeviceKey = NULL, hSeqKey = NULL;
    HKEY hSrc = NULL, hDst = NULL;
    DWORD Seq = 0, Size, HadDriver = 0;
    LONG Error;

    if (OutSeq)
        *OutSeq = 0;

    EncodeDeviceId(DeviceInstanceId, Encoded);

    EnterCriticalSection(&SnapshotLock);

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, DRVINST_REG_SNAPSHOTS, 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hSnapshots, NULL);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    Size = sizeof(Seq);
    if (RegQueryValueExW(hSnapshots, L"NextSeq", NULL, NULL, (PBYTE)&Seq, &Size) != ERROR_SUCCESS || Size != sizeof(Seq))
        Seq = 0;
    Seq++;
    RegSetValueExW(hSnapshots, L"NextSeq", 0, REG_DWORD, (PBYTE)&Seq, sizeof(Seq));

    Error = RegCreateKeyExW(hSnapshots, Encoded, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_ALL_ACCESS, NULL, &hDeviceKey, NULL);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    {
        WCHAR SeqName[16];
        _snwprintf(SeqName, RTL_NUMBER_OF(SeqName), L"%lu", Seq);
        SeqName[RTL_NUMBER_OF(SeqName) - 1] = UNICODE_NULL;
        Error = RegCreateKeyExW(hDeviceKey, SeqName, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_ALL_ACCESS, NULL, &hSeqKey, NULL);
    }
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    RegSetValueExW(hSeqKey, SNAP_VAL_DEVICENAME, 0, REG_SZ,
                   (PBYTE)DeviceInstanceId, (lstrlenW(DeviceInstanceId) + 1) * sizeof(WCHAR));

    _snwprintf(Path, RTL_NUMBER_OF(Path), L"%s%s", ENUM_KEY, DeviceInstanceId);
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;

    /* 1. The devnode subtree. */
    Error = OpenPath(HKEY_LOCAL_MACHINE, Path, KEY_READ, &hSrc);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = RegCreateKeyExW(hSeqKey, SNAP_KEY_DEVNODE, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_WRITE, NULL, &hDst, NULL);
    if (Error == ERROR_SUCCESS)
    {
        Error = CopyTreeIntoKey(hSrc, hDst);
        RegCloseKey(hDst);
    }
    RegCloseKey(hSrc);
    hSrc = NULL;
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* 2. The class driver key named by the devnode "Driver" value. */
    Size = sizeof(ValueBuffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Driver", RRF_RT_REG_SZ, NULL,
                     ValueBuffer, &Size) == ERROR_SUCCESS)
    {
        WCHAR ClassPath[512];

        _snwprintf(ClassPath, RTL_NUMBER_OF(ClassPath), L"%s%s", CLASS_KEY, ValueBuffer);
        ClassPath[RTL_NUMBER_OF(ClassPath) - 1] = UNICODE_NULL;
        RegSetValueExW(hSeqKey, SNAP_VAL_CLASSKEYPATH, 0, REG_SZ,
                       (PBYTE)ClassPath, (lstrlenW(ClassPath) + 1) * sizeof(WCHAR));
        if (OpenPath(HKEY_LOCAL_MACHINE, ClassPath, KEY_READ, &hSrc) == ERROR_SUCCESS)
        {
            if (RegCreateKeyExW(hSeqKey, SNAP_KEY_CLASSKEY, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE, NULL, &hDst, NULL) == ERROR_SUCCESS)
            {
                if (CopyTreeIntoKey(hSrc, hDst) == ERROR_SUCCESS)
                    HadDriver |= 1;
                else
                    DPRINT1("Snapshot of class key %ls incomplete for %ls\n", ClassPath, DeviceInstanceId);
                RegCloseKey(hDst);
            }
            RegCloseKey(hSrc);
            hSrc = NULL;
        }
    }

    /* 3. The service named by the devnode "Service" value. */
    Size = sizeof(ValueBuffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Service", RRF_RT_REG_SZ, NULL,
                     ValueBuffer, &Size) == ERROR_SUCCESS)
    {
        WCHAR ServicePath[512];

        RegSetValueExW(hSeqKey, SNAP_VAL_SERVICENAME, 0, REG_SZ,
                       (PBYTE)ValueBuffer, (lstrlenW(ValueBuffer) + 1) * sizeof(WCHAR));
        _snwprintf(ServicePath, RTL_NUMBER_OF(ServicePath), L"%s%s", SERVICES_KEY, ValueBuffer);
        ServicePath[RTL_NUMBER_OF(ServicePath) - 1] = UNICODE_NULL;
        if (OpenPath(HKEY_LOCAL_MACHINE, ServicePath, KEY_READ, &hSrc) == ERROR_SUCCESS)
        {
            if (RegCreateKeyExW(hSeqKey, SNAP_KEY_SERVICE, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_WRITE, NULL, &hDst, NULL) == ERROR_SUCCESS)
            {
                if (CopyTreeIntoKey(hSrc, hDst) == ERROR_SUCCESS)
                    HadDriver |= 2;
                else
                    DPRINT1("Snapshot of service %ls incomplete for %ls\n", ValueBuffer, DeviceInstanceId);
                RegCloseKey(hDst);
            }
            RegCloseKey(hSrc);
            hSrc = NULL;
        }
    }

    RegSetValueExW(hSeqKey, SNAP_VAL_HADDRIVER, 0, REG_DWORD, (PBYTE)&HadDriver, sizeof(HadDriver));

    PruneOldSnapshots(hDeviceKey);
    if (OutSeq)
        *OutSeq = Seq;
    Error = ERROR_SUCCESS;

Cleanup:
    if (hSrc)
        RegCloseKey(hSrc);
    if (hSeqKey)
        RegCloseKey(hSeqKey);
    if (hDeviceKey)
        RegCloseKey(hDeviceKey);
    if (hSnapshots)
        RegCloseKey(hSnapshots);
    LeaveCriticalSection(&SnapshotLock);
    return Error;
}

static ULONG
FindNewestSeq(
    _In_ HKEY hDeviceKey)
{
    NAME_LIST Keys = {0};
    ULONG Newest = 0, Seq, i;

    if (NameListCollect(hDeviceKey, FALSE, &Keys) != ERROR_SUCCESS)
        return 0;
    if (Keys.Count == 0)
        goto Cleanup;

    for (i = 0; i < Keys.Count; i++)
    {
        Seq = wcstoul(Keys.Names + i * Keys.Stride, NULL, 10);
        if (Seq > Newest)
            Newest = Seq;
    }

Cleanup:
    NameListFree(&Keys);
    return Newest;
}

/* Restores one snapshot of the device and re-runs its start.
 * Returns ERROR_SUCCESS when the registry is restored and the device restarted
 * without a problem, ERROR_INSTALL_FAILURE when the registry is restored but
 * the device still reports a problem, and another error when the restore
 * itself failed. The caller must hold the install lock. */
DWORD
DrvInstRollbackDeviceToSeq(
    _In_ PCWSTR DeviceInstanceId,
    _In_ ULONG Seq,
    _Out_opt_ PBOOL NeedReboot)
{
    WCHAR Encoded[DRVINST_DEVICE_ID_CCH];
    WCHAR Path[512];
    WCHAR CurrentDriver[256];
    WCHAR RestoredDriver[256];
    WCHAR CurrentService[256];
    WCHAR ClassPath[512];
    WCHAR ServiceName[256];
    WCHAR SeqName[16];
    HKEY hSnapshots = NULL, hDeviceKey = NULL, hSeqKey = NULL;
    HKEY hSnap = NULL, hReal = NULL;
    DWORD Size, HadDriver = 0, Disposition;
    CONFIGRET Cr = CR_FAILURE;
    DEVINST DevInst;
    LONG Error;

    if (NeedReboot)
        *NeedReboot = FALSE;

    EncodeDeviceId(DeviceInstanceId, Encoded);
    CurrentDriver[0] = UNICODE_NULL;
    CurrentService[0] = UNICODE_NULL;
    ServiceName[0] = UNICODE_NULL;
    ClassPath[0] = UNICODE_NULL;

    if (!DeviceInstanceId[0] || !IsValidDeviceInstanceId(DeviceInstanceId))
        return ERROR_INVALID_PARAMETER;

    Error = OpenPath(HKEY_LOCAL_MACHINE, DRVINST_REG_SNAPSHOTS, KEY_READ, &hSnapshots);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = OpenPath(hSnapshots, Encoded, KEY_READ, &hDeviceKey);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    _snwprintf(SeqName, RTL_NUMBER_OF(SeqName), L"%lu", Seq);
    SeqName[RTL_NUMBER_OF(SeqName) - 1] = UNICODE_NULL;
    Error = OpenPath(hDeviceKey, SeqName, KEY_READ, &hSeqKey);
    if (Error != ERROR_SUCCESS)
    {
        Error = ERROR_NOT_FOUND;
        goto Cleanup;
    }

    /* What the failed install pointed the devnode at, before we overwrite it. */
    _snwprintf(Path, RTL_NUMBER_OF(Path), L"%s%s", ENUM_KEY, DeviceInstanceId);
    Path[RTL_NUMBER_OF(Path) - 1] = UNICODE_NULL;
    Size = sizeof(CurrentDriver);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Driver", RRF_RT_REG_SZ, NULL,
                     CurrentDriver, &Size) != ERROR_SUCCESS)
        CurrentDriver[0] = UNICODE_NULL;
    Size = sizeof(CurrentService);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Service", RRF_RT_REG_SZ, NULL,
                     CurrentService, &Size) != ERROR_SUCCESS)
        CurrentService[0] = UNICODE_NULL;

    Size = sizeof(HadDriver);
    if (RegQueryValueExW(hSeqKey, SNAP_VAL_HADDRIVER, NULL, NULL, (PBYTE)&HadDriver, &Size) != ERROR_SUCCESS)
        HadDriver = 0;

    Size = sizeof(ClassPath);
    RegGetValueW(hSeqKey, NULL, SNAP_VAL_CLASSKEYPATH, RRF_RT_REG_SZ, NULL, ClassPath, &Size);
    Size = sizeof(ServiceName);
    RegGetValueW(hSeqKey, NULL, SNAP_VAL_SERVICENAME, RRF_RT_REG_SZ, NULL, ServiceName, &Size);

    /* 1. Devnode back to the snapshot. */
    Error = OpenPath(hSeqKey, SNAP_KEY_DEVNODE, KEY_READ, &hSnap);
    if (Error != ERROR_SUCCESS)
        goto Cleanup;
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, Path, 0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_WRITE | KEY_READ, NULL, &hReal, &Disposition);
    if (Error == ERROR_SUCCESS)
    {
        Error = SyncKeyWithSnapshot(hSnap, hReal);
        RegCloseKey(hReal);
    }
    RegCloseKey(hSnap);
    hSnap = NULL;
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* 2. Class driver key: restore the saved one, and remove the one the
     *    failed install left behind when it differs. */
    if (HadDriver & 1)
    {
        Error = OpenPath(hSeqKey, SNAP_KEY_CLASSKEY, KEY_READ, &hSnap);
        if (Error == ERROR_SUCCESS)
        {
            Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ClassPath, 0, NULL, REG_OPTION_NON_VOLATILE,
                                    KEY_WRITE | KEY_READ, NULL, &hReal, &Disposition);
            if (Error == ERROR_SUCCESS)
            {
                Error = SyncKeyWithSnapshot(hSnap, hReal);
                RegCloseKey(hReal);
            }
            RegCloseKey(hSnap);
            hSnap = NULL;
        }
    }
    if (Error == ERROR_SUCCESS && CurrentDriver[0] && IsSafeKeyName(CurrentDriver))
    {
        Size = sizeof(RestoredDriver);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Driver", RRF_RT_REG_SZ, NULL,
                         RestoredDriver, &Size) != ERROR_SUCCESS)
            RestoredDriver[0] = UNICODE_NULL;
        if (RestoredDriver[0] == UNICODE_NULL || lstrcmpW(RestoredDriver, CurrentDriver) != 0)
        {
            WCHAR Orphan[512];

            _snwprintf(Orphan, RTL_NUMBER_OF(Orphan), L"%s%s", CLASS_KEY, CurrentDriver);
            Orphan[RTL_NUMBER_OF(Orphan) - 1] = UNICODE_NULL;
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, Orphan);
        }
    }
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* 3. Service: restore the saved one; remove the service of the failed
     *    install only when it is not the restored one, has a safe name, and is
     *    a demand- or disabled-start service (never a boot/system one, and
     *    never the service another device may share). */
    if (HadDriver & 2)
    {
        Error = OpenPath(hSeqKey, SNAP_KEY_SERVICE, KEY_READ, &hSnap);
        if (Error == ERROR_SUCCESS)
        {
            WCHAR ServicePath[512];

            _snwprintf(ServicePath, RTL_NUMBER_OF(ServicePath), L"%s%s", SERVICES_KEY, ServiceName);
            ServicePath[RTL_NUMBER_OF(ServicePath) - 1] = UNICODE_NULL;
            Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ServicePath, 0, NULL, REG_OPTION_NON_VOLATILE,
                                    KEY_WRITE | KEY_READ, NULL, &hReal, &Disposition);
            if (Error == ERROR_SUCCESS)
            {
                Error = SyncKeyWithSnapshot(hSnap, hReal);
                RegCloseKey(hReal);
            }
            RegCloseKey(hSnap);
            hSnap = NULL;
        }
    }

    if (Error == ERROR_SUCCESS && CurrentService[0] &&
        lstrcmpW(CurrentService, ServiceName) != 0 && IsSafeKeyName(CurrentService))
    {
        WCHAR ServicePath[512];
        DWORD Start = 0xFFFFFFFF;

        Size = sizeof(Start);
        _snwprintf(ServicePath, RTL_NUMBER_OF(ServicePath), L"%s%s", SERVICES_KEY, CurrentService);
        ServicePath[RTL_NUMBER_OF(ServicePath) - 1] = UNICODE_NULL;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, ServicePath, L"Start", RRF_RT_REG_DWORD, NULL,
                         &Start, &Size) != ERROR_SUCCESS)
            Start = 0xFFFFFFFF;
        if (Start == SERVICE_DEMAND_START || Start == SERVICE_DISABLED)
        {
            if (IsServiceReferencedElsewhere(CurrentService, DeviceInstanceId))
            {
                DPRINT1("Keeping service %ls after rollback of %ls (other device uses it)\n",
                        CurrentService, DeviceInstanceId);
            }
            else
            {
                RegDeleteTreeW(HKEY_LOCAL_MACHINE, ServicePath);
            }
        }
        else if (Start != 0xFFFFFFFF)
        {
            DPRINT1("Keeping service %ls after rollback of %ls (start %lu)\n",
                    CurrentService, DeviceInstanceId, Start);
        }
    }
    if (Error != ERROR_SUCCESS)
        goto Cleanup;

    /* A rollback that points the devnode at a different service than the one
     * the failed install loaded cannot unload the old driver in place. */
    if (NeedReboot)
    {
        WCHAR RestoredService[256];

        Size = sizeof(RestoredService);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"Service", RRF_RT_REG_SZ, NULL,
                         RestoredService, &Size) != ERROR_SUCCESS)
            RestoredService[0] = UNICODE_NULL;
        if (CurrentService[0] != UNICODE_NULL &&
            lstrcmpW(CurrentService, RestoredService) != 0)
            *NeedReboot = TRUE;
    }

    /* 4. Re-run the start so the restored driver binds again, and check. */
    Cr = CM_Locate_DevNodeW(&DevInst, (DEVINSTID_W)DeviceInstanceId, CM_LOCATE_DEVNODE_NORMAL);
    if (Cr == CR_SUCCESS)
        Cr = CM_Setup_DevNode(DevInst, CM_SETUP_DEVNODE_READY);
    if (Cr == CR_SUCCESS)
    {
        ULONG Status = 0, Problem = 0;

        Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
        if (Cr == CR_SUCCESS && Problem != 0)
            Cr = (CONFIGRET)Problem;
    }

    if (Cr != CR_SUCCESS)
    {
        DPRINT1("Rollback restart of %ls did not clear the problem (cr %lu)\n",
                DeviceInstanceId, Cr);
        Error = ERROR_INSTALL_FAILURE;
    }
    else
    {
        Error = ERROR_SUCCESS;
    }

Cleanup:
    if (hSnap)
        RegCloseKey(hSnap);
    if (hSeqKey)
        RegCloseKey(hSeqKey);
    if (hDeviceKey)
        RegCloseKey(hDeviceKey);
    if (hSnapshots)
        RegCloseKey(hSnapshots);
    return Error;
}

/* Restores the newest snapshot of the device. Caller holds the install lock. */
DWORD
DrvInstRollbackDevice(
    _In_ PCWSTR DeviceInstanceId,
    _Out_opt_ PBOOL NeedReboot)
{
    WCHAR Encoded[DRVINST_DEVICE_ID_CCH];
    HKEY hSnapshots = NULL, hDeviceKey = NULL;
    ULONG Newest;
    DWORD Error;

    if (NeedReboot)
        *NeedReboot = FALSE;

    EncodeDeviceId(DeviceInstanceId, Encoded);

    Error = OpenPath(HKEY_LOCAL_MACHINE, DRVINST_REG_SNAPSHOTS, KEY_READ, &hSnapshots);
    if (Error != ERROR_SUCCESS)
        return Error;
    Error = OpenPath(hSnapshots, Encoded, KEY_READ, &hDeviceKey);
    RegCloseKey(hSnapshots);
    if (Error != ERROR_SUCCESS)
        return Error;

    Newest = FindNewestSeq(hDeviceKey);
    RegCloseKey(hDeviceKey);
    if (!Newest)
        return ERROR_NOT_FOUND;

    return DrvInstRollbackDeviceToSeq(DeviceInstanceId, Newest, NeedReboot);
}
