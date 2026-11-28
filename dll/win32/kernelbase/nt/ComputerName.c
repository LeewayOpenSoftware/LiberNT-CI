/*
 * PROJECT:         ReactOS Win32 Base API (Orosi KernelBase)
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         Computer name functions
 *                  Moved from kernel32 client/compname.c; kernelbase implements them on
 *                  Windows 11 26100 and kernel32 forwards.
 * COPYRIGHT:       Copyright (C) 2003 ReactOS Team
 *                  Eric Kohl
 *                  Katayama Hirofumi MZ
 */

#include <stdio.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <windns.h>

#define NTOS_MODE_USER
#include <ndk/cmfuncs.h>
#include <ndk/exfuncs.h>
#include <ndk/mmfuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/psfuncs.h>
#include <ndk/rtlfuncs.h>

#include <ntstrsafe.h>
#include <pseh/pseh2.h>

#define NDEBUG
#include <debug.h>

/* kernel32_shared */
DWORD
WINAPI
BaseSetLastNTError(IN NTSTATUS Status);

typedef NTSTATUS (WINAPI *FN_DnsValidateName_W)(LPCWSTR, DNS_NAME_FORMAT);

/* PRIVATE FUNCTIONS **********************************************************/

/* kernel32 keeps its own copy for the alternate computer name functions */
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

static
BOOL
GetComputerNameFromRegistry(LPWSTR RegistryKey,
                            LPWSTR ValueNameStr,
                            LPWSTR lpBuffer,
                            LPDWORD nSize)
{
    PKEY_VALUE_PARTIAL_INFORMATION KeyInfo;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;
    UNICODE_STRING ValueName;
    HANDLE KeyHandle;
    ULONG KeyInfoSize;
    ULONG ReturnSize;
    NTSTATUS Status;

    if (lpBuffer != NULL && *nSize > 0)
        lpBuffer[0] = 0;

    RtlInitUnicodeString(&KeyName, RegistryKey);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = NtOpenKey(&KeyHandle,
                       KEY_READ,
                       &ObjectAttributes);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError (Status);
        return FALSE;
    }

    KeyInfoSize = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + *nSize * sizeof(WCHAR);
    KeyInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, KeyInfoSize);
    if (KeyInfo == NULL)
    {
        NtClose(KeyHandle);
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }

    RtlInitUnicodeString(&ValueName, ValueNameStr);

    Status = NtQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             KeyInfo,
                             KeyInfoSize,
                             &ReturnSize);

    NtClose(KeyHandle);

    if (!NT_SUCCESS(Status))
    {
        if ((Status == STATUS_BUFFER_TOO_SMALL ||
             Status == STATUS_BUFFER_OVERFLOW) &&
            ReturnSize >= FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data))
        {
            *nSize = (ReturnSize - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data)) / sizeof(WCHAR);
        }
        goto failed;
    }

    if (KeyInfo->Type != REG_SZ)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto failed;
    }

    if (!lpBuffer || *nSize < (KeyInfo->DataLength / sizeof(WCHAR)))
    {
        *nSize = (ReturnSize - FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data)) / sizeof(WCHAR);
        Status = STATUS_BUFFER_OVERFLOW;
        goto failed;
    }

    *nSize = KeyInfo->DataLength / sizeof(WCHAR) - 1;
    RtlCopyMemory(lpBuffer, KeyInfo->Data, KeyInfo->DataLength);
    lpBuffer[*nSize] = 0;

    RtlFreeHeap(RtlGetProcessHeap(), 0, KeyInfo);

    return TRUE;

failed:
    RtlFreeHeap(RtlGetProcessHeap(), 0, KeyInfo);
    BaseSetLastNTError(Status);
    return FALSE;
}

