/*
 * PROJECT:     LiberNT Credential Providers
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Password credential tile
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "precomp.h"

static PWCREDENTIAL *
CredentialFrom(ICredentialProviderCredential2 *iface)
{
    return CONTAINING_RECORD(iface, PWCREDENTIAL, ICredentialProviderCredential2_iface);
}

static BOOL
IsSecretField(_In_ DWORD Field)
{
    return Field == PWFIELD_PASSWORD || Field == PWFIELD_NEW_PASSWORD || Field == PWFIELD_CONFIRM_PASSWORD;
}

static VOID
ClearSecrets(_Inout_ PWCREDENTIAL *This, _In_ BOOL Notify)
{
    DWORD Field;

    for (Field = PWFIELD_PASSWORD; Field <= PWFIELD_CONFIRM_PASSWORD; Field++)
    {
        SecureZeroMemory(This->Text[Field], sizeof(This->Text[Field]));
        if (Notify && This->Events)
        {
            ICredentialProviderCredentialEvents_SetFieldString(This->Events,
                (ICredentialProviderCredential *)&This->ICredentialProviderCredential2_iface, Field, L"");
        }
    }
}

static VOID
LoadText(_In_ UINT Id, _Out_writes_(Chars) LPWSTR Out, _In_ DWORD Chars)
{
    if (!LoadStringW(CredProvsInstance, Id, Out, Chars))
        Out[0] = UNICODE_NULL;
}

static HRESULT
LoadTextAlloc(_In_ UINT Id, _Outptr_ LPWSTR *Out)
{
    WCHAR Text[128];

    LoadText(Id, Text, ARRAYSIZE(Text));
    return PasswordProvider_DupString(Text, Out);
}

static HRESULT WINAPI
Credential_QueryInterface(ICredentialProviderCredential2 *iface, REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_ICredentialProviderCredential) ||
        IsEqualGUID(riid, &IID_ICredentialProviderCredential2))
    {
        *ppv = iface;
        ICredentialProviderCredential2_AddRef(iface);
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_ICredentialProviderCredentialWithFieldOptions))
    {
        *ppv = &CredentialFrom(iface)->ICredentialProviderCredentialWithFieldOptions_iface;
        ICredentialProviderCredential2_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI
Credential_AddRef(ICredentialProviderCredential2 *iface)
{
    return InterlockedIncrement(&CredentialFrom(iface)->Ref);
}

static ULONG WINAPI
Credential_Release(ICredentialProviderCredential2 *iface)
{
    PWCREDENTIAL *This = CredentialFrom(iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);

    if (!Ref)
    {
        if (This->Events)
            ICredentialProviderCredentialEvents_Release(This->Events);
        CoTaskMemFree(This->Sid);
        SecureZeroMemory(This, sizeof(*This));
        HeapFree(GetProcessHeap(), 0, This);
        InterlockedDecrement(&CredProvsObjects);
    }
    return Ref;
}

static HRESULT WINAPI
Credential_Advise(ICredentialProviderCredential2 *iface, ICredentialProviderCredentialEvents *pcpce)
{
    PWCREDENTIAL *This = CredentialFrom(iface);

    if (!pcpce)
        return E_INVALIDARG;
    if (This->Events)
        ICredentialProviderCredentialEvents_Release(This->Events);
    This->Events = pcpce;
    ICredentialProviderCredentialEvents_AddRef(pcpce);
    return S_OK;
}

static HRESULT WINAPI
Credential_UnAdvise(ICredentialProviderCredential2 *iface)
{
    PWCREDENTIAL *This = CredentialFrom(iface);

    ClearSecrets(This, FALSE);
    if (This->Events)
    {
        ICredentialProviderCredentialEvents_Release(This->Events);
        This->Events = NULL;
    }
    return S_OK;
}

static HRESULT WINAPI
Credential_SetSelected(ICredentialProviderCredential2 *iface, BOOL *pbAutoLogon)
{
    if (!pbAutoLogon)
        return E_POINTER;
    *pbAutoLogon = FALSE;
    return S_OK;
}

static HRESULT WINAPI
Credential_SetDeselected(ICredentialProviderCredential2 *iface)
{
    ClearSecrets(CredentialFrom(iface), TRUE);
    return S_OK;
}

static HRESULT WINAPI
Credential_GetFieldState(ICredentialProviderCredential2 *iface, DWORD dwFieldID,
                         CREDENTIAL_PROVIDER_FIELD_STATE *pcpfs, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE *pcpfis)
{
    PWCREDENTIAL *This = CredentialFrom(iface);
    BOOL Change = This->Scenario == CPUS_CHANGE_PASSWORD;

    if (!pcpfs || !pcpfis)
        return E_POINTER;
    if (!PasswordProvider_IsKnownFieldId(dwFieldID))
        return E_INVALIDARG;

    *pcpfis = CPFIS_NONE;
    switch (dwFieldID)
    {
        case PWFIELD_FRIENDLY_NAME:
            *pcpfs = CPFS_DISPLAY_IN_BOTH;
            break;
        case PWFIELD_USER_NAME:
            *pcpfs = This->KnownUser ? CPFS_HIDDEN : CPFS_DISPLAY_IN_SELECTED_TILE;
            if (!This->KnownUser)
                *pcpfis = CPFIS_FOCUSED;
            break;
        case PWFIELD_PASSWORD:
            *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
            if (This->KnownUser)
                *pcpfis = CPFIS_FOCUSED;
            break;
        case PWFIELD_NEW_PASSWORD:
        case PWFIELD_CONFIRM_PASSWORD:
            *pcpfs = Change ? CPFS_DISPLAY_IN_SELECTED_TILE : CPFS_HIDDEN;
            break;
        case PWFIELD_SUBMIT:
            *pcpfs = CPFS_DISPLAY_IN_SELECTED_TILE;
            break;
        default:
            *pcpfs = CPFS_HIDDEN;
            break;
    }
    return S_OK;
}

static HRESULT WINAPI
Credential_GetStringValue(ICredentialProviderCredential2 *iface, DWORD dwFieldID, LPWSTR *ppsz)
{
    PWCREDENTIAL *This = CredentialFrom(iface);

    if (!ppsz)
        return E_POINTER;
    *ppsz = NULL;
    if (!PasswordProvider_IsKnownFieldId(dwFieldID) || dwFieldID == PWFIELD_SUBMIT)
        return E_INVALIDARG;
    return PasswordProvider_DupString(This->Text[dwFieldID], ppsz);
}

static HRESULT WINAPI
Credential_GetBitmapValue(ICredentialProviderCredential2 *iface, DWORD dwFieldID, HBITMAP *phbmp)
{
    if (!phbmp)
        return E_POINTER;
    *phbmp = NULL;
    return E_INVALIDARG;
}

static HRESULT WINAPI
Credential_GetCheckboxValue(ICredentialProviderCredential2 *iface, DWORD dwFieldID, BOOL *pbChecked, LPWSTR *ppszLabel)
{
    if (ppszLabel)
        *ppszLabel = NULL;
    return E_INVALIDARG;
}

static HRESULT WINAPI
Credential_GetSubmitButtonValue(ICredentialProviderCredential2 *iface, DWORD dwFieldID, DWORD *pdwAdjacentTo)
{
    PWCREDENTIAL *This = CredentialFrom(iface);

    if (!pdwAdjacentTo)
        return E_POINTER;
    if (dwFieldID != PWFIELD_SUBMIT)
        return E_INVALIDARG;
    *pdwAdjacentTo = (This->Scenario == CPUS_CHANGE_PASSWORD) ? PWFIELD_CONFIRM_PASSWORD : PWFIELD_PASSWORD;
    return S_OK;
}

static HRESULT WINAPI
Credential_GetComboBoxValueCount(ICredentialProviderCredential2 *iface, DWORD dwFieldID, DWORD *pcItems,
                                 DWORD *pdwSelectedItem)
{
    return E_INVALIDARG;
}

static HRESULT WINAPI
Credential_GetComboBoxValueAt(ICredentialProviderCredential2 *iface, DWORD dwFieldID, DWORD dwItem, LPWSTR *ppszItem)
{
    if (ppszItem)
        *ppszItem = NULL;
    return E_INVALIDARG;
}

static HRESULT WINAPI
Credential_SetStringValue(ICredentialProviderCredential2 *iface, DWORD dwFieldID, LPCWSTR psz)
{
    PWCREDENTIAL *This = CredentialFrom(iface);
    SIZE_T Chars;

    if (!psz)
        return E_INVALIDARG;
    if (!PasswordProvider_IsKnownFieldId(dwFieldID))
        return E_INVALIDARG;
    if (dwFieldID != PWFIELD_USER_NAME && !IsSecretField(dwFieldID))
        return E_INVALIDARG;
    if (dwFieldID == PWFIELD_USER_NAME && This->KnownUser)
        return E_INVALIDARG;

    Chars = wcslen(psz);
    if (Chars >= PWFIELD_TEXT_CHARS)
        return E_INVALIDARG;
    SecureZeroMemory(This->Text[dwFieldID], sizeof(This->Text[dwFieldID]));
    CopyMemory(This->Text[dwFieldID], psz, Chars * sizeof(WCHAR));
    return S_OK;
}

static HRESULT WINAPI
Credential_SetCheckboxValue(ICredentialProviderCredential2 *iface, DWORD dwFieldID, BOOL bChecked)
{
    return E_INVALIDARG;
}

static HRESULT WINAPI
Credential_SetComboBoxSelectedValue(ICredentialProviderCredential2 *iface, DWORD dwFieldID, DWORD dwSelectedItem)
{
    return E_INVALIDARG;
}

static HRESULT WINAPI
Credential_CommandLinkClicked(ICredentialProviderCredential2 *iface, DWORD dwFieldID)
{
    return E_INVALIDARG;
}

static VOID
SplitUserName(_In_ PCWSTR Typed, _Out_writes_(PWFIELD_TEXT_CHARS) LPWSTR Domain, _Outptr_ PCWSTR *User)
{
    PCWSTR Separator = wcschr(Typed, L'\\');
    DWORD Chars = PWFIELD_TEXT_CHARS;

    Domain[0] = UNICODE_NULL;
    *User = Typed;
    if (Separator)
    {
        CopyMemory(Domain, Typed, (Separator - Typed) * sizeof(WCHAR));
        Domain[Separator - Typed] = UNICODE_NULL;
        *User = Separator + 1;
    }
    else if (!wcschr(Typed, L'@') && !GetComputerNameW(Domain, &Chars))
    {
        Domain[0] = UNICODE_NULL;
    }
}

static HRESULT
GetAuthPackage(_Out_ PULONG Package)
{
    CREDSER_PACKAGES Packages;
    HANDLE Lsa;
    NTSTATUS Status;

    Status = LsaConnectUntrusted(&Lsa);
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_NT(Status);
    Status = CredSerLookupPackages(Lsa, &Packages);
    LsaDeregisterLogonProcess(Lsa);
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_NT(Status);
    *Package = CredSerPreferredPackage(&Packages);
    return S_OK;
}

static HRESULT
PackSerialization(_In_ PWCREDENTIAL *This, _Out_ CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs)
{
    WCHAR Domain[PWFIELD_TEXT_CHARS];
    PCWSTR User;
    ULONG Size = 0;
    PBYTE Blob;
    NTSTATUS Status;
    HRESULT hr;

    hr = GetAuthPackage(&pcpcs->ulAuthenticationPackage);
    if (FAILED(hr))
        return hr;

    SplitUserName(This->Text[PWFIELD_USER_NAME], Domain, &User);
    /* FIXME: Windows protects the password with CredProtect; LiberNT has no CredProtect yet. */
    if (This->Scenario == CPUS_CHANGE_PASSWORD)
        Status = CredSerPackChangePassword(Domain[0] ? Domain : NULL, User, This->Text[PWFIELD_PASSWORD],
                                           This->Text[PWFIELD_NEW_PASSWORD], NULL, &Size);
    else
        Status = CredSerPackLogon(This->Scenario == CPUS_UNLOCK_WORKSTATION ? KerbWorkstationUnlockLogon : KerbInteractiveLogon,
                                  Domain[0] ? Domain : NULL, User, This->Text[PWFIELD_PASSWORD], NULL, NULL, &Size);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        return HRESULT_FROM_NT(Status);

    Blob = CoTaskMemAlloc(Size);
    if (!Blob)
        return E_OUTOFMEMORY;
    if (This->Scenario == CPUS_CHANGE_PASSWORD)
        Status = CredSerPackChangePassword(Domain[0] ? Domain : NULL, User, This->Text[PWFIELD_PASSWORD],
                                           This->Text[PWFIELD_NEW_PASSWORD], Blob, &Size);
    else
        Status = CredSerPackLogon(This->Scenario == CPUS_UNLOCK_WORKSTATION ? KerbWorkstationUnlockLogon : KerbInteractiveLogon,
                                  Domain[0] ? Domain : NULL, User, This->Text[PWFIELD_PASSWORD], NULL, Blob, &Size);
    if (!NT_SUCCESS(Status))
    {
        SecureZeroMemory(Blob, Size);
        CoTaskMemFree(Blob);
        return HRESULT_FROM_NT(Status);
    }

    pcpcs->clsidCredentialProvider = CLSID_PasswordCredentialProvider;
    pcpcs->cbSerialization = Size;
    pcpcs->rgbSerialization = Blob;
    return S_OK;
}

