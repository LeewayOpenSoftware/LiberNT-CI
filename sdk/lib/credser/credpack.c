/*
 * PROJECT:     LiberNT Credential Serialization Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     credui CredPackAuthenticationBufferW and CredUnPackAuthenticationBufferW
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <stdarg.h>
#include <string.h>
#include <windef.h>
#include <winbase.h>
#include <wincred.h>
#include <ntsecapi.h>
#include <credser.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(credui);

#define CREDPACK_UNSUPPORTED_FLAGS (CRED_PACK_WOW_BUFFER | CRED_PACK_ID_PROVIDER_CREDENTIALS)

static const CREDSER_LIMITS CredPackLimits =
    {CREDSER_MAX_NAME_CHARS, CREDSER_MAX_CREDUI_USER_CHARS, CREDSER_MAX_CREDUI_PASSWORD_CHARS};

static VOID
CredPackFreeSecret(_In_opt_ _Post_invalid_ LPWSTR Secret, _In_ DWORD Chars)
{
    if (!Secret)
        return;
    SecureZeroMemory(Secret, Chars * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, Secret);
}

static DWORD
CredPackProtect(_In_ LPWSTR Password, _Outptr_result_buffer_(*Chars) LPWSTR *Protected, _Out_ DWORD *Chars)
{
    DWORD Need = 0, Size, Length = (DWORD)wcslen(Password) + 1;

    *Protected = NULL;
    *Chars = 0;
    if (CredProtectW(FALSE, Password, Length, NULL, &Need, NULL))
        return ERROR_INVALID_PARAMETER;
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return GetLastError();
    *Protected = HeapAlloc(GetProcessHeap(), 0, Need * sizeof(WCHAR));
    if (!*Protected)
        return ERROR_OUTOFMEMORY;
    *Chars = Size = Need;
    if (!CredProtectW(FALSE, Password, Length, *Protected, &Size, NULL))
        return GetLastError();
    return ERROR_SUCCESS;
}

static BOOL
CredPackFail(_In_ DWORD Error)
{
    SetLastError(Error);
    return FALSE;
}

static BOOL
CredPackGeneric(
    _In_ LPWSTR pszUserName,
    _In_ LPWSTR pszPassword,
    _Out_writes_bytes_opt_(*pcbPackedCredentials) PBYTE pPackedCredentials,
    _Inout_ DWORD *pcbPackedCredentials)
{
    ULONG Size = pPackedCredentials ? *pcbPackedCredentials : 0;
    NTSTATUS Status;

    Status = CredSerPackGeneric(pszUserName, pszPassword, pPackedCredentials, &Size);
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        *pcbPackedCredentials = Size;
        return CredPackFail(ERROR_INSUFFICIENT_BUFFER);
    }
    if (Status == STATUS_NOT_SUPPORTED)
    {
        *pcbPackedCredentials = 0;
        return CredPackFail(ERROR_NOT_SUPPORTED);
    }
    if (!NT_SUCCESS(Status))
        return CredPackFail(ERROR_INVALID_PARAMETER);
    /* Windows leaves *pcbPackedCredentials at the caller's buffer size when a generic pack succeeds. */
    return TRUE;
}

