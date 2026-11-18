/*
 * Copyright 2005 Kees Cook <kees@outflux.net>
 * Copyright 2026 LeewayOpenSoftware
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * CryptProtectData and CryptUnprotectData forward the work to the LSA DPAPI
 * server (dll/win32/lsasrv/dpapi.c) over RPC, so protection is keyed by the
 * caller's logon identity (or, with CRYPTPROTECT_LOCAL_MACHINE, by a machine
 * key kept by LSA) rather than by a local secret in this process.
 *
 * The data description round-trips inside the protected payload: it is
 * prepended to the plaintext before protection and split back out on
 * unprotection, so it is covered by the server's integrity check. pPromptStruct
 * is ignored. No Windows master-key blob-format compatibility yet.
 */

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "rpc.h"
#include "dpapi_c.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((LONG)0x00000000)
#endif

ULONG WINAPI RtlNtStatusToDosError(LONG Status);

void * __RPC_USER MIDL_user_allocate(SIZE_T len)
{
    return HeapAlloc(GetProcessHeap(), 0, len);
}

void __RPC_USER MIDL_user_free(void *ptr)
{
    HeapFree(GetProcessHeap(), 0, ptr);
}

static handle_t dpapi_bind(void)
{
    RPC_WSTR string_binding = NULL;
    handle_t binding = NULL;
    RPC_STATUS status;

    status = RpcStringBindingComposeW(NULL,
                                      (RPC_WSTR)L"ncacn_np",
                                      NULL,
                                      (RPC_WSTR)L"\\pipe\\lsarpc",
                                      NULL,
                                      &string_binding);
    if (status != RPC_S_OK)
        return NULL;

    status = RpcBindingFromStringBindingW(string_binding, &binding);
    RpcStringFreeW(&string_binding);
    if (status != RPC_S_OK)
        return NULL;

    return binding;
}

static void dpapi_unbind(handle_t binding)
{
    if (binding != NULL)
        RpcBindingFree(&binding);
}

/* Map the server NTSTATUS onto a Win32 error. A failed unprotect is reported as
 * ERROR_INVALID_DATA (bad, tampered or foreign blob, or a key the caller cannot
 * derive), matching what callers expect from a blob they cannot open. */
static DWORD dpapi_protect_error(LONG status)
{
    if (status == STATUS_SUCCESS)
        return ERROR_SUCCESS;
    return RtlNtStatusToDosError(status);
}

/* Prepend the description to the plaintext: [cbDescr: DWORD][descr WCHARs incl. NUL][data]. */
static BOOL dpapi_wrap_plaintext(LPCWSTR szDataDescr, const DATA_BLOB *pDataIn,
                                 BYTE **ppWrapped, DWORD *pcbWrapped)
{
    DWORD cbDescr, cbData, cbTotal;
    BYTE *wrapped;

    if (szDataDescr == NULL)
        szDataDescr = L"";

    cbDescr = (lstrlenW(szDataDescr) + 1) * sizeof(WCHAR);
    cbData = pDataIn->cbData;
    if (cbData > (DWORD)~0u - cbDescr - sizeof(DWORD))
        return FALSE;

    cbTotal = sizeof(DWORD) + cbDescr + cbData;
    wrapped = HeapAlloc(GetProcessHeap(), 0, cbTotal);
    if (wrapped == NULL)
        return FALSE;

    memcpy(wrapped, &cbDescr, sizeof(DWORD));
    memcpy(wrapped + sizeof(DWORD), szDataDescr, cbDescr);
    if (cbData != 0)
        memcpy(wrapped + sizeof(DWORD) + cbDescr, pDataIn->pbData, cbData);

    *ppWrapped = wrapped;
    *pcbWrapped = cbTotal;
    return TRUE;
}

/* Reverse of dpapi_wrap_plaintext. The description goes to ppszDataDescr (LocalAlloc),
 * the remaining bytes to pDataOut (LocalAlloc). */
static BOOL dpapi_unwrap_plaintext(const BYTE *pWrapped, DWORD cbWrapped,
                                   LPWSTR *ppszDataDescr, DATA_BLOB *pDataOut)
{
    DWORD cbDescr, cbData;

    if (cbWrapped < sizeof(DWORD))
        return FALSE;

    memcpy(&cbDescr, pWrapped, sizeof(DWORD));
    if (cbDescr < sizeof(WCHAR) ||
        (cbDescr % sizeof(WCHAR)) != 0 ||
        cbDescr > cbWrapped - sizeof(DWORD))
    {
        return FALSE;
    }

    cbData = cbWrapped - sizeof(DWORD) - cbDescr;

    pDataOut->pbData = LocalAlloc(LPTR, cbData != 0 ? cbData : 1);
    if (pDataOut->pbData == NULL)
        return FALSE;
    if (cbData != 0)
        memcpy(pDataOut->pbData, pWrapped + sizeof(DWORD) + cbDescr, cbData);
    pDataOut->cbData = cbData;

    if (ppszDataDescr != NULL)
    {
        *ppszDataDescr = LocalAlloc(LPTR, cbDescr);
        if (*ppszDataDescr == NULL)
        {
            LocalFree(pDataOut->pbData);
            pDataOut->pbData = NULL;
            pDataOut->cbData = 0;
            return FALSE;
        }
        memcpy(*ppszDataDescr, pWrapped + sizeof(DWORD), cbDescr);
        ((LPWSTR)*ppszDataDescr)[cbDescr / sizeof(WCHAR) - 1] = L'\0';
    }

    return TRUE;
}