static HRESULT WINAPI
Credential_GetSerialization(ICredentialProviderCredential2 *iface, CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE *pcpgsr,
                            CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION *pcpcs, LPWSTR *ppszOptionalStatusText,
                            CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    PWCREDENTIAL *This = CredentialFrom(iface);
    HRESULT hr;

    if (!pcpgsr || !pcpcs || !ppszOptionalStatusText || !pcpsiOptionalStatusIcon)
        return E_POINTER;
    *pcpgsr = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
    *ppszOptionalStatusText = NULL;
    *pcpsiOptionalStatusIcon = CPSI_NONE;
    ZeroMemory(pcpcs, sizeof(*pcpcs));

    if (This->Scenario == CPUS_CHANGE_PASSWORD &&
        wcscmp(This->Text[PWFIELD_NEW_PASSWORD], This->Text[PWFIELD_CONFIRM_PASSWORD]))
    {
        *pcpsiOptionalStatusIcon = CPSI_ERROR;
        return LoadTextAlloc(IDS_PASSWORD_MISMATCH, ppszOptionalStatusText);
    }
    if (!This->Text[PWFIELD_USER_NAME][0])
        return S_OK;

    hr = PackSerialization(This, pcpcs);
    if (FAILED(hr))
        return hr;
    *pcpgsr = CPGSR_RETURN_CREDENTIAL_FINISHED;
    return S_OK;
}