BOOL WINAPI
CredPackAuthenticationBufferW(
    DWORD dwFlags,
    LPWSTR pszUserName,
    LPWSTR pszPassword,
    PBYTE pPackedCredentials,
    DWORD *pcbPackedCredentials)
{
    WCHAR Domain[CREDSER_MAX_NAME_CHARS + 1];
    PCWSTR User, Separator;
    LPWSTR Protected = NULL;
    SIZE_T DomainChars;
    DWORD ProtectedChars = 0, Error;
    ULONG Size;
    NTSTATUS Status;

    TRACE("(%08lx, %s, %p, %p, %p)\n", dwFlags, debugstr_w(pszUserName), pszPassword, pPackedCredentials,
          pcbPackedCredentials);

    if (!pszUserName || !pszPassword || !pcbPackedCredentials)
        return CredPackFail(ERROR_INVALID_PARAMETER);
    if (dwFlags & CREDPACK_UNSUPPORTED_FLAGS)
    {
        /* TODO: WOW and identity-provider buffers. */
        FIXME("unsupported flags %08lx\n", dwFlags);
        return CredPackFail(ERROR_NOT_SUPPORTED);
    }
    if (dwFlags & CRED_PACK_GENERIC_CREDENTIALS)
        return CredPackGeneric(pszUserName, pszPassword, pPackedCredentials, pcbPackedCredentials);
    if (wcslen(pszUserName) > CREDSER_MAX_CREDUI_USER_CHARS)
    {
        if (!pPackedCredentials)
            *pcbPackedCredentials = 0;
        return CredPackFail(ERROR_NO_SYSTEM_RESOURCES);
    }

    User = pszUserName;
    Domain[0] = UNICODE_NULL;
    Separator = wcschr(pszUserName, L'\\');
    if (Separator)
    {
        DomainChars = Separator - pszUserName;
        if (DomainChars > CREDSER_MAX_NAME_CHARS)
            return CredPackFail(ERROR_INVALID_PARAMETER);
        CopyMemory(Domain, pszUserName, DomainChars * sizeof(WCHAR));
        Domain[DomainChars] = UNICODE_NULL;
        User = Separator + 1;
    }

    if (dwFlags & CRED_PACK_PROTECTED_CREDENTIALS)
    {
        Error = CredPackProtect(pszPassword, &Protected, &ProtectedChars);
        if (Error != ERROR_SUCCESS)
        {
            CredPackFreeSecret(Protected, ProtectedChars);
            return CredPackFail(Error);
        }
    }

    Size = pPackedCredentials ? *pcbPackedCredentials : 0;
    Status = CredSerPackLogonEx(KerbInteractiveLogon, Domain[0] ? Domain : NULL, User,
                                Protected ? Protected : pszPassword, NULL, &CredPackLimits, pPackedCredentials, &Size);
    CredPackFreeSecret(Protected, ProtectedChars);
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        *pcbPackedCredentials = Size;
        return CredPackFail(ERROR_INSUFFICIENT_BUFFER);
    }
    if (!NT_SUCCESS(Status))
        return CredPackFail(ERROR_INVALID_PARAMETER);

    *pcbPackedCredentials = Size;
    return TRUE;
}

