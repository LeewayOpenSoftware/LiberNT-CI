/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The user array handed to credential providers (ICredentialProviderUserArray)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include <lm.h>
#include <sddl.h>
#include <propkey.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

#define LUH_SID_CHARS 192   /* S-1-<auth>- and 15 sub-authorities, with room to spare */

/* The local-account user provider: the only kind of user the host enumerates. */
static const GUID LuhLocalUserProvider =
    { 0xa198529b, 0x730f, 0x4089, { 0xb6, 0x46, 0xa1, 0x25, 0x57, 0xf5, 0x66, 0x5e } };

typedef struct _LUH_USER
{
    ICredentialProviderUser ICredentialProviderUser_iface;
    LONG Ref;
    WCHAR Name[UNLEN + 1];
    WCHAR Qualified[MAX_COMPUTERNAME_LENGTH + UNLEN + 2];
    WCHAR Sid[LUH_SID_CHARS];
} LUH_USER;

typedef struct _LUH_USER_ARRAY
{
    ICredentialProviderUserArray ICredentialProviderUserArray_iface;
    LONG Ref;
    BOOL Filtered;              /* a provider filter other than the local-user provider: no users */
    DWORD Count;
    LUH_USER *Users[LUH_MAX_USERS];
} LUH_USER_ARRAY;

static HRESULT
DupString(
    _In_ PCWSTR Source,
    _Outptr_ LPWSTR *Out)
{
    SIZE_T Bytes = (wcslen(Source) + 1) * sizeof(WCHAR);

    *Out = CoTaskMemAlloc(Bytes);
    if (*Out == NULL)
        return E_OUTOFMEMORY;
    CopyMemory(*Out, Source, Bytes);
    return S_OK;
}

/* ---- ICredentialProviderUser ---------------------------------------------------- */

static LUH_USER *
UserFrom(ICredentialProviderUser *iface)
{
    return CONTAINING_RECORD(iface, LUH_USER, ICredentialProviderUser_iface);
}