static HRESULT WINAPI
Credential_ReportResult(ICredentialProviderCredential2 *iface, NTSTATUS ntsStatus, NTSTATUS ntsSubstatus,
                        LPWSTR *ppszOptionalStatusText, CREDENTIAL_PROVIDER_STATUS_ICON *pcpsiOptionalStatusIcon)
{
    PWCREDENTIAL *This = CredentialFrom(iface);
    UINT Text = 0;
    CREDENTIAL_PROVIDER_STATUS_ICON Icon = CPSI_NONE;

    if (!ppszOptionalStatusText || !pcpsiOptionalStatusIcon)
        return E_POINTER;
    *ppszOptionalStatusText = NULL;
    *pcpsiOptionalStatusIcon = CPSI_NONE;

    if (ntsStatus == STATUS_LOGON_FAILURE)
    {
        Text = (This->KnownUser && This->Scenario != CPUS_CHANGE_PASSWORD) ? IDS_BAD_PASSWORD : IDS_BAD_USER_OR_PASSWORD;
        Icon = CPSI_ERROR;
    }
    else if (ntsStatus == STATUS_PASSWORD_MUST_CHANGE && This->Scenario == CPUS_UNLOCK_WORKSTATION)
    {
        Text = IDS_PASSWORD_EXPIRED;
        Icon = CPSI_ERROR;
    }
    else if (NT_SUCCESS(ntsStatus) && This->Scenario == CPUS_CHANGE_PASSWORD)
    {
        Text = IDS_PASSWORD_CHANGED;
        Icon = CPSI_SUCCESS;
    }
    else if (This->Scenario == CPUS_CHANGE_PASSWORD)
    {
        /* SAM's verdict on a change: the old password, the new one against policy, or anything else. */
        if (ntsStatus == STATUS_WRONG_PASSWORD)
            Text = IDS_BAD_PASSWORD;
        else if (ntsStatus == STATUS_NO_SUCH_USER)
            Text = IDS_BAD_USER_OR_PASSWORD;
        else if (ntsStatus == STATUS_PASSWORD_RESTRICTION || ntsStatus == STATUS_PWD_TOO_SHORT ||
                 ntsStatus == STATUS_PWD_TOO_RECENT || ntsStatus == STATUS_PWD_HISTORY_CONFLICT)
            Text = IDS_PASSWORD_RESTRICTION;
        else
            Text = IDS_PASSWORD_CHANGE_FAILED;
        Icon = CPSI_ERROR;
    }

    ClearSecrets(This, !NT_SUCCESS(ntsStatus));
    if (!Text)
        return S_OK;
    *pcpsiOptionalStatusIcon = Icon;
    return LoadTextAlloc(Text, ppszOptionalStatusText);
}

