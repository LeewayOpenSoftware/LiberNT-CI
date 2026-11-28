/*
 *  ReactOS kernel
 *  Copyright (C) 2003 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * PURPOSE:         Computer name functions
 * FILE:            dll/win32/kernel32/client/compname.c
 * PROGRAMERS:      Eric Kohl
 *                  Katayama Hirofumi MZ
 */

/* INCLUDES ******************************************************************/

#include <k32.h>
#include <windns.h>

#define NDEBUG
#include <debug.h>

typedef NTSTATUS (WINAPI *FN_DnsValidateName_W)(LPCWSTR, DNS_NAME_FORMAT);

/* FUNCTIONS *****************************************************************/



/*
 * @implemented
 */
BOOL
WINAPI
GetComputerNameA(LPSTR lpBuffer, LPDWORD lpnSize)
{
    BOOL ret;

    ret = GetComputerNameExA(ComputerNameNetBIOS, lpBuffer, lpnSize);
    if (!ret && GetLastError() == ERROR_MORE_DATA)
        SetLastError(ERROR_BUFFER_OVERFLOW);

    return ret;
}


/*
 * @implemented
 */
BOOL
WINAPI
GetComputerNameW(LPWSTR lpBuffer, LPDWORD lpnSize)
{
    BOOL ret;

    ret = GetComputerNameExW(ComputerNameNetBIOS, lpBuffer, lpnSize);
    if (!ret && GetLastError() == ERROR_MORE_DATA)
        SetLastError(ERROR_BUFFER_OVERFLOW);

    return ret;
}

static
BOOL
BaseVerifyDnsName(LPCWSTR lpDnsName, DNS_NAME_FORMAT Format)
{
    HINSTANCE hDNSAPI;
    FN_DnsValidateName_W fnValidate;
    NTSTATUS Status;
    BOOL ret = FALSE;

    hDNSAPI = LoadLibraryW(L"dnsapi.dll");
    if (hDNSAPI == NULL)
        return FALSE;

    fnValidate = (FN_DnsValidateName_W)GetProcAddress(hDNSAPI, "DnsValidateName_W");
    if (fnValidate)
    {
        Status = (*fnValidate)(lpDnsName, Format);
        if (Status == STATUS_SUCCESS || Status == DNS_ERROR_NON_RFC_NAME)
            ret = TRUE;
    }

    FreeLibrary(hDNSAPI);

    return ret;
}






/*
 * @implemented
 */
BOOL
WINAPI
DnsHostnameToComputerNameA(LPCSTR Hostname,
                           LPSTR ComputerName,
                           LPDWORD nSize)
{
    DWORD len;

    DPRINT("(%s, %p, %p)\n", Hostname, ComputerName, nSize);

    if (!Hostname || !nSize)
        return FALSE;

    len = lstrlenA(Hostname);

    if (len > MAX_COMPUTERNAME_LENGTH)
        len = MAX_COMPUTERNAME_LENGTH;

    if (*nSize < len)
    {
        *nSize = len;
        return FALSE;
    }

    if (!ComputerName) return FALSE;

    memcpy(ComputerName, Hostname, len);
    ComputerName[len + 1] = 0;
    return TRUE;
}


/*
 * @implemented
 */
BOOL
WINAPI
DnsHostnameToComputerNameW(LPCWSTR hostname,
                           LPWSTR computername,
                           LPDWORD size)
{
    DWORD len;

    DPRINT("(%s, %p, %p): stub\n", hostname, computername, size);

    if (!hostname || !size) return FALSE;
    len = lstrlenW(hostname);

    if (len > MAX_COMPUTERNAME_LENGTH)
        len = MAX_COMPUTERNAME_LENGTH;

    if (*size < len)
    {
        *size = len;
        return FALSE;
    }
    if (!computername) return FALSE;

    memcpy(computername, hostname, len * sizeof(WCHAR));
    computername[len + 1] = 0;
    return TRUE;
}