static BOOL
CredUnPackCopy(_Out_writes_opt_(Capacity) LPWSTR Out, _In_ DWORD Capacity,
               _In_opt_ const UNICODE_STRING *Prefix, _In_ const UNICODE_STRING *String)
{
    DWORD At = 0;

    if (!Out)
        return FALSE;
    if (Prefix && Prefix->Length)
    {
        CopyMemory(Out, Prefix->Buffer, Prefix->Length);
        At = Prefix->Length / sizeof(WCHAR);
        Out[At++] = L'\\';
    }
    if (At + String->Length / sizeof(WCHAR) >= Capacity)
        return FALSE;
    CopyMemory(Out + At, String->Buffer, String->Length);
    Out[At + String->Length / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

static BOOL
CredUnPackGeneric(
    _In_reads_bytes_(Size) const VOID *Copy,
    _In_ DWORD Size,
    _Out_writes_opt_(*pcchMaxUserName) LPWSTR pszUserName,
    _Inout_ DWORD *pcchMaxUserName,
    _Inout_opt_ DWORD *pcchMaxDomainName,
    _Out_writes_opt_(*pcchMaxPassword) LPWSTR pszPassword,
    _Inout_ DWORD *pcchMaxPassword)
{
    CREDSER_GENERIC Generic;
    DWORD NeedUser, NeedPassword;
    BOOL Ret;

    if (!NT_SUCCESS(CredSerUnpackGeneric(Copy, Size, &Generic)))
        return CredPackFail(ERROR_INVALID_PARAMETER);

    NeedUser = Generic.UserName.Length / sizeof(WCHAR) + 1;
    NeedPassword = Generic.Password.Length / sizeof(WCHAR) + 1;
    Ret = *pcchMaxUserName >= NeedUser && *pcchMaxPassword >= NeedPassword &&
          CredUnPackCopy(pszUserName, *pcchMaxUserName, NULL, &Generic.UserName) &&
          CredUnPackCopy(pszPassword, *pcchMaxPassword, NULL, &Generic.Password);

    *pcchMaxUserName = NeedUser;
    *pcchMaxPassword = NeedPassword;
    /* Windows reports no domain for a generic buffer, on success as well. */
    if (pcchMaxDomainName)
        *pcchMaxDomainName = 0;
    return Ret ? TRUE : CredPackFail(ERROR_INSUFFICIENT_BUFFER);
}

/* Returns a heap copy of the plaintext when Password is protected, NULL with ERROR_SUCCESS when it is not. */
static DWORD
CredUnPackUnprotect(_In_ const UNICODE_STRING *Password, _Outptr_result_maybenull_ LPWSTR *Plain,
                    _Out_ DWORD *PlainChars)
{
    CRED_PROTECTION_TYPE Type;
    LPWSTR Text;
    DWORD Chars = Password->Length / sizeof(WCHAR), Need = 0, Size, Error = ERROR_SUCCESS;

    *Plain = NULL;
    *PlainChars = 0;
    Text = HeapAlloc(GetProcessHeap(), 0, (Chars + 1) * sizeof(WCHAR));
    if (!Text)
        return ERROR_OUTOFMEMORY;
    CopyMemory(Text, Password->Buffer, Password->Length);
    Text[Chars] = UNICODE_NULL;

    if (!CredIsProtectedW(Text, &Type) || Type == CredUnprotected)
        goto Done;
    if (CredUnprotectW(FALSE, Text, Chars + 1, NULL, &Need) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        Error = GetLastError();
        goto Done;
    }
    *Plain = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (Need + 1) * sizeof(WCHAR));
    if (!*Plain)
    {
        Error = ERROR_OUTOFMEMORY;
        goto Done;
    }
    *PlainChars = Need + 1;
    Size = Need + 1;
    if (!CredUnprotectW(FALSE, Text, Chars + 1, *Plain, &Size))
        Error = GetLastError();

Done:
    CredPackFreeSecret(Text, Chars + 1);
    return Error;
}

BOOL WINAPI
CredUnPackAuthenticationBufferW(
    DWORD dwFlags,
    PVOID pAuthBuffer,
    DWORD cbAuthBuffer,
    LPWSTR pszUserName,
    DWORD *pcchMaxUserName,
    LPWSTR pszDomainName,
    DWORD *pcchMaxDomainName,
    LPWSTR pszPassword,
    DWORD *pcchMaxPassword)
{
    CREDSER_LOGON Logon;
    UNICODE_STRING Password;
    LPWSTR Plain = NULL;
    PVOID Copy;
    DWORD NeedUser, NeedPassword, PlainChars = 0, Error;
    NTSTATUS Status;
    BOOL Ret;

    TRACE("(%08lx, %p, %lu, %p, %p, %p, %p, %p, %p)\n", dwFlags, pAuthBuffer, cbAuthBuffer, pszUserName,
          pcchMaxUserName, pszDomainName, pcchMaxDomainName, pszPassword, pcchMaxPassword);

    if (!pAuthBuffer || !pcchMaxUserName || !pcchMaxPassword)
        return CredPackFail(ERROR_INVALID_PARAMETER);

    Status = CredSerCapture(pAuthBuffer, cbAuthBuffer, &Copy);
    if (!NT_SUCCESS(Status))
        return CredPackFail(ERROR_INVALID_PARAMETER);
    if (CredSerIsGeneric(Copy, cbAuthBuffer))
    {
        Ret = CredUnPackGeneric(Copy, cbAuthBuffer, pszUserName, pcchMaxUserName, pcchMaxDomainName, pszPassword,
                                pcchMaxPassword);
        CredSerFree(Copy, cbAuthBuffer);
        return Ret;
    }
    Status = CredSerUnpackLogonEx(Copy, cbAuthBuffer, &CredPackLimits, &Logon);
    if (!NT_SUCCESS(Status))
    {
        CredSerFree(Copy, cbAuthBuffer);
        return CredPackFail(ERROR_INVALID_PARAMETER);
    }

    Password = Logon.Password;
    if (dwFlags & CRED_PACK_PROTECTED_CREDENTIALS)
    {
        Error = CredUnPackUnprotect(&Logon.Password, &Plain, &PlainChars);
        if (Error != ERROR_SUCCESS)
        {
            CredPackFreeSecret(Plain, PlainChars);
            CredSerFree(Copy, cbAuthBuffer);
            return CredPackFail(Error);
        }
        if (Plain)
        {
            Password.Buffer = Plain;
            Password.Length = Password.MaximumLength = (USHORT)(wcslen(Plain) * sizeof(WCHAR));
        }
    }

    /* Windows sizes the user name with room for a domain separator even without a domain. */
    NeedUser = (Logon.LogonDomainName.Length + Logon.UserName.Length) / sizeof(WCHAR) + 2;
    NeedPassword = Password.Length / sizeof(WCHAR) + 1;

    Ret = *pcchMaxUserName >= NeedUser && *pcchMaxPassword >= NeedPassword &&
          CredUnPackCopy(pszUserName, *pcchMaxUserName, &Logon.LogonDomainName, &Logon.UserName) &&
          CredUnPackCopy(pszPassword, *pcchMaxPassword, NULL, &Password);

    *pcchMaxUserName = NeedUser;
    *pcchMaxPassword = NeedPassword;
    CredPackFreeSecret(Plain, PlainChars);
    CredSerFree(Copy, cbAuthBuffer);
    if (!Ret)
    {
        if (pcchMaxDomainName)
            *pcchMaxDomainName = 0;
        return CredPackFail(ERROR_INSUFFICIENT_BUFFER);
    }
    return TRUE;
}