static HRESULT WINAPI
Credential_GetUserSid(ICredentialProviderCredential2 *iface, LPWSTR *sid)
{
    PWCREDENTIAL *This = CredentialFrom(iface);
    HRESULT hr;

    if (!sid)
        return E_POINTER;
    *sid = NULL;
    if (!This->Sid)
        return S_FALSE;
    hr = PasswordProvider_DupString(This->Sid, sid);
    return hr;
}

static const ICredentialProviderCredential2Vtbl CredentialVtbl =
{
    Credential_QueryInterface,
    Credential_AddRef,
    Credential_Release,
    Credential_Advise,
    Credential_UnAdvise,
    Credential_SetSelected,
    Credential_SetDeselected,
    Credential_GetFieldState,
    Credential_GetStringValue,
    Credential_GetBitmapValue,
    Credential_GetCheckboxValue,
    Credential_GetSubmitButtonValue,
    Credential_GetComboBoxValueCount,
    Credential_GetComboBoxValueAt,
    Credential_SetStringValue,
    Credential_SetCheckboxValue,
    Credential_SetComboBoxSelectedValue,
    Credential_CommandLinkClicked,
    Credential_GetSerialization,
    Credential_ReportResult,
    Credential_GetUserSid,
};

static PWCREDENTIAL *
CredentialFromOptions(ICredentialProviderCredentialWithFieldOptions *iface)
{
    return CONTAINING_RECORD(iface, PWCREDENTIAL, ICredentialProviderCredentialWithFieldOptions_iface);
}

