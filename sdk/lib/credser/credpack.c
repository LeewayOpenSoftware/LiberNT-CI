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

#define CREDPACK_UNSUPPORTED_FLAGS \
    (CRED_PACK_PROTECTED_CREDENTIALS | CRED_PACK_WOW_BUFFER | CRED_PACK_GENERIC_CREDENTIALS | \
     CRED_PACK_ID_PROVIDER_CREDENTIALS)

static BOOL
CredPackFail(_In_ DWORD Error)
{
    SetLastError(Error);
    return FALSE;
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
    SIZE_T DomainChars;
    ULONG Size;
    NTSTATUS Status;

    TRACE("(%08lx, %s, %p, %p, %p)\n", dwFlags, debugstr_w(pszUserName), pszPassword, pPackedCredentials,
          pcbPackedCredentials);

    if (!pszUserName || !pszPassword || !pcbPackedCredentials)
        return CredPackFail(ERROR_INVALID_PARAMETER);
    if (dwFlags & CREDPACK_UNSUPPORTED_FLAGS)
    {
        /* TODO: protected, WOW, generic and identity-provider buffers. */
        FIXME("unsupported flags %08lx\n", dwFlags);
        return CredPackFail(ERROR_NOT_SUPPORTED);
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

    Size = pPackedCredentials ? *pcbPackedCredentials : 0;
    Status = CredSerPackLogon(KerbInteractiveLogon, Domain[0] ? Domain : NULL, User, pszPassword, NULL,
                              pPackedCredentials, &Size);
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
    PVOID Copy;
    DWORD NeedUser, NeedPassword;
    NTSTATUS Status;
    BOOL Ret;

    TRACE("(%08lx, %p, %lu, %p, %p, %p, %p, %p, %p)\n", dwFlags, pAuthBuffer, cbAuthBuffer, pszUserName,
          pcchMaxUserName, pszDomainName, pcchMaxDomainName, pszPassword, pcchMaxPassword);

    if (!pAuthBuffer || !pcchMaxUserName || !pcchMaxPassword)
        return CredPackFail(ERROR_INVALID_PARAMETER);

    Status = CredSerCapture(pAuthBuffer, cbAuthBuffer, &Copy);
    if (!NT_SUCCESS(Status))
        return CredPackFail(ERROR_INVALID_PARAMETER);
    Status = CredSerUnpackLogon(Copy, cbAuthBuffer, &Logon);
    if (!NT_SUCCESS(Status))
    {
        CredSerFree(Copy, cbAuthBuffer);
        return CredPackFail(ERROR_INVALID_PARAMETER);
    }

    /* Windows sizes the user name with room for a domain separator even without a domain. */
    NeedUser = (Logon.LogonDomainName.Length + Logon.UserName.Length) / sizeof(WCHAR) + 2;
    NeedPassword = Logon.Password.Length / sizeof(WCHAR) + 1;

    Ret = *pcchMaxUserName >= NeedUser && *pcchMaxPassword >= NeedPassword &&
          CredUnPackCopy(pszUserName, *pcchMaxUserName, &Logon.LogonDomainName, &Logon.UserName) &&
          CredUnPackCopy(pszPassword, *pcchMaxPassword, NULL, &Logon.Password);

    *pcchMaxUserName = NeedUser;
    *pcchMaxPassword = NeedPassword;
    CredSerFree(Copy, cbAuthBuffer);
    if (!Ret)
    {
        if (pcchMaxDomainName)
            *pcchMaxDomainName = 0;
        return CredPackFail(ERROR_INSUFFICIENT_BUFFER);
    }
    return TRUE;
}
