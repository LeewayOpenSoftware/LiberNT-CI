/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Network provider for the smb2rdr redirector: drive letter connections
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Stateless: a connection X: -> \\server\share is a DOS device whose target is
 *   \Device\smb2rdr\;X:\server\share
 * and every query is answered from that device definition, so there is no shared memory to keep in sync.
 * The actual SMB session, tree connect and authentication happen in the redirector and the daemon the
 * first time the path is opened; AddConnection opens the root once so a bad name or password fails here.
 */

#include <windows.h>
#include <winnetwk.h>
#include <npapi.h>
#include <wchar.h>

#include "smb2rdr_ioctl.h"

/* Private network type: SMB2 redirector (not one of the registered MS provider types) */
#define SMB2NP_NET_TYPE     0x00F10000

#define DEVICE_PREFIX       SMB2RDR_DEVICE_NAME L"\\;"
#define DEVICE_PREFIX_LEN   ((sizeof(DEVICE_PREFIX) / sizeof(WCHAR)) - 1)

/* "X:" -> upper case drive name; returns FALSE for anything else */
static BOOL ParseLocalName(LPCWSTR local, WCHAR out[3])
{
    if (!local || !local[0] || local[1] != L':' || local[2] != L'\0')
        return FALSE;
    if (!((local[0] >= L'A' && local[0] <= L'Z') || (local[0] >= L'a' && local[0] <= L'z')))
        return FALSE;
    out[0] = (WCHAR)((local[0] >= L'a') ? local[0] - L'a' + L'A' : local[0]);
    out[1] = L':';
    out[2] = L'\0';
    return TRUE;
}

/* "\\server\share[\path]" (forward slashes accepted) -> "\server\share[\path]"; at least server and share */
static BOOL BuildServerShare(LPCWSTR remote, WCHAR *out, size_t cch)
{
    size_t i, n;
    unsigned parts = 0;
    BOOL inPart = FALSE;

    if (!remote)
        return FALSE;
    n = wcslen(remote);
    if (n < 5 || n + 1 > cch ||
        (remote[0] != L'\\' && remote[0] != L'/') || (remote[1] != L'\\' && remote[1] != L'/'))
        return FALSE;
    for (i = 1; i < n; i++)
    {
        WCHAR c = remote[i];

        if (c == L'/')
            c = L'\\';
        out[i - 1] = c;
        if (c == L'\\')
            inPart = FALSE;
        else if (!inPart)
        {
            inPart = TRUE;
            parts++;
        }
    }
    out[n - 1] = L'\0';
    /* trailing separators are not part of the name */
    while (n > 1 && out[n - 2] == L'\\')
        out[--n - 1] = L'\0';
    return parts >= 2;
}

static BOOL BuildTarget(const WCHAR drive[3], LPCWSTR serverShare, WCHAR *out, size_t cch)
{
    if (DEVICE_PREFIX_LEN + 2 + wcslen(serverShare) + 1 > cch)
        return FALSE;
    wcscpy(out, DEVICE_PREFIX);
    wcscat(out, drive);
    wcscat(out, serverShare);
    return TRUE;
}

/* The smb2rdr target of a DOS device, or FALSE when the device is not one of ours */
static BOOL QueryOurTarget(const WCHAR drive[3], WCHAR *target, DWORD cch)
{
    if (!QueryDosDeviceW(drive, target, cch))
        return FALSE;
    return _wcsnicmp(target, DEVICE_PREFIX, DEVICE_PREFIX_LEN) == 0 && target[DEVICE_PREFIX_LEN] != L'\0';
}

DWORD WINAPI NPGetCaps(DWORD nIndex)
{
    switch (nIndex)
    {
        case WNNC_SPEC_VERSION:
            return WNNC_SPEC_VERSION51;
        case WNNC_NET_TYPE:
            return SMB2NP_NET_TYPE;
        case WNNC_DRIVER_VERSION:
            return 0x00010000;
        case WNNC_CONNECTION:
            return WNNC_CON_GETCONNECTIONS | WNNC_CON_CANCELCONNECTION | WNNC_CON_ADDCONNECTION |
                   WNNC_CON_ADDCONNECTION3;
        case WNNC_ENUMERATION:
            return WNNC_ENUM_LOCAL;
        case WNNC_START:
            return 1;
        default:
            return 0;
    }
}

DWORD WINAPI NPLogonNotify(PLUID LogonId, LPCWSTR AuthInfoType, PVOID AuthInfo, LPCWSTR PrevAuthInfoType,
                           PVOID PrevAuthInfo, LPWSTR StationName, PVOID StationHandle, LPWSTR *LogonScript)
{
    (void)LogonId; (void)AuthInfoType; (void)AuthInfo; (void)PrevAuthInfoType; (void)PrevAuthInfo;
    (void)StationName; (void)StationHandle;
    if (LogonScript)
        *LogonScript = NULL;
    return WN_SUCCESS;
}