static HRESULT WINAPI
Options_QueryInterface(ICredentialProviderCredentialWithFieldOptions *iface, REFIID riid, void **ppv)
{
    return Credential_QueryInterface(&CredentialFromOptions(iface)->ICredentialProviderCredential2_iface, riid, ppv);
}

static ULONG WINAPI
Options_AddRef(ICredentialProviderCredentialWithFieldOptions *iface)
{
    return Credential_AddRef(&CredentialFromOptions(iface)->ICredentialProviderCredential2_iface);
}

static ULONG WINAPI
Options_Release(ICredentialProviderCredentialWithFieldOptions *iface)
{
    return Credential_Release(&CredentialFromOptions(iface)->ICredentialProviderCredential2_iface);
}

static HRESULT WINAPI
Options_GetFieldOptions(ICredentialProviderCredentialWithFieldOptions *iface, DWORD fieldID,
                        CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS *options)
{
    if (!options)
        return E_POINTER;
    /* Win11's PasswordProvider answers any ID here: CPCFO_NONE unless it is a secret field
     * (26100 baseline, Abi.c:135); only the per-field getters and setters reject unknown IDs. */
    *options = IsSecretField(fieldID) ? CPCFO_ENABLE_PASSWORD_REVEAL : CPCFO_NONE;
    return S_OK;
}