static
BOOL
SetActiveComputerNameToRegistry(LPCWSTR RegistryKey,
                                LPCWSTR SubKey,
                                LPCWSTR ValueNameStr,
                                LPCWSTR lpBuffer)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;
    UNICODE_STRING ValueName;
    HANDLE KeyHandle, SubKeyHandle;
    SIZE_T StringLength;
    ULONG Disposition;
    NTSTATUS Status;

    StringLength = wcslen(lpBuffer);
    if (StringLength > ((MAXULONG / sizeof(WCHAR)) - 1))
    {
        return FALSE;
    }

    RtlInitUnicodeString(&KeyName, RegistryKey);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = NtOpenKey(&KeyHandle,
                       KEY_WRITE,
                       &ObjectAttributes);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    RtlInitUnicodeString(&KeyName, SubKey);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE,
                               KeyHandle,
                               NULL);

    Status = NtCreateKey(&SubKeyHandle,
                         KEY_WRITE,
                         &ObjectAttributes,
                         0,
                         NULL,
                         REG_OPTION_VOLATILE,
                         &Disposition);
    if (!NT_SUCCESS(Status))
    {
        NtClose(KeyHandle);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    RtlInitUnicodeString(&ValueName, ValueNameStr);

    Status = NtSetValueKey(SubKeyHandle,
                           &ValueName,
                           0,
                           REG_SZ,
                           (PVOID)lpBuffer,
                           (StringLength + 1) * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
    {
        NtClose(SubKeyHandle);
        NtClose(KeyHandle);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    NtFlushKey(SubKeyHandle);
    NtClose(SubKeyHandle);
    NtClose(KeyHandle);

    return TRUE;
}

/*
 * @implemented
 */
static
BOOL
IsValidComputerName(COMPUTER_NAME_FORMAT NameType,
                    LPCWSTR lpComputerName)
{
    size_t Length;
    static const WCHAR s_szInvalidChars[] =
        L"\"/\\[]:|<>+=;,?"
        L"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
        L"\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F";

    if (lpComputerName == NULL)
        return FALSE;

#define MAX_COMPUTER_NAME_EX 64
    /* Get string length */
    if (!NT_SUCCESS(RtlStringCchLengthW(lpComputerName, MAX_COMPUTER_NAME_EX + 1, &Length)))
        return FALSE;
#undef MAX_COMPUTER_NAME_EX

    /* An empty name is invalid, except a DNS name */
    if (Length == 0 && NameType != ComputerNamePhysicalDnsDomain)
        return FALSE;

    /* Leading or trailing spaces are invalid */
    if (Length > 0 &&
        (lpComputerName[0] == L' ' || lpComputerName[Length - 1] == L' '))
    {
        return FALSE;
    }

    /* Check whether the name contains any invalid character */
    if (wcscspn(lpComputerName, s_szInvalidChars) < Length)
        return FALSE;

    switch (NameType)
    {
        case ComputerNamePhysicalNetBIOS:
            if (Length > MAX_COMPUTERNAME_LENGTH)
                return FALSE;
            return TRUE;

        case ComputerNamePhysicalDnsDomain:
            /* An empty DNS name is valid */
            if (Length != 0)
                return BaseVerifyDnsName(lpComputerName, DnsNameDomain);
            return TRUE;

        case ComputerNamePhysicalDnsHostname:
            return BaseVerifyDnsName(lpComputerName, DnsNameHostnameLabel);

        default:
            return FALSE;
    }
}

static
BOOL
SetComputerNameToRegistry(LPCWSTR RegistryKey,
                          LPCWSTR ValueNameStr,
                          LPCWSTR lpBuffer)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING KeyName;
    UNICODE_STRING ValueName;
    HANDLE KeyHandle;
    SIZE_T StringLength;
    NTSTATUS Status;

    StringLength = wcslen(lpBuffer);
    if (StringLength > ((MAXULONG / sizeof(WCHAR)) - 1))
    {
        return FALSE;
    }

    RtlInitUnicodeString(&KeyName, RegistryKey);
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    Status = NtOpenKey(&KeyHandle,
                       KEY_WRITE,
                       &ObjectAttributes);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    RtlInitUnicodeString(&ValueName, ValueNameStr);

    Status = NtSetValueKey(KeyHandle,
                           &ValueName,
                           0,
                           REG_SZ,
                           (PVOID)lpBuffer,
                           (StringLength + 1) * sizeof(WCHAR));
    if (!NT_SUCCESS(Status))
    {
        NtClose(KeyHandle);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    NtFlushKey(KeyHandle);
    NtClose(KeyHandle);

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
BOOL
WINAPI
GetComputerNameExW(COMPUTER_NAME_FORMAT NameType,
                   LPWSTR lpBuffer,
                   LPDWORD nSize)
{
    UNICODE_STRING ResultString;
    UNICODE_STRING DomainPart;
    RTL_QUERY_REGISTRY_TABLE QueryTable[2];
    NTSTATUS Status;
    BOOL ret = TRUE;
    DWORD HostSize;

    if ((nSize == NULL) ||
        (lpBuffer == NULL && *nSize > 0))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (NameType)
    {
        case ComputerNameNetBIOS:
            ret = GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                               L"\\Control\\ComputerName\\ActiveComputerName",
                                               L"ComputerName",
                                               lpBuffer,
                                               nSize);
            if ((ret == FALSE) &&
                (GetLastError() != ERROR_MORE_DATA))
            {
                ret = GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                                  L"\\Control\\ComputerName\\ComputerName",
                                                  L"ComputerName",
                                                  lpBuffer,
                                                  nSize);
                if (ret)
                {
                    ret = SetActiveComputerNameToRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                                          L"\\Control\\ComputerName",
                                                          L"ActiveComputerName",
                                                          L"ComputerName",
                                                          lpBuffer);
                }
            }
            return ret;

        case ComputerNameDnsDomain:
            return GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                               L"\\Services\\Tcpip\\Parameters",
                                               L"Domain",
                                               lpBuffer,
                                               nSize);

        case ComputerNameDnsFullyQualified:
            ResultString.Length = 0;
            ResultString.MaximumLength = (USHORT)*nSize * sizeof(WCHAR);
            ResultString.Buffer = lpBuffer;

            RtlZeroMemory(QueryTable, sizeof(QueryTable));
            RtlInitUnicodeString(&DomainPart, NULL);

            QueryTable[0].Name = L"HostName";
            QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
            QueryTable[0].EntryContext = &DomainPart;

            Status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                            L"\\Registry\\Machine\\System"
                                            L"\\CurrentControlSet\\Services\\Tcpip"
                                            L"\\Parameters",
                                            QueryTable,
                                            NULL,
                                            NULL);

            if (NT_SUCCESS(Status))
            {
                Status = RtlAppendUnicodeStringToString(&ResultString, &DomainPart);
                HostSize = DomainPart.Length;

                if (!NT_SUCCESS(Status))
                {
                    ret = FALSE;
                }

                RtlAppendUnicodeToString(&ResultString, L".");
                RtlFreeUnicodeString(&DomainPart);

                RtlInitUnicodeString(&DomainPart, NULL);
                QueryTable[0].Name = L"Domain";
                QueryTable[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
                QueryTable[0].EntryContext = &DomainPart;

                Status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                                L"\\Registry\\Machine\\System"
                                                L"\\CurrentControlSet\\Services\\Tcpip"
                                                L"\\Parameters",
                                                QueryTable,
                                                NULL,
                                                NULL);

                if (NT_SUCCESS(Status))
                {
                    Status = RtlAppendUnicodeStringToString(&ResultString, &DomainPart);
                    if ((!NT_SUCCESS(Status)) || (!ret))
                    {
                        *nSize = HostSize + DomainPart.Length;
                        SetLastError(ERROR_MORE_DATA);
                        RtlFreeUnicodeString(&DomainPart);
                        return FALSE;
                    }
                    RtlFreeUnicodeString(&DomainPart);
                    *nSize = ResultString.Length / sizeof(WCHAR) - 1;
                    return TRUE;
                }
            }
            return FALSE;

        case ComputerNameDnsHostname:
            return GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                               L"\\Services\\Tcpip\\Parameters",
                                               L"Hostname",
                                               lpBuffer,
                                               nSize);

        case ComputerNamePhysicalDnsDomain:
            ret = GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                              L"\\Services\\Tcpip\\Parameters",
                                              L"NV Domain",
                                              lpBuffer,
                                              nSize);
            if (ret || GetLastError() == ERROR_MORE_DATA)
                return ret;

            return GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                               L"\\Services\\Tcpip\\Parameters",
                                               L"Domain",
                                               lpBuffer,
                                               nSize);

        /* XXX Redo this */
        case ComputerNamePhysicalDnsFullyQualified:
            return GetComputerNameExW(ComputerNameDnsFullyQualified,
                                      lpBuffer,
                                      nSize);

        case ComputerNamePhysicalDnsHostname:
            ret = GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                              L"\\Services\\Tcpip\\Parameters",
                                              L"NV Hostname",
                                              lpBuffer,
                                              nSize);
            if (ret || GetLastError() == ERROR_MORE_DATA)
                return ret;

            return GetComputerNameFromRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                               L"\\Services\\Tcpip\\Parameters",
                                               L"Hostname",
                                               lpBuffer,
                                               nSize);

        /* XXX Redo this */
        case ComputerNamePhysicalNetBIOS:
            return GetComputerNameExW(ComputerNameNetBIOS,
                                      lpBuffer,
                                      nSize);

        case ComputerNameMax:
            return FALSE;
    }

    return FALSE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetComputerNameExA(COMPUTER_NAME_FORMAT NameType,
                   LPSTR lpBuffer,
                   LPDWORD nSize)
{
    UNICODE_STRING UnicodeString;
    ANSI_STRING AnsiString;
    BOOL Result;
    PWCHAR TempBuffer = NULL;

    if ((nSize == NULL) ||
        (lpBuffer == NULL && *nSize > 0))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (*nSize > 0)
    {
        TempBuffer = RtlAllocateHeap(RtlGetProcessHeap(), 0, *nSize * sizeof(WCHAR));
        if (!TempBuffer)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }

    AnsiString.MaximumLength = (USHORT)*nSize;
    AnsiString.Length = 0;
    AnsiString.Buffer = lpBuffer;

    Result = GetComputerNameExW(NameType, TempBuffer, nSize);

    if (Result)
    {
        UnicodeString.MaximumLength = (USHORT)*nSize * sizeof(WCHAR) + sizeof(WCHAR);
        UnicodeString.Length = (USHORT)*nSize * sizeof(WCHAR);
        UnicodeString.Buffer = TempBuffer;

        RtlUnicodeStringToAnsiString(&AnsiString,
                                     &UnicodeString,
                                     FALSE);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, TempBuffer);

    return Result;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetComputerNameW(LPCWSTR lpComputerName)
{
    return SetComputerNameExW(ComputerNamePhysicalNetBIOS, lpComputerName);
}

/*
 * @implemented
 */
BOOL
WINAPI
SetComputerNameA(LPCSTR lpComputerName)
{
    return SetComputerNameExA(ComputerNamePhysicalNetBIOS, lpComputerName);
}

/*
 * @implemented
 */
BOOL
WINAPI
SetComputerNameExW(COMPUTER_NAME_FORMAT NameType,
                   LPCWSTR lpBuffer)
{
    WCHAR szShortName[MAX_COMPUTERNAME_LENGTH + 1];
    BOOL ret1, ret2;

    if (!IsValidComputerName(NameType, lpBuffer))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (NameType)
    {
        case ComputerNamePhysicalDnsDomain:
            return SetComputerNameToRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                             L"\\Services\\Tcpip\\Parameters",
                                             L"NV Domain",
                                             lpBuffer);

        case ComputerNamePhysicalDnsHostname:
            ret1 = SetComputerNameToRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                             L"\\Services\\Tcpip\\Parameters",
                                             L"NV Hostname",
                                             lpBuffer);

            RtlStringCchCopyNW(szShortName, ARRAYSIZE(szShortName), lpBuffer, MAX_COMPUTERNAME_LENGTH);
            ret2 = SetComputerNameToRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                             L"\\Control\\ComputerName\\ComputerName",
                                             L"ComputerName",
                                             szShortName);
            return (ret1 && ret2);

        case ComputerNamePhysicalNetBIOS:
            RtlStringCchCopyNW(szShortName, ARRAYSIZE(szShortName), lpBuffer, MAX_COMPUTERNAME_LENGTH);
            return SetComputerNameToRegistry(L"\\Registry\\Machine\\System\\CurrentControlSet"
                                             L"\\Control\\ComputerName\\ComputerName",
                                             L"ComputerName",
                                             szShortName);

        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }
}

/*
 * @implemented
 */
BOOL
WINAPI
SetComputerNameExA(COMPUTER_NAME_FORMAT NameType,
                   LPCSTR lpBuffer)
{
    UNICODE_STRING Buffer;
    BOOL bResult;

    RtlCreateUnicodeStringFromAsciiz(&Buffer, (LPSTR)lpBuffer);

    bResult = SetComputerNameExW(NameType, Buffer.Buffer);

    RtlFreeUnicodeString(&Buffer);

    return bResult;
}
/* EOF */