static HRESULT WINAPI
User_QueryInterface(ICredentialProviderUser *iface, REFIID riid, void **ppv)
{
    if (ppv == NULL)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProviderUser))
    {
        *ppv = iface;
        ICredentialProviderUser_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
User_AddRef(ICredentialProviderUser *iface)
{
    return InterlockedIncrement(&UserFrom(iface)->Ref);
}

static ULONG WINAPI
User_Release(ICredentialProviderUser *iface)
{
    LUH_USER *This = UserFrom(iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);

    if (Ref == 0)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static HRESULT WINAPI
User_GetSid(ICredentialProviderUser *iface, LPWSTR *sid)
{
    if (sid == NULL)
        return E_POINTER;
    *sid = NULL;
    if (UserFrom(iface)->Sid[0] == UNICODE_NULL)
        return E_FAIL;
    return DupString(UserFrom(iface)->Sid, sid);
}

static HRESULT WINAPI
User_GetProviderID(ICredentialProviderUser *iface, GUID *providerID)
{
    if (providerID == NULL)
        return E_POINTER;
    *providerID = LuhLocalUserProvider;
    return S_OK;
}

static HRESULT WINAPI
User_GetStringValue(ICredentialProviderUser *iface, REFPROPERTYKEY key, LPWSTR *stringValue)
{
    LUH_USER *This = UserFrom(iface);

    if (key == NULL || stringValue == NULL)
        return E_POINTER;
    *stringValue = NULL;
    /* A local account has no separate display name here: the account name stands in, as on the msgina page. */
    if (IsEqualPropertyKey(*key, PKEY_Identity_UserName) || IsEqualPropertyKey(*key, PKEY_Identity_DisplayName))
        return DupString(This->Name, stringValue);
    if (IsEqualPropertyKey(*key, PKEY_Identity_QualifiedUserName))
        return DupString(This->Qualified, stringValue);
    if (IsEqualPropertyKey(*key, PKEY_Identity_PrimarySid))
        return This->Sid[0] ? DupString(This->Sid, stringValue) : E_FAIL;
    return E_INVALIDARG;
}

static HRESULT WINAPI
User_GetValue(ICredentialProviderUser *iface, REFPROPERTYKEY key, PROPVARIANT *value)
{
    LPWSTR Text = NULL;
    HRESULT hr;

    if (value == NULL)
        return E_POINTER;
    PropVariantInit(value);
    hr = User_GetStringValue(iface, key, &Text);
    if (SUCCEEDED(hr))
    {
        value->vt = VT_LPWSTR;
        value->pwszVal = Text;
    }
    return hr;
}

static const ICredentialProviderUserVtbl LuhUserVtbl =
{
    User_QueryInterface,
    User_AddRef,
    User_Release,
    User_GetSid,
    User_GetProviderID,
    User_GetStringValue,
    User_GetValue,
};

/* ---- ICredentialProviderUserArray ----------------------------------------------- */

static LUH_USER_ARRAY *
ArrayFrom(ICredentialProviderUserArray *iface)
{
    return CONTAINING_RECORD(iface, LUH_USER_ARRAY, ICredentialProviderUserArray_iface);
}

static HRESULT WINAPI
Array_QueryInterface(ICredentialProviderUserArray *iface, REFIID riid, void **ppv)
{
    if (ppv == NULL)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProviderUserArray))
    {
        *ppv = iface;
        ICredentialProviderUserArray_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Array_AddRef(ICredentialProviderUserArray *iface)
{
    return InterlockedIncrement(&ArrayFrom(iface)->Ref);
}

static ULONG WINAPI
Array_Release(ICredentialProviderUserArray *iface)
{
    LUH_USER_ARRAY *This = ArrayFrom(iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    DWORD i;

    if (Ref == 0)
    {
        for (i = 0; i < This->Count; i++)
            ICredentialProviderUser_Release(&This->Users[i]->ICredentialProviderUser_iface);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return Ref;
}

static HRESULT WINAPI
Array_SetProviderFilter(ICredentialProviderUserArray *iface, REFGUID guidProviderToFilterTo)
{
    if (guidProviderToFilterTo == NULL)
        return E_POINTER;
    ArrayFrom(iface)->Filtered = !IsEqualGUID(guidProviderToFilterTo, &LuhLocalUserProvider);
    return S_OK;
}

static HRESULT WINAPI
Array_GetAccountOptions(ICredentialProviderUserArray *iface, CREDENTIAL_PROVIDER_ACCOUNT_OPTIONS *options)
{
    if (options == NULL)
        return E_POINTER;
    /* The msgina page always offers "Other user" after the accounts; ask the providers for the same empty tile. */
    *options = CPAO_EMPTY_LOCAL;
    return S_OK;
}

static HRESULT WINAPI
Array_GetCount(ICredentialProviderUserArray *iface, DWORD *userCount)
{
    if (userCount == NULL)
        return E_POINTER;
    *userCount = ArrayFrom(iface)->Filtered ? 0 : ArrayFrom(iface)->Count;
    return S_OK;
}

static HRESULT WINAPI
Array_GetAt(ICredentialProviderUserArray *iface, DWORD userIndex, ICredentialProviderUser **user)
{
    LUH_USER_ARRAY *This = ArrayFrom(iface);

    if (user == NULL)
        return E_POINTER;
    *user = NULL;
    if (This->Filtered || userIndex >= This->Count)
        return E_INVALIDARG;
    *user = &This->Users[userIndex]->ICredentialProviderUser_iface;
    ICredentialProviderUser_AddRef(*user);
    return S_OK;
}

static const ICredentialProviderUserArrayVtbl LuhUserArrayVtbl =
{
    Array_QueryInterface,
    Array_AddRef,
    Array_Release,
    Array_SetProviderFilter,
    Array_GetAccountOptions,
    Array_GetCount,
    Array_GetAt,
};

/* ---- building the array --------------------------------------------------------- */

static VOID
AddUser(
    _Inout_ LUH_USER_ARRAY *Array,
    _In_ PCWSTR Computer,
    _In_ PCWSTR Name)
{
    LUH_USER *User;
    BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
    WCHAR Domain[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD SidSize = sizeof(SidBuffer), DomainChars = ARRAYSIZE(Domain);
    SID_NAME_USE Use;
    LPWSTR SidText = NULL;

    if (Array->Count >= LUH_MAX_USERS || wcslen(Name) > UNLEN)
        return;
    User = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*User));
    if (User == NULL)
        return;
    User->ICredentialProviderUser_iface.lpVtbl = &LuhUserVtbl;
    User->Ref = 1;
    StringCchCopyW(User->Name, ARRAYSIZE(User->Name), Name);
    StringCchPrintfW(User->Qualified, ARRAYSIZE(User->Qualified), L"%s\\%s", Computer, Name);
    if (LookupAccountNameW(NULL, User->Qualified, SidBuffer, &SidSize, Domain, &DomainChars, &Use) &&
        ConvertSidToStringSidW((PSID)SidBuffer, &SidText))
    {
        StringCchCopyW(User->Sid, ARRAYSIZE(User->Sid), SidText);
        LocalFree(SidText);
    }
    Array->Users[Array->Count++] = User;
}

/*
 * Logon: the accounts the msgina page lists, every enabled normal local account (msgina
 * LoadUsers). Unlock: only the locked local account Only names (DOMAIN\user, the domain being
 * this computer), as msgina lists only the session's user; without such a name, none at all,
 * so nothing is enumerated on the lock screen.
 */
HRESULT
LuhUsersCreate(
    _In_ BOOL Unlock,
    _In_opt_ PCWSTR Only,
    _Outptr_ ICredentialProviderUserArray **Out)
{
    LUH_USER_ARRAY *Array;
    PUSER_INFO_1 Info;
    DWORD Read, Total, Resume = 0, i;
    NET_API_STATUS Status;
    WCHAR Computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD ComputerChars = ARRAYSIZE(Computer);
    PCWSTR OnlyName = NULL;

    *Out = NULL;
    if (!GetComputerNameW(Computer, &ComputerChars))
        return HRESULT_FROM_WIN32(GetLastError());
    if (Only != NULL && Only[0] != UNICODE_NULL)
    {
        PCWSTR Slash = wcschr(Only, L'\\');

        /* DOMAIN\user: only a local account (domain = this computer) can match a local tile;
         * a domain account of the same name must not, so it gets no tile here. */
        if (Slash == NULL)
            OnlyName = Only;
        else if ((Slash - Only == 1 && Only[0] == L'.') ||      /* a lone dot is this computer too */
                 ((SIZE_T)(Slash - Only) == wcslen(Computer) && _wcsnicmp(Only, Computer, Slash - Only) == 0))
            OnlyName = Slash + 1;
    }

    Array = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Array));
    if (Array == NULL)
        return E_OUTOFMEMORY;
    Array->ICredentialProviderUserArray_iface.lpVtbl = &LuhUserArrayVtbl;
    Array->Ref = 1;
    if (Unlock && OnlyName == NULL)
    {
        TRACE("unlock without an account name: no users\n");
        *Out = &Array->ICredentialProviderUserArray_iface;
        return S_OK;
    }

    do
    {
        Info = NULL;
        Read = 0;
        Status = NetUserEnum(NULL, 1, FILTER_NORMAL_ACCOUNT, (LPBYTE *)&Info, MAX_PREFERRED_LENGTH, &Read, &Total,
                             &Resume);
        if (Status != NERR_Success && Status != ERROR_MORE_DATA)
            break;
        for (i = 0; Info != NULL && i < Read; i++)
        {
            if (Info[i].usri1_flags & UF_ACCOUNTDISABLE)
                continue;
            if (Unlock && _wcsicmp(Info[i].usri1_name, OnlyName) != 0)
                continue;
            AddUser(Array, Computer, Info[i].usri1_name);
        }
        if (Info != NULL)
            NetApiBufferFree(Info);
    } while (Status == ERROR_MORE_DATA && Read);

    TRACE("%lu users (status %lu)\n", Array->Count, Status);
    *Out = &Array->ICredentialProviderUserArray_iface;
    return S_OK;
}