static const ICredentialProviderCredentialWithFieldOptionsVtbl OptionsVtbl =
{
    Options_QueryInterface,
    Options_AddRef,
    Options_Release,
    Options_GetFieldOptions,
};

HRESULT
PasswordCredential_Create(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
                          _In_opt_ ICredentialProviderUser *User,
                          _In_opt_ PCWSTR PrefillUserName,
                          _Outptr_ PWCREDENTIAL **Credential)
{
    PWCREDENTIAL *This;
    LPWSTR Name = NULL;

    *Credential = NULL;
    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;
    This->ICredentialProviderCredential2_iface.lpVtbl = &CredentialVtbl;
    This->ICredentialProviderCredentialWithFieldOptions_iface.lpVtbl = &OptionsVtbl;
    This->Ref = 1;
    This->Scenario = Scenario;
    InterlockedIncrement(&CredProvsObjects);

    LoadText(IDS_PROVIDER_LABEL, This->Text[PWFIELD_PROVIDER_LABEL], PWFIELD_TEXT_CHARS);
    if (User)
    {
        if (FAILED(ICredentialProviderUser_GetSid(User, &This->Sid)))
            This->Sid = NULL;
        if (SUCCEEDED(ICredentialProviderUser_GetStringValue(User, &PKEY_Identity_UserName, &Name)) && Name &&
            wcslen(Name) < PWFIELD_TEXT_CHARS)
        {
            wcscpy(This->Text[PWFIELD_USER_NAME], Name);
            wcscpy(This->Text[PWFIELD_FRIENDLY_NAME], Name);
            This->KnownUser = TRUE;
        }
        CoTaskMemFree(Name);
        Name = NULL;
        if (This->KnownUser &&
            SUCCEEDED(ICredentialProviderUser_GetStringValue(User, &PKEY_Identity_DisplayName, &Name)) && Name &&
            Name[0] && wcslen(Name) < PWFIELD_TEXT_CHARS)
        {
            wcscpy(This->Text[PWFIELD_FRIENDLY_NAME], Name);
        }
        CoTaskMemFree(Name);
    }
    if (!This->KnownUser)
    {
        CoTaskMemFree(This->Sid);
        This->Sid = NULL;
        LoadText(IDS_FRIENDLY_OTHER_USER, This->Text[PWFIELD_FRIENDLY_NAME], PWFIELD_TEXT_CHARS);
        if (PrefillUserName && wcslen(PrefillUserName) < PWFIELD_TEXT_CHARS)
        {
            wcscpy(This->Text[PWFIELD_USER_NAME], PrefillUserName);
            /* Changing a password for a given account: Windows 11 shows it read only in the first
             * box, with no "Other user" tile name and no user name edit beside it. */
            if (Scenario == CPUS_CHANGE_PASSWORD && PrefillUserName[0])
            {
                wcscpy(This->Text[PWFIELD_FRIENDLY_NAME], PrefillUserName);
                This->KnownUser = TRUE;
            }
        }
    }

    *Credential = This;
    return S_OK;
}
