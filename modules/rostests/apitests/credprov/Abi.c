/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Checks the credential provider ABI: IIDs, CLSIDs, enums, layouts and vtables
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS

#include <apitest.h>
#include <objbase.h>
#include <ntsecapi.h>
#include <credentialprovider.h>
#include <shlguid.h>

C_ASSERT(CPUS_INVALID == 0 && CPUS_LOGON == 1 && CPUS_UNLOCK_WORKSTATION == 2 &&
         CPUS_CHANGE_PASSWORD == 3 && CPUS_CREDUI == 4 && CPUS_PLAP == 5);
C_ASSERT(CPFT_INVALID == 0 && CPFT_PASSWORD_TEXT == 5 && CPFT_TILE_IMAGE == 6 && CPFT_SUBMIT_BUTTON == 9);
C_ASSERT(CPFS_HIDDEN == 0 && CPFS_DISPLAY_IN_BOTH == 3);
C_ASSERT(CPFIS_NONE == 0 && CPFIS_FOCUSED == 3);
C_ASSERT(CPGSR_NO_CREDENTIAL_NOT_FINISHED == 0 && CPGSR_RETURN_CREDENTIAL_FINISHED == 2 &&
         CPGSR_RETURN_NO_CREDENTIAL_FINISHED == 3);
C_ASSERT(CPSI_NONE == 0 && CPSI_SUCCESS == 3);
C_ASSERT(CPAO_EMPTY_CONNECTED == 2 && CPCFO_SHOW_ENGLISH_KEYBOARD == 0x10);
C_ASSERT(KerbInteractiveLogon == 2 && KerbWorkstationUnlockLogon == 7 && KerbChangePasswordMessage == 7);

C_ASSERT(FIELD_OFFSET(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR, pszLabel) == 8);
C_ASSERT(FIELD_OFFSET(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR, guidFieldType) == 8 + sizeof(void *));
C_ASSERT(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR) == 24 + sizeof(void *));
C_ASSERT(FIELD_OFFSET(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION, clsidCredentialProvider) == 4);
C_ASSERT(FIELD_OFFSET(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION, cbSerialization) == 20);
C_ASSERT(FIELD_OFFSET(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION, rgbSerialization) == 24);
C_ASSERT(sizeof(CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION) == (sizeof(void *) == 8 ? 32 : 28));
C_ASSERT(sizeof(KERB_INTERACTIVE_LOGON) == (sizeof(void *) == 8 ? 56 : 28));
C_ASSERT(sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) == (sizeof(void *) == 8 ? 64 : 36));
C_ASSERT(FIELD_OFFSET(KERB_INTERACTIVE_UNLOCK_LOGON, LogonId) == sizeof(KERB_INTERACTIVE_LOGON));
C_ASSERT(FIELD_OFFSET(KERB_CHANGEPASSWORD_REQUEST, NewPassword) == FIELD_OFFSET(KERB_CHANGEPASSWORD_REQUEST, DomainName) + 3 * sizeof(UNICODE_STRING));

static const struct
{
    const GUID *Guid;
    const char *Name;
    GUID Expected;
} Guids[] =
{
    { &IID_ICredentialProviderCredential, "ICredentialProviderCredential",
      { 0x63913a93, 0x40c1, 0x481a, { 0x81, 0x8d, 0x40, 0x72, 0xff, 0x8c, 0x70, 0xcc } } },
    { &IID_IQueryContinueWithStatus, "IQueryContinueWithStatus",
      { 0x9090be5b, 0x502b, 0x41fb, { 0xbc, 0xcc, 0x00, 0x49, 0xa6, 0xc7, 0x25, 0x4b } } },
    { &IID_IConnectableCredentialProviderCredential, "IConnectableCredentialProviderCredential",
      { 0x9387928b, 0xac75, 0x4bf9, { 0x8a, 0xb2, 0x2b, 0x93, 0xc4, 0xa5, 0x52, 0x90 } } },
    { &IID_ICredentialProviderCredentialEvents, "ICredentialProviderCredentialEvents",
      { 0xfa6fa76b, 0x66b7, 0x4b11, { 0x95, 0xf1, 0x86, 0x17, 0x11, 0x18, 0xe8, 0x16 } } },
    { &IID_ICredentialProvider, "ICredentialProvider",
      { 0xd27c3481, 0x5a1c, 0x45b2, { 0x8a, 0xaa, 0xc2, 0x0e, 0xbb, 0xe8, 0x22, 0x9e } } },
    { &IID_ICredentialProviderEvents, "ICredentialProviderEvents",
      { 0x34201e5a, 0xa787, 0x41a3, { 0xa5, 0xa4, 0xbd, 0x6d, 0xcf, 0x2a, 0x85, 0x4e } } },
    { &IID_ICredentialProviderFilter, "ICredentialProviderFilter",
      { 0xa5da53f9, 0xd475, 0x4080, { 0xa1, 0x20, 0x91, 0x0c, 0x4a, 0x73, 0x98, 0x80 } } },
    { &IID_ICredentialProviderCredential2, "ICredentialProviderCredential2",
      { 0xfd672c54, 0x40ea, 0x4d6e, { 0x9b, 0x49, 0xcf, 0xb1, 0xa7, 0x50, 0x7b, 0xd7 } } },
    { &IID_ICredentialProviderCredentialWithFieldOptions, "ICredentialProviderCredentialWithFieldOptions",
      { 0xdbc6fb30, 0xc843, 0x49e3, { 0xa6, 0x45, 0x57, 0x3e, 0x6f, 0x39, 0x44, 0x6a } } },
    { &IID_ICredentialProviderCredentialEvents2, "ICredentialProviderCredentialEvents2",
      { 0xb53c00b6, 0x9922, 0x4b78, { 0xb1, 0xf4, 0xdd, 0xfe, 0x77, 0x4d, 0xc3, 0x9b } } },
    { &IID_ICredentialProviderUser, "ICredentialProviderUser",
      { 0x13793285, 0x3ea6, 0x40fd, { 0xb4, 0x20, 0x15, 0xf4, 0x7d, 0xa4, 0x1f, 0xbb } } },
    { &IID_ICredentialProviderUserArray, "ICredentialProviderUserArray",
      { 0x90c119ae, 0x0f18, 0x4520, { 0xa1, 0xf1, 0x11, 0x43, 0x66, 0xa4, 0x0f, 0xe8 } } },
    { &IID_ICredentialProviderSetUserArray, "ICredentialProviderSetUserArray",
      { 0x095c1484, 0x1c0c, 0x4388, { 0x9c, 0x6d, 0x50, 0x0e, 0x61, 0xbf, 0x84, 0xbd } } },
    { &CLSID_PasswordCredentialProvider, "CLSID_PasswordCredentialProvider",
      { 0x60b78e88, 0xead8, 0x445c, { 0x9c, 0xfd, 0x0b, 0x87, 0xf7, 0x4e, 0xa6, 0xcd } } },
    { &CPFG_LOGON_USERNAME, "CPFG_LOGON_USERNAME",
      { 0xda15bbe8, 0x954d, 0x4fd3, { 0xb0, 0xf4, 0x1f, 0xb5, 0xb9, 0x0b, 0x17, 0x4b } } },
    { &CPFG_LOGON_PASSWORD, "CPFG_LOGON_PASSWORD",
      { 0x60624cfa, 0xa477, 0x47b1, { 0x8a, 0x8e, 0x3a, 0x4a, 0x19, 0x98, 0x18, 0x27 } } },
    { &CPFG_CREDENTIAL_PROVIDER_LOGO, "CPFG_CREDENTIAL_PROVIDER_LOGO",
      { 0x2d837775, 0xf6cd, 0x464e, { 0xa7, 0x45, 0x48, 0x2f, 0xd0, 0xb4, 0x74, 0x93 } } },
    { &CPFG_CREDENTIAL_PROVIDER_LABEL, "CPFG_CREDENTIAL_PROVIDER_LABEL",
      { 0x286bbff3, 0xbad4, 0x438f, { 0xb0, 0x07, 0x79, 0xb7, 0x26, 0x7c, 0x3d, 0x48 } } },
};