/***************************************************************************
 * CryptProtectData     [CRYPT32.@]
 */
BOOL WINAPI CryptProtectData(DATA_BLOB* pDataIn,
                             LPCWSTR szDataDescr,
                             DATA_BLOB* pOptionalEntropy,
                             PVOID pvReserved,
                             CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct,
                             DWORD dwFlags,
                             DATA_BLOB* pDataOut)
{
    handle_t binding;
    DPAPI_BLOB data_in = {0}, entropy = {0}, data_out = {0};
    BYTE *wrapped = NULL;
    DWORD cbWrapped = 0;
    LONG status = STATUS_SUCCESS;
    BOOL rc = FALSE;

    TRACE("(%p %s %p %p %p %08lx %p)\n", pDataIn, debugstr_w(szDataDescr),
          pOptionalEntropy, pvReserved, pPromptStruct, dwFlags, pDataOut);

    if (!pDataIn || !pDataOut || pvReserved != NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    pDataOut->cbData = 0;
    pDataOut->pbData = NULL;

    if (!dpapi_wrap_plaintext(szDataDescr, pDataIn, &wrapped, &cbWrapped))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    binding = dpapi_bind();
    if (binding == NULL)
    {
        SecureZeroMemory(wrapped, cbWrapped);
        HeapFree(GetProcessHeap(), 0, wrapped);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    data_in.cbData = cbWrapped;
    data_in.pbData = wrapped;
    if (pOptionalEntropy != NULL)
    {
        entropy.cbData = pOptionalEntropy->cbData;
        entropy.pbData = pOptionalEntropy->pbData;
    }

    RpcTryExcept
    {
        status = s_DpapiProtect(binding, dwFlags, NULL, data_in, entropy, &data_out);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    if (status == STATUS_SUCCESS)
    {
        pDataOut->pbData = LocalAlloc(LPTR, data_out.cbData != 0 ? data_out.cbData : 1);
        if (pDataOut->pbData != NULL)
        {
            if (data_out.cbData != 0)
                memcpy(pDataOut->pbData, data_out.pbData, data_out.cbData);
            pDataOut->cbData = data_out.cbData;
            rc = TRUE;
        }
        else
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    else
    {
        SetLastError(dpapi_protect_error(status));
    }

    if (data_out.pbData != NULL)
        MIDL_user_free(data_out.pbData);
    SecureZeroMemory(wrapped, cbWrapped);
    HeapFree(GetProcessHeap(), 0, wrapped);
    dpapi_unbind(binding);

    if (rc)
        SetLastError(ERROR_SUCCESS);

    TRACE("returning %d\n", rc);
    return rc;
}

/***************************************************************************
 * CryptUnprotectData   [CRYPT32.@]
 */
BOOL WINAPI CryptUnprotectData(DATA_BLOB* pDataIn,
                               LPWSTR * ppszDataDescr,
                               DATA_BLOB* pOptionalEntropy,
                               PVOID pvReserved,
                               CRYPTPROTECT_PROMPTSTRUCT* pPromptStruct,
                               DWORD dwFlags,
                               DATA_BLOB* pDataOut)
{
    handle_t binding;
    DPAPI_BLOB data_in = {0}, entropy = {0}, data_out = {0};
    LONG status = STATUS_SUCCESS;
    BOOL rc = FALSE;

    TRACE("(%p %p %p %p %p %08lx %p)\n", pDataIn, ppszDataDescr,
          pOptionalEntropy, pvReserved, pPromptStruct, dwFlags, pDataOut);

    if (ppszDataDescr != NULL)
        *ppszDataDescr = NULL;

    if (!pDataIn || !pDataOut || pvReserved != NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    pDataOut->cbData = 0;
    pDataOut->pbData = NULL;

    binding = dpapi_bind();
    if (binding == NULL)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    data_in.cbData = pDataIn->cbData;
    data_in.pbData = pDataIn->pbData;
    if (pOptionalEntropy != NULL)
    {
        entropy.cbData = pOptionalEntropy->cbData;
        entropy.pbData = pOptionalEntropy->pbData;
    }

    RpcTryExcept
    {
        status = s_DpapiUnprotect(binding, dwFlags, data_in, entropy, &data_out);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    if (status == STATUS_SUCCESS)
    {
        if (dpapi_unwrap_plaintext(data_out.pbData, data_out.cbData, ppszDataDescr, pDataOut))
            rc = TRUE;
        else
            SetLastError(ERROR_INVALID_DATA);
    }
    else
    {
        SetLastError(ERROR_INVALID_DATA);
    }

    if (data_out.pbData != NULL)
    {
        SecureZeroMemory(data_out.pbData, data_out.cbData);
        MIDL_user_free(data_out.pbData);
    }
    dpapi_unbind(binding);

    if (rc)
        SetLastError(ERROR_SUCCESS);

    TRACE("returning %d\n", rc);
    return rc;
}