DWORD WINAPI NPPasswordChangeNotify(LPCWSTR AuthInfoType, LPVOID AuthInfo, LPCWSTR PrevAuthInfoType,
                                    LPVOID PrevAuthInfo, LPWSTR StationName, LPVOID StationHandle,
                                    DWORD ChangeInfo)
{
    (void)AuthInfoType; (void)AuthInfo; (void)PrevAuthInfoType; (void)PrevAuthInfo; (void)StationName;
    (void)StationHandle; (void)ChangeInfo;
    SetLastError(WN_NOT_SUPPORTED);
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI NPAddConnection3(HWND hwndOwner, LPNETRESOURCEW Resource, LPWSTR Password, LPWSTR UserName,
                              DWORD Flags)
{
    WCHAR drive[3], serverShare[MAX_PATH], target[MAX_PATH], probe[8], existing[MAX_PATH];
    HANDLE h;
    DWORD err;

    (void)hwndOwner; (void)Password; (void)UserName; (void)Flags;
    /* TODO(identity): Password/UserName are not forwarded yet; the daemon uses its own identity. */

    if (!Resource || !BuildServerShare(Resource->lpRemoteName, serverShare, MAX_PATH))
        return WN_BAD_NETNAME;
    if (!Resource->lpLocalName || !Resource->lpLocalName[0])
    {
        /* Deviceless connection: just prove the share can be opened through the redirector. */
        WCHAR unc[MAX_PATH + 8];

        wcscpy(unc, L"\\\\?\\UNC");
        wcscat(unc, serverShare + 0 /* "\server\share" */);
        h = CreateFileW(unc, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE)
            return GetLastError();
        CloseHandle(h);
        return WN_SUCCESS;
    }
    if (!ParseLocalName(Resource->lpLocalName, drive))
        return WN_BAD_LOCALNAME;
    if (!BuildTarget(drive, serverShare, target, MAX_PATH))
        return WN_BAD_NETNAME;

    if (QueryDosDeviceW(drive, existing, MAX_PATH) || GetLastError() != ERROR_FILE_NOT_FOUND)
        return WN_ALREADY_CONNECTED;

    if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, drive, target))
        return GetLastError();

    /* Prove the connection: open the root of the new drive */
    probe[0] = drive[0];
    probe[1] = L':';
    probe[2] = L'\\';
    probe[3] = L'\0';
    h = CreateFileW(probe, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        err = GetLastError();
        DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH | DDD_EXACT_MATCH_ON_REMOVE |
                         DDD_NO_BROADCAST_SYSTEM, drive, target);
        return err ? err : WN_BAD_NETNAME;
    }
    CloseHandle(h);
    return WN_SUCCESS;
}

DWORD WINAPI NPAddConnection(LPNETRESOURCEW Resource, LPWSTR Password, LPWSTR UserName)
{
    return NPAddConnection3(NULL, Resource, Password, UserName, 0);
}

DWORD WINAPI NPCancelConnection(LPWSTR Name, BOOL Force)
{
    WCHAR drive[3], target[MAX_PATH];

    (void)Force;
    if (!Name || !ParseLocalName(Name, drive))
        return WN_SUCCESS;              /* deviceless connections hold no state here */
    if (!QueryOurTarget(drive, target, MAX_PATH))
        return WN_NOT_CONNECTED;
    if (!DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH | DDD_EXACT_MATCH_ON_REMOVE |
                          DDD_NO_BROADCAST_SYSTEM, drive, target))
        return GetLastError();
    return WN_SUCCESS;
}

/* Buffer size is in characters, including the terminator (as mpr passes it through). */
DWORD WINAPI NPGetConnection(LPWSTR LocalName, LPWSTR RemoteName, LPDWORD BufferSize)
{
    WCHAR drive[3], target[MAX_PATH];
    const WCHAR *serverShare;
    DWORD need;

    if (!LocalName || !BufferSize || !ParseLocalName(LocalName, drive))
        return WN_BAD_LOCALNAME;
    if (!QueryOurTarget(drive, target, MAX_PATH))
        return WN_NOT_CONNECTED;
    /* target: \Device\smb2rdr\;X:\server\share */
    serverShare = target + DEVICE_PREFIX_LEN;
    if (_wcsnicmp(serverShare, drive, 2) != 0 || serverShare[2] != L'\\')
        return WN_NOT_CONNECTED;
    serverShare += 2;                   /* "\server\share" */
    need = (DWORD)wcslen(serverShare) + 2 /* leading backslash */ + 1;
    if (*BufferSize < need)
    {
        *BufferSize = need;
        return WN_MORE_DATA;
    }
    RemoteName[0] = L'\\';
    wcscpy(RemoteName + 1, serverShare);
    *BufferSize = need;
    return WN_SUCCESS;
}

DWORD WINAPI NPOpenEnum(DWORD Scope, DWORD Type, DWORD Usage, LPNETRESOURCEW Resource, LPHANDLE Enum)
{
    (void)Scope; (void)Type; (void)Usage; (void)Resource;
    if (Enum)
        *Enum = NULL;
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI NPEnumResource(HANDLE Enum, LPDWORD Count, LPVOID Buffer, LPDWORD BufferSize)
{
    (void)Enum; (void)Count; (void)Buffer; (void)BufferSize;
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI NPCloseEnum(HANDLE Enum)
{
    (void)Enum;
    return WN_SUCCESS;
}

DWORD WINAPI NPGetResourceParent(LPNETRESOURCEW Resource, LPVOID Buffer, LPDWORD BufferSize)
{
    (void)Resource; (void)Buffer; (void)BufferSize;
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI NPGetResourceInformation(LPNETRESOURCEW Resource, LPVOID Buffer, LPDWORD BufferSize, LPWSTR *System)
{
    (void)Resource; (void)Buffer; (void)BufferSize; (void)System;
    return WN_NOT_SUPPORTED;
}

DWORD WINAPI NPGetUniversalName(LPWSTR LocalPath, DWORD InfoLevel, LPVOID Buffer, LPDWORD BufferSize)
{
    (void)LocalPath; (void)InfoLevel; (void)Buffer; (void)BufferSize;
    return WN_NOT_SUPPORTED;
}