static void
TestGuids(void)
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(Guids); i++)
        ok(IsEqualGUID(Guids[i].Guid, &Guids[i].Expected), "%s has the wrong value\n", Guids[i].Name);
}

static void
TestPasswordProviderVtable(void)
{
    ICredentialProvider *Provider = NULL;
    ICredentialProviderSetUserArray *SetUserArray = NULL;
    IUnknown *Unknown = NULL;
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *Field;
    DWORD Count = 0, Default = 0, i;
    BOOL AutoLogon = TRUE, SawPassword = FALSE;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_PasswordCredentialProvider, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ICredentialProvider, (void **)&Provider);
    if (hr == REGDB_E_CLASSNOTREG)
    {
        skip("PasswordProvider is not registered\n");
        return;
    }
    ok_hr(hr, S_OK);
    if (FAILED(hr))
        return;

    ok_hr(ICredentialProvider_QueryInterface(Provider, &IID_IUnknown, (void **)&Unknown), S_OK);
    if (Unknown)
        IUnknown_Release(Unknown);
    hr = ICredentialProvider_QueryInterface(Provider, &IID_ICredentialProviderSetUserArray, (void **)&SetUserArray);
    ok_hr(hr, S_OK);
    if (SetUserArray)
        ICredentialProviderSetUserArray_Release(SetUserArray);

    ok_hr(ICredentialProvider_SetUsageScenario(Provider, CPUS_PLAP, 0), E_INVALIDARG);
    ok_hr(ICredentialProvider_SetUsageScenario(Provider, CPUS_CREDUI, 0), S_OK);

    ok_hr(ICredentialProvider_GetFieldDescriptorCount(Provider, &Count), S_OK);
    ok(Count > 0 && Count < 64, "unexpected field count %lu\n", Count);
    for (i = 0; i < Count && i < 64; i++)
    {
        Field = NULL;
        hr = ICredentialProvider_GetFieldDescriptorAt(Provider, i, &Field);
        ok_hr(hr, S_OK);
        if (FAILED(hr) || !Field)
            continue;
        ok(Field->dwFieldID < 64, "field %lu has id %lu\n", i, Field->dwFieldID);
        ok(Field->cpft > CPFT_INVALID && Field->cpft <= CPFT_SUBMIT_BUTTON, "field %lu has type %d\n", i, Field->cpft);
        if (Field->cpft == CPFT_PASSWORD_TEXT && IsEqualGUID(&Field->guidFieldType, &CPFG_LOGON_PASSWORD))
            SawPassword = TRUE;
        CoTaskMemFree(Field->pszLabel);
        CoTaskMemFree(Field);
    }
    ok(SawPassword, "no CPFG_LOGON_PASSWORD password field\n");
    ok_hr(ICredentialProvider_GetFieldDescriptorAt(Provider, Count, &Field), E_INVALIDARG);

    hr = ICredentialProvider_GetCredentialCount(Provider, &Count, &Default, &AutoLogon);
    ok_hr(hr, S_OK);
    ok(!AutoLogon, "CREDUI must not auto logon\n");

    ICredentialProvider_Release(Provider);
}

START_TEST(Abi)
{
    HRESULT hr;

    TestGuids();
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok_hr(hr, S_OK);
    TestPasswordProviderVtable();
    if (SUCCEEDED(hr))
        CoUninitialize();
}