static
DWORD
BaseAppendAlternateComputerName(HANDLE KeyHandle,
                                PCUNICODE_STRING ValueName,
                                PCWSTR List,
                                SIZE_T ListChars,
                                PCUNICODE_STRING Name)
{
    UNICODE_STRING Entry;
    SIZE_T Offset = 0, Length, Kept, Terminator, NewChars;
    PWSTR NewList;
    NTSTATUS Status;

    while (Offset < ListChars && List[Offset] != UNICODE_NULL)
    {
        Length = wcsnlen(&List[Offset], ListChars - Offset);
        if (Length * sizeof(WCHAR) == Name->Length)
        {
            Entry.Buffer = (PWSTR)&List[Offset];
            Entry.Length = Entry.MaximumLength = Name->Length;
            if (RtlEqualUnicodeString(&Entry, Name, TRUE))
                return ERROR_SUCCESS;
        }
        Offset += Length + 1;
    }

    Kept = min(Offset, ListChars);
    Terminator = (Offset > ListChars) ? 1 : 0;
    NewChars = Kept + Terminator + Name->Length / sizeof(WCHAR) + 2;
    NewList = RtlAllocateHeap(RtlGetProcessHeap(), 0, NewChars * sizeof(WCHAR));
    if (NewList == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    if (Kept != 0)
        RtlCopyMemory(NewList, List, Kept * sizeof(WCHAR));
    if (Terminator)
        NewList[Kept] = UNICODE_NULL;
    RtlCopyMemory(&NewList[Kept + Terminator], Name->Buffer, Name->Length);
    NewList[NewChars - 2] = UNICODE_NULL;
    NewList[NewChars - 1] = UNICODE_NULL;

    Status = NtSetValueKey(KeyHandle,
                           (PUNICODE_STRING)ValueName,
                           0,
                           REG_MULTI_SZ,
                           NewList,
                           (ULONG)(NewChars * sizeof(WCHAR)));
    RtlFreeHeap(RtlGetProcessHeap(), 0, NewList);
    if (!NT_SUCCESS(Status))
        return RtlNtStatusToDosError(Status);

    NtFlushKey(KeyHandle);
    return ERROR_SUCCESS;
}

static
DWORD
BaseAddAlternateComputerName(PCUNICODE_STRING Name)
{
    UNICODE_STRING KeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                                 L"\\Services\\Dnscache\\Parameters");
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"AlternateComputerNames");
    OBJECT_ATTRIBUTES ObjectAttributes;
    PKEY_VALUE_PARTIAL_INFORMATION Info = NULL;
    HANDLE KeyHandle;
    ULONG Size = 0, Disposition;
    NTSTATUS Status;
    DWORD Error;

    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    Status = NtCreateKey(&KeyHandle,
                         KEY_QUERY_VALUE | KEY_SET_VALUE,
                         &ObjectAttributes,
                         0,
                         NULL,
                         REG_OPTION_NON_VOLATILE,
                         &Disposition);
    if (!NT_SUCCESS(Status))
        return RtlNtStatusToDosError(Status);

    Status = NtQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             NULL,
                             0,
                             &Size);
    if (Status == STATUS_BUFFER_TOO_SMALL || Status == STATUS_BUFFER_OVERFLOW)
    {
        Info = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
        if (Info == NULL)
        {
            NtClose(KeyHandle);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        Status = NtQueryValueKey(KeyHandle,
                                 &ValueName,
                                 KeyValuePartialInformation,
                                 Info,
                                 Size,
                                 &Size);
    }

    if (NT_SUCCESS(Status) && Info != NULL && Info->Type == REG_MULTI_SZ)
    {
        Error = BaseAppendAlternateComputerName(KeyHandle,
                                                &ValueName,
                                                (PCWSTR)Info->Data,
                                                Info->DataLength / sizeof(WCHAR),
                                                Name);
    }
    else if (NT_SUCCESS(Status) || Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        Error = BaseAppendAlternateComputerName(KeyHandle, &ValueName, NULL, 0, Name);
    }
    else
    {
        Error = RtlNtStatusToDosError(Status);
    }

    if (Info != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, Info);
    NtClose(KeyHandle);
    return Error;
}

DWORD
WINAPI
AddLocalAlternateComputerNameW(LPCWSTR lpDnsFQHostname, ULONG ulFlags)
{
    UNICODE_STRING Name;

    if (ulFlags != 0 || lpDnsFQHostname == NULL)
        return ERROR_INVALID_PARAMETER;

    if (!NT_SUCCESS(RtlInitUnicodeStringEx(&Name, lpDnsFQHostname)) ||
        !BaseVerifyDnsName(lpDnsFQHostname, DnsNameHostnameFull))
    {
        return ERROR_INVALID_PARAMETER;
    }

    return BaseAddAlternateComputerName(&Name);
}

DWORD
WINAPI
AddLocalAlternateComputerNameA(LPCSTR lpDnsFQHostname, ULONG ulFlags)
{
    ANSI_STRING AnsiName;
    UNICODE_STRING Name;
    DWORD Error;

    if (ulFlags != 0 || lpDnsFQHostname == NULL)
        return ERROR_INVALID_PARAMETER;

    if (!NT_SUCCESS(RtlInitAnsiStringEx(&AnsiName, lpDnsFQHostname)))
        return ERROR_INVALID_PARAMETER;

    if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&Name, &AnsiName, TRUE)))
        return ERROR_NOT_ENOUGH_MEMORY;

    Error = AddLocalAlternateComputerNameW(Name.Buffer, ulFlags);
    RtlFreeUnicodeString(&Name);
    return Error;
}

DWORD
WINAPI
EnumerateLocalComputerNamesA(PVOID pUnknown, DWORD Size, LPSTR lpBuffer, LPDWORD lpnSize)
{
    STUB;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD
WINAPI
EnumerateLocalComputerNamesW(PVOID pUnknown, DWORD Size, LPWSTR lpBuffer, LPDWORD lpnSize)
{
    STUB;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD
WINAPI
RemoveLocalAlternateComputerNameA(LPSTR lpName, DWORD Unknown)
{
    STUB;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD
WINAPI
RemoveLocalAlternateComputerNameW(LPWSTR lpName, DWORD Unknown)
{
    STUB;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
SetLocalPrimaryComputerNameA(IN DWORD Unknown1,
                             IN DWORD Unknown2)
{
    STUB;
    return FALSE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
SetLocalPrimaryComputerNameW(IN DWORD Unknown1,
                             IN DWORD Unknown2)
{
    STUB;
    return FALSE;
}


/* EOF */
