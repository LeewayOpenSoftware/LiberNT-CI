/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the built-in password credential provider
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define COBJMACROS
#define CONST_VTABLE
#define WIN32_NO_STATUS

#include <apitest.h>
#include <ntstatus.h>
#include <objbase.h>
#include <sddl.h>
#include <ntsecapi.h>
#include <credentialprovider.h>
#include <shlguid.h>
#include <credser.h>
#include <lm.h>

typedef struct _TEST_USER
{
    ICredentialProviderUser ICredentialProviderUser_iface;
    LONG Ref;
    WCHAR Sid[128];
    WCHAR Name[64];
} TEST_USER;

typedef struct _TEST_USER_ARRAY
{
    ICredentialProviderUserArray ICredentialProviderUserArray_iface;
    LONG Ref;
    TEST_USER *User;
} TEST_USER_ARRAY;

static const GUID TestLocalUserProvider =
    { 0xa198529b, 0x730f, 0x4089, { 0xb6, 0x46, 0xa1, 0x25, 0x57, 0xf5, 0x66, 0x5e } };

static HRESULT
DupString(_In_ PCWSTR Source, _Out_ LPWSTR *Out)
{
    SIZE_T Bytes = (wcslen(Source) + 1) * sizeof(WCHAR);
    *Out = CoTaskMemAlloc(Bytes);
    if (!*Out)
        return E_OUTOFMEMORY;
    CopyMemory(*Out, Source, Bytes);
    return S_OK;
}

static TEST_USER *
UserFrom(ICredentialProviderUser *iface)
{
    return CONTAINING_RECORD(iface, TEST_USER, ICredentialProviderUser_iface);
}

static HRESULT WINAPI
User_QueryInterface(ICredentialProviderUser *iface, REFIID riid, void **ppv)
{
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
    TEST_USER *This = UserFrom(iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    if (!Ref)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static HRESULT WINAPI
User_GetSid(ICredentialProviderUser *iface, LPWSTR *sid)
{
    return DupString(UserFrom(iface)->Sid, sid);
}

static HRESULT WINAPI
User_GetProviderID(ICredentialProviderUser *iface, GUID *providerID)
{
    *providerID = TestLocalUserProvider;
    return S_OK;
}

static HRESULT WINAPI
User_GetStringValue(ICredentialProviderUser *iface, REFPROPERTYKEY key, LPWSTR *stringValue)
{
    return DupString(UserFrom(iface)->Name, stringValue);
}

static HRESULT WINAPI
User_GetValue(ICredentialProviderUser *iface, REFPROPERTYKEY key, PROPVARIANT *value)
{
    PropVariantInit(value);
    return E_NOTIMPL;
}

static const ICredentialProviderUserVtbl UserVtbl =
{
    User_QueryInterface, User_AddRef, User_Release,
    User_GetSid, User_GetProviderID, User_GetStringValue, User_GetValue,
};

static TEST_USER_ARRAY *
ArrayFrom(ICredentialProviderUserArray *iface)
{
    return CONTAINING_RECORD(iface, TEST_USER_ARRAY, ICredentialProviderUserArray_iface);
}

static HRESULT WINAPI
Array_QueryInterface(ICredentialProviderUserArray *iface, REFIID riid, void **ppv)
{
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
    TEST_USER_ARRAY *This = ArrayFrom(iface);
    ULONG Ref = InterlockedDecrement(&This->Ref);
    if (!Ref)
    {
        ICredentialProviderUser_Release(&This->User->ICredentialProviderUser_iface);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return Ref;
}

static HRESULT WINAPI
Array_SetProviderFilter(ICredentialProviderUserArray *iface, REFGUID guidProviderToFilterTo)
{
    return S_OK;
}

static HRESULT WINAPI
Array_GetAccountOptions(ICredentialProviderUserArray *iface, CREDENTIAL_PROVIDER_ACCOUNT_OPTIONS *options)
{
    *options = CPAO_NONE;
    return S_OK;
}

static HRESULT WINAPI
Array_GetCount(ICredentialProviderUserArray *iface, DWORD *userCount)
{
    *userCount = 1;
    return S_OK;
}

static HRESULT WINAPI
Array_GetAt(ICredentialProviderUserArray *iface, DWORD userIndex, ICredentialProviderUser **user)
{
    if (userIndex)
        return E_INVALIDARG;
    *user = &ArrayFrom(iface)->User->ICredentialProviderUser_iface;
    ICredentialProviderUser_AddRef(*user);
    return S_OK;
}

static const ICredentialProviderUserArrayVtbl ArrayVtbl =
{
    Array_QueryInterface, Array_AddRef, Array_Release,
    Array_SetProviderFilter, Array_GetAccountOptions, Array_GetCount, Array_GetAt,
};

static ICredentialProviderUserArray *
CreateUserArray(_In_ PCWSTR Name, _In_ PCWSTR Sid)
{
    TEST_USER_ARRAY *Array;
    TEST_USER *User;

    User = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*User));
    Array = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Array));
    if (!User || !Array)
        return NULL;
    User->ICredentialProviderUser_iface.lpVtbl = &UserVtbl;
    User->Ref = 1;
    Array->ICredentialProviderUserArray_iface.lpVtbl = &ArrayVtbl;
    Array->Ref = 1;
    Array->User = User;
    lstrcpynW(User->Name, Name, ARRAYSIZE(User->Name));
    lstrcpynW(User->Sid, Sid, ARRAYSIZE(User->Sid));
    return &Array->ICredentialProviderUserArray_iface;
}

static ICredentialProviderUserArray *
CreateCurrentUserArray(void)
{
    TEST_USER_ARRAY *Array;
    TEST_USER *User;
    BYTE TokenBuffer[256];
    TOKEN_USER *TokenUserInfo = (TOKEN_USER *)TokenBuffer;
    HANDLE Token;
    DWORD Size = ARRAYSIZE(((TEST_USER *)0)->Name);
    LPWSTR SidString;

    User = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*User));
    Array = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Array));
    if (!User || !Array)
        return NULL;
    User->ICredentialProviderUser_iface.lpVtbl = &UserVtbl;
    User->Ref = 1;
    Array->ICredentialProviderUserArray_iface.lpVtbl = &ArrayVtbl;
    Array->Ref = 1;
    Array->User = User;

    GetUserNameW(User->Name, &Size);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
    {
        if (GetTokenInformation(Token, TokenUser, TokenBuffer, sizeof(TokenBuffer), &Size) &&
            ConvertSidToStringSidW(TokenUserInfo->User.Sid, &SidString))
        {
            lstrcpynW(User->Sid, SidString, ARRAYSIZE(User->Sid));
            LocalFree(SidString);
        }
        CloseHandle(Token);
    }
    return &Array->ICredentialProviderUserArray_iface;
}


typedef HRESULT (WINAPI *PFN_GET_CLASS_OBJECT)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *PFN_GET_SECRET)(ICredentialProviderCredential *, DWORD, const WCHAR **, SIZE_T *);

static PFN_GET_CLASS_OBJECT pGetClassObject;

static HRESULT
CreateProvider(_Out_ ICredentialProvider **Provider)
{
    IClassFactory *Factory;
    HRESULT hr;

    *Provider = NULL;
    if (!pGetClassObject)
        return CoCreateInstance(&CLSID_PasswordCredentialProvider, NULL, CLSCTX_INPROC_SERVER,
                                &IID_ICredentialProvider, (void **)Provider);
    hr = pGetClassObject(&CLSID_PasswordCredentialProvider, &IID_IClassFactory, (void **)&Factory);
    if (FAILED(hr))
        return hr;
    hr = IClassFactory_CreateInstance(Factory, NULL, &IID_ICredentialProvider, (void **)Provider);
    IClassFactory_Release(Factory);
    return hr;
}

static ICredentialProviderCredential *
OpenTileForUsers(_In_ ICredentialProvider *Provider, _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
                 _In_opt_ ICredentialProviderUserArray *Users);

static ICredentialProviderCredential *
OpenTile(_In_ ICredentialProvider *Provider, _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario, _In_ BOOL WithUsers)
{
    ICredentialProviderUserArray *Users = WithUsers ? CreateCurrentUserArray() : NULL;
    ICredentialProviderCredential *Credential;

    Credential = OpenTileForUsers(Provider, Scenario, Users);
    if (Users)
        ICredentialProviderUserArray_Release(Users);
    return Credential;
}

static ICredentialProviderCredential *
OpenTileForUsers(_In_ ICredentialProvider *Provider, _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
                 _In_opt_ ICredentialProviderUserArray *Users)
{
    ICredentialProviderSetUserArray *SetUsers;
    ICredentialProviderCredential *Credential = NULL;
    DWORD Count = 0, Default = 0;
    BOOL AutoLogon = TRUE;
    HRESULT hr;

    if (Users)
    {
        hr = ICredentialProvider_QueryInterface(Provider, &IID_ICredentialProviderSetUserArray, (void **)&SetUsers);
        ok_hr(hr, S_OK);
        if (FAILED(hr))
            return NULL;
        ok_hr(ICredentialProviderSetUserArray_SetUserArray(SetUsers, Users), S_OK);
        ICredentialProviderSetUserArray_Release(SetUsers);
    }
    ok_hr(ICredentialProvider_SetUsageScenario(Provider, Scenario, 0), S_OK);
    ok_hr(ICredentialProvider_GetCredentialCount(Provider, &Count, &Default, &AutoLogon), S_OK);
    ok_long(Count, 1);
    ok_long(Default, Scenario == CPUS_CREDUI ? 0 : CREDENTIAL_PROVIDER_NO_DEFAULT);
    ok(!AutoLogon, "the password provider must not auto logon\n");
    if (Count)
        ok_hr(ICredentialProvider_GetCredentialAt(Provider, 0, &Credential), S_OK);
    return Credential;
}

static BOOL
FindField(_In_ ICredentialProvider *Provider, _In_ CREDENTIAL_PROVIDER_FIELD_TYPE Type, _In_ const GUID *FieldType,
          _Out_ DWORD *FieldId)
{
    CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *Desc;
    DWORD Count = 0, i;
    BOOL Found = FALSE;

    ICredentialProvider_GetFieldDescriptorCount(Provider, &Count);
    for (i = 0; i < Count && !Found; i++)
    {
        if (FAILED(ICredentialProvider_GetFieldDescriptorAt(Provider, i, &Desc)))
            continue;
        if (Desc->cpft == Type && (!FieldType || IsEqualGUID(&Desc->guidFieldType, FieldType)))
        {
            *FieldId = Desc->dwFieldID;
            Found = TRUE;
        }
        CoTaskMemFree(Desc->pszLabel);
        CoTaskMemFree(Desc);
    }
    return Found;
}

static VOID
TestTiles(void)
{
    ICredentialProvider *Provider;
    ICredentialProviderCredential *Credential;
    ICredentialProviderCredential2 *Credential2;
    ICredentialProviderCredentialWithFieldOptions *Options = NULL;
    CREDENTIAL_PROVIDER_CREDENTIAL_FIELD_OPTIONS FieldOptions;
    CREDENTIAL_PROVIDER_FIELD_STATE State;
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE Interactive;
    DWORD UserField = ~0u, PasswordField = ~0u, SubmitField = ~0u, LabelField = ~0u, Adjacent = ~0u;
    LPWSTR Sid;
    BOOL WithUsers;

    for (WithUsers = FALSE; WithUsers <= TRUE; WithUsers++)
    {
        if (FAILED(CreateProvider(&Provider)))
            return;
        ok_hr(ICredentialProvider_SetUsageScenario(Provider, CPUS_PLAP, 0), E_INVALIDARG);
        Credential = OpenTile(Provider, CPUS_LOGON, WithUsers);
        if (!Credential)
        {
            ICredentialProvider_Release(Provider);
            continue;
        }
        ok(FindField(Provider, CPFT_EDIT_TEXT, &CPFG_LOGON_USERNAME, &UserField), "no user name field\n");
        ok(FindField(Provider, CPFT_PASSWORD_TEXT, &CPFG_LOGON_PASSWORD, &PasswordField), "no password field\n");
        ok(FindField(Provider, CPFT_SUBMIT_BUTTON, NULL, &SubmitField), "no submit button\n");
        ok_long(UserField, 1);
        ok_long(PasswordField, 2);
        ok_long(SubmitField, 12);
        ok(FindField(Provider, CPFT_SMALL_TEXT, &CPFG_CREDENTIAL_PROVIDER_LABEL, &LabelField), "no provider label\n");
        ok_long(LabelField, 15);

        /* F1: the FieldTable gaps (5-11, 13, 14) are not fields. */
        ok_hr(ICredentialProviderCredential_GetFieldState(Credential, 5, &State, &Interactive), E_INVALIDARG);
        ok_hr(ICredentialProviderCredential_GetFieldState(Credential, 13, &State, &Interactive), E_INVALIDARG);
        {
            LPWSTR GapString = NULL;
            ok_hr(ICredentialProviderCredential_GetStringValue(Credential, 5, &GapString), E_INVALIDARG);
            ok(GapString == NULL, "gap ID 5 returned a string\n");
            ok_hr(ICredentialProviderCredential_GetStringValue(Credential, 13, &GapString), E_INVALIDARG);
            ok(GapString == NULL, "gap ID 13 returned a string\n");
        }

        ok_hr(ICredentialProviderCredential_GetSubmitButtonValue(Credential, SubmitField, &Adjacent), S_OK);
        ok_long(Adjacent, PasswordField);
        ok_hr(ICredentialProviderCredential_GetFieldState(Credential, UserField, &State, &Interactive), S_OK);
        ok_long(State, WithUsers ? CPFS_HIDDEN : CPFS_DISPLAY_IN_SELECTED_TILE);
        ok_long(Interactive, WithUsers ? CPFIS_NONE : CPFIS_FOCUSED);
        ok_hr(ICredentialProviderCredential_GetFieldState(Credential, PasswordField, &State, &Interactive), S_OK);
        /* The account running the test may have a blank password (TestBlankPassword covers that). */
        if (WithUsers && State == CPFS_HIDDEN)
        {
            ok_long(Interactive, CPFIS_NONE);
        }
        else
        {
            ok_long(State, CPFS_DISPLAY_IN_SELECTED_TILE);
            ok_long(Interactive, WithUsers ? CPFIS_FOCUSED : CPFIS_NONE);
        }

        ok_hr(ICredentialProviderCredential_QueryInterface(Credential, &IID_ICredentialProviderCredential2,
                                                           (void **)&Credential2), S_OK);
        if (Credential2)
        {
            Sid = (LPWSTR)(ULONG_PTR)0xdeadbeef;
            ok_hr(ICredentialProviderCredential2_GetUserSid(Credential2, &Sid), WithUsers ? S_OK : S_FALSE);
            ok(WithUsers ? (Sid && !wcsncmp(Sid, L"S-1-5-", 6)) : !Sid, "unexpected SID %p\n", Sid);
            if (Sid != (LPWSTR)(ULONG_PTR)0xdeadbeef)
                CoTaskMemFree(Sid);
            ICredentialProviderCredential2_Release(Credential2);
        }
        ok_hr(ICredentialProviderCredential_QueryInterface(Credential, &IID_ICredentialProviderCredentialWithFieldOptions,
                                                           (void **)&Options), S_OK);
        if (Options)
        {
            FieldOptions = 0xdead;
            ok_hr(ICredentialProviderCredentialWithFieldOptions_GetFieldOptions(Options, PasswordField, &FieldOptions), S_OK);
            ok_long(FieldOptions, CPCFO_ENABLE_PASSWORD_REVEAL);
            FieldOptions = 0xdead;
            ok_hr(ICredentialProviderCredentialWithFieldOptions_GetFieldOptions(Options, UserField, &FieldOptions), S_OK);
            ok_long(FieldOptions, CPCFO_NONE);
            ICredentialProviderCredentialWithFieldOptions_Release(Options);
            Options = NULL;
        }
        ICredentialProviderCredential_Release(Credential);
        ICredentialProvider_Release(Provider);
    }
}

static ULONG
PreferredPackage(void)
{
    CREDSER_PACKAGES Packages;
    HANDLE Lsa;

    if (!NT_SUCCESS(LsaConnectUntrusted(&Lsa)))
        return ~0u;
    if (!NT_SUCCESS(CredSerLookupPackages(Lsa, &Packages)))
        Packages.HaveNegotiate = Packages.HaveMsv1_0 = FALSE;
    LsaDeregisterLogonProcess(Lsa);
    return (Packages.HaveNegotiate || Packages.HaveMsv1_0) ? CredSerPreferredPackage(&Packages) : ~0u;
}

static BOOL
Equals(_In_ const UNICODE_STRING *String, _In_ PCWSTR Expected)
{
    return String->Length == wcslen(Expected) * sizeof(WCHAR) &&
           !memcmp(String->Buffer, Expected, String->Length);
}

static VOID
Serialize(_In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario, _In_ BOOL WithUsers, _In_opt_ PCWSTR User,
          _In_opt_ PCWSTR ExpectedDomain, _In_opt_ PCWSTR ExpectedUser, _In_opt_ PCWSTR NewPassword,
          _In_opt_ PCWSTR Confirm)
{
    ICredentialProvider *Provider;
    ICredentialProviderCredential *Credential;
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE Response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION Cs;
    CREDENTIAL_PROVIDER_STATUS_ICON Icon;
    CREDSER_LOGON Logon;
    CREDSER_CHANGE_PASSWORD Change;
    LPWSTR Text = NULL;
    BOOL Mismatch = NewPassword && Confirm && wcscmp(NewPassword, Confirm);

    if (FAILED(CreateProvider(&Provider)))
        return;
    Credential = OpenTile(Provider, Scenario, WithUsers);
    if (!Credential)
    {
        ICredentialProvider_Release(Provider);
        return;
    }
    if (User)
        ok_hr(ICredentialProviderCredential_SetStringValue(Credential, 1, User), S_OK);
    ok_hr(ICredentialProviderCredential_SetStringValue(Credential, 2, L"secret"), S_OK);
    if (NewPassword)
        ICredentialProviderCredential_SetStringValue(Credential, 3, NewPassword);
    if (Confirm)
        ICredentialProviderCredential_SetStringValue(Credential, 4, Confirm);

    ZeroMemory(&Cs, sizeof(Cs));
    ok_hr(ICredentialProviderCredential_GetSerialization(Credential, &Response, &Cs, &Text, &Icon), S_OK);
    if (Mismatch)
    {
        ok_long(Response, CPGSR_NO_CREDENTIAL_NOT_FINISHED);
        ok_long(Icon, CPSI_ERROR);
        ok(Text != NULL, "a password mismatch needs a status text\n");
        ok(!Cs.rgbSerialization && !Cs.cbSerialization, "a mismatch must not serialize\n");
    }
    else
    {
        ok_long(Response, CPGSR_RETURN_CREDENTIAL_FINISHED);
        ok_long(Icon, CPSI_NONE);
        ok(!Text, "unexpected status text '%ls'\n", Text);
        ok(IsEqualGUID(&Cs.clsidCredentialProvider, &CLSID_PasswordCredentialProvider), "wrong provider CLSID\n");
        ok_long(Cs.ulAuthenticationPackage, PreferredPackage());
        if (Scenario == CPUS_CHANGE_PASSWORD)
        {
            ok_ntstatus(CredSerUnpackChangePassword(Cs.rgbSerialization, Cs.cbSerialization, &Change), STATUS_SUCCESS);
            ok(Equals(&Change.AccountName, ExpectedUser), "account mismatch\n");
            ok(Change.OldPassword.Length && Change.NewPassword.Length, "empty passwords\n");
        }
        else
        {
            ok_ntstatus(CredSerUnpackLogon(Cs.rgbSerialization, Cs.cbSerialization, &Logon), STATUS_SUCCESS);
            ok_long(Logon.MessageType,
                    Scenario == CPUS_UNLOCK_WORKSTATION ? KerbWorkstationUnlockLogon : KerbInteractiveLogon);
            ok(Equals(&Logon.UserName, ExpectedUser), "user mismatch for %ls\n", ExpectedUser);
            ok(Equals(&Logon.LogonDomainName, ExpectedDomain), "domain mismatch, expected '%ls'\n", ExpectedDomain);
            ok(Logon.Password.Length != 0, "empty password\n");
        }
        if (Cs.rgbSerialization)
        {
            SecureZeroMemory(Cs.rgbSerialization, Cs.cbSerialization);
            CoTaskMemFree(Cs.rgbSerialization);
        }
    }
    CoTaskMemFree(Text);

    Text = NULL;
    ok_hr(ICredentialProviderCredential_ReportResult(Credential, STATUS_LOGON_FAILURE, 0, &Text, &Icon), S_OK);
    ok_long(Icon, CPSI_ERROR);
    ok(Text != NULL, "a logon failure needs a status text\n");
    CoTaskMemFree(Text);
    Text = NULL;
    ok_hr(ICredentialProviderCredential_ReportResult(Credential, STATUS_ACCOUNT_DISABLED, 0, &Text, &Icon), S_OK);
    ok_long(Icon, CPSI_NONE);
    ok(!Text, "unexpected text for a disabled account\n");
    CoTaskMemFree(Text);
    Text = NULL;
    ok_hr(ICredentialProviderCredential_ReportResult(Credential, STATUS_SUCCESS, 0, &Text, &Icon), S_OK);
    ok_long(Icon, Scenario == CPUS_CHANGE_PASSWORD ? CPSI_SUCCESS : CPSI_NONE);
    CoTaskMemFree(Text);

    ICredentialProviderCredential_Release(Credential);
    ICredentialProvider_Release(Provider);
}

static VOID
TestSerialization(void)
{
    WCHAR Computer[MAX_COMPUTERNAME_LENGTH + 1], User[64];
    DWORD Chars = ARRAYSIZE(Computer), UserChars = ARRAYSIZE(User);

    GetComputerNameW(Computer, &Chars);
    GetUserNameW(User, &UserChars);
    Serialize(CPUS_LOGON, FALSE, L"someone", Computer, L"someone", NULL, NULL);
    Serialize(CPUS_LOGON, FALSE, L"DOM\\someone", L"DOM", L"someone", NULL, NULL);
    Serialize(CPUS_LOGON, FALSE, L"someone@dom.example", L"", L"someone@dom.example", NULL, NULL);
    Serialize(CPUS_LOGON, TRUE, NULL, Computer, User, NULL, NULL);
    Serialize(CPUS_UNLOCK_WORKSTATION, TRUE, NULL, Computer, User, NULL, NULL);
    Serialize(CPUS_CHANGE_PASSWORD, TRUE, NULL, Computer, User, L"new1", L"new1");
    Serialize(CPUS_CHANGE_PASSWORD, TRUE, NULL, Computer, User, L"new1", L"new2");
    Serialize(CPUS_CREDUI, FALSE, L"someone", Computer, L"someone", NULL, NULL);
}

/* A local account with a blank password gets only the Sign in button on the sign-in and unlock
 * pages; one with a password keeps the box. Uses real accounts, created and removed here.
 * VM only: never run this test on a real user machine. */
static BOOL
AddTestAccount(_In_ PCWSTR Name, _In_opt_ PCWSTR Password, _Out_writes_(SidChars) PWSTR Sid, _In_ DWORD SidChars)
{
    USER_INFO_1 Info;
    BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
    WCHAR Domain[256];
    DWORD SidSize = sizeof(SidBuffer), DomainChars = ARRAYSIZE(Domain);
    SID_NAME_USE Use;
    LPWSTR SidString;
    NET_API_STATUS Status;

    NetUserDel(NULL, Name);
    ZeroMemory(&Info, sizeof(Info));
    Info.usri1_name = (LPWSTR)Name;
    Info.usri1_password = (LPWSTR)(Password ? Password : L"");
    Info.usri1_priv = USER_PRIV_USER;
    Info.usri1_flags = UF_SCRIPT | (Password ? 0 : UF_PASSWD_NOTREQD);
    Status = NetUserAdd(NULL, 1, (LPBYTE)&Info, NULL);
    if (Status != NERR_Success)
    {
        skip("NetUserAdd(%ls) failed with %lu\n", Name, Status);
        return FALSE;
    }
    if (!LookupAccountNameW(NULL, Name, SidBuffer, &SidSize, Domain, &DomainChars, &Use) ||
        !ConvertSidToStringSidW(SidBuffer, &SidString))
    {
        ok(0, "cannot look up %ls: %lu\n", Name, GetLastError());
        NetUserDel(NULL, Name);
        return FALSE;
    }
    lstrcpynW(Sid, SidString, SidChars);
    LocalFree(SidString);
    return TRUE;
}

static VOID
CheckPasswordBox(_In_ PCWSTR Name, _In_ PCWSTR Sid, _In_ CREDENTIAL_PROVIDER_USAGE_SCENARIO Scenario,
                 _In_ BOOL ExpectBox)
{
    ICredentialProvider *Provider;
    ICredentialProviderUserArray *Users;
    ICredentialProviderCredential *Credential;
    CREDENTIAL_PROVIDER_FIELD_STATE State;
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE Interactive;

    if (FAILED(CreateProvider(&Provider)))
        return;
    Users = CreateUserArray(Name, Sid);
    Credential = Users ? OpenTileForUsers(Provider, Scenario, Users) : NULL;
    if (Credential)
    {
        ok_hr(ICredentialProviderCredential_GetFieldState(Credential, 2, &State, &Interactive), S_OK);
        ok(State == (ExpectBox ? CPFS_DISPLAY_IN_SELECTED_TILE : CPFS_HIDDEN),
           "%ls, scenario %d: password state %d\n", Name, Scenario, State);
        ok(Interactive == (ExpectBox ? CPFIS_FOCUSED : CPFIS_NONE),
           "%ls, scenario %d: password interactive state %d\n", Name, Scenario, Interactive);
        ok_hr(ICredentialProviderCredential_GetFieldState(Credential, 12, &State, &Interactive), S_OK);
        ok_long(State, CPFS_DISPLAY_IN_SELECTED_TILE);
        ok(Interactive == (ExpectBox ? CPFIS_NONE : CPFIS_FOCUSED),
           "%ls, scenario %d: submit interactive state %d\n", Name, Scenario, Interactive);
        ICredentialProviderCredential_Release(Credential);
    }
    if (Users)
        ICredentialProviderUserArray_Release(Users);
    ICredentialProvider_Release(Provider);
}

static VOID
TestBlankPassword(void)
{
    static const WCHAR BlankName[] = L"cptest_blank", PasswordName[] = L"cptest_pw";
    WCHAR BlankSid[128], PasswordSid[128];

    if (!AddTestAccount(BlankName, NULL, BlankSid, ARRAYSIZE(BlankSid)))
        return;
    if (AddTestAccount(PasswordName, L"Cp7!tile-Box", PasswordSid, ARRAYSIZE(PasswordSid)))
    {
        CheckPasswordBox(BlankName, BlankSid, CPUS_LOGON, FALSE);
        CheckPasswordBox(BlankName, BlankSid, CPUS_UNLOCK_WORKSTATION, FALSE);
        CheckPasswordBox(BlankName, BlankSid, CPUS_CREDUI, TRUE);
        CheckPasswordBox(PasswordName, PasswordSid, CPUS_LOGON, TRUE);
        CheckPasswordBox(PasswordName, PasswordSid, CPUS_UNLOCK_WORKSTATION, TRUE);
        /* A SID that is no account of this computer keeps the box. */
        CheckPasswordBox(BlankName, L"S-1-5-21-1-2-3-1001", CPUS_LOGON, TRUE);
        NetUserDel(NULL, PasswordName);
    }
    NetUserDel(NULL, BlankName);
}

static BOOL
AllZero(_In_ const WCHAR *Buffer, _In_ SIZE_T Bytes)
{
    const BYTE *p = (const BYTE *)Buffer;
    SIZE_T i;

    for (i = 0; i < Bytes; i++)
    {
        if (p[i])
            return FALSE;
    }
    return TRUE;
}

static VOID
TestSecretZeroing(void)
{
    HMODULE Hooks = LoadLibraryW(L"credprovs_test.dll");
    PFN_GET_SECRET pGetSecret;
    ICredentialProvider *Provider;
    ICredentialProviderCredential *Credential;
    CREDENTIAL_PROVIDER_STATUS_ICON Icon;
    const WCHAR *Buffer;
    SIZE_T Bytes;
    LPWSTR Text;
    PFN_GET_CLASS_OBJECT Saved = pGetClassObject;

    if (!Hooks)
    {
        skip("credprovs_test.dll is not available\n");
        return;
    }
    pGetSecret = (PFN_GET_SECRET)GetProcAddress(Hooks, "CredProvsTestGetSecretBuffer");
    pGetClassObject = (PFN_GET_CLASS_OBJECT)GetProcAddress(Hooks, "DllGetClassObject");
    if (pGetSecret && pGetClassObject && SUCCEEDED(CreateProvider(&Provider)))
    {
        Credential = OpenTile(Provider, CPUS_CHANGE_PASSWORD, FALSE);
        if (Credential)
        {
            ICredentialProviderCredential_SetStringValue(Credential, 2, L"secret");
            ICredentialProviderCredential_SetStringValue(Credential, 3, L"secret2");
            ok_hr(pGetSecret(Credential, 2, &Buffer, &Bytes), S_OK);
            ok(!AllZero(Buffer, Bytes), "the password was not stored\n");
            ICredentialProviderCredential_SetDeselected(Credential);
            ok(AllZero(Buffer, Bytes), "SetDeselected left the password in memory\n");
            ok_hr(pGetSecret(Credential, 3, &Buffer, &Bytes), S_OK);
            ok(AllZero(Buffer, Bytes), "SetDeselected left the new password in memory\n");

            ICredentialProviderCredential_SetStringValue(Credential, 2, L"secret");
            Text = NULL;
            ICredentialProviderCredential_ReportResult(Credential, STATUS_LOGON_FAILURE, 0, &Text, &Icon);
            CoTaskMemFree(Text);
            ok_hr(pGetSecret(Credential, 2, &Buffer, &Bytes), S_OK);
            ok(AllZero(Buffer, Bytes), "ReportResult left the password in memory\n");

            ICredentialProviderCredential_SetStringValue(Credential, 2, L"secret");
            ICredentialProviderCredential_UnAdvise(Credential);
            ok(AllZero(Buffer, Bytes), "UnAdvise left the password in memory\n");
            ICredentialProviderCredential_Release(Credential);
        }
        ICredentialProvider_Release(Provider);
    }
    pGetClassObject = Saved;
    FreeLibrary(Hooks);
}

START_TEST(PasswordProvider)
{
    WCHAR Path[MAX_PATH];
    HMODULE Module = NULL;
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    ok_hr(hr, S_OK);
    if (GetEnvironmentVariableW(L"CREDPROV_TEST_CREDPROVS", Path, ARRAYSIZE(Path)))
    {
        Module = LoadLibraryW(Path);
        pGetClassObject = Module ? (PFN_GET_CLASS_OBJECT)GetProcAddress(Module, "DllGetClassObject") : NULL;
        ok(pGetClassObject != NULL, "cannot load %ls\n", Path);
        trace("testing %ls\n", Path);
    }
    TestTiles();
    TestSerialization();
    TestBlankPassword();
    TestSecretZeroing();
    if (Module)
        FreeLibrary(Module);
    if (SUCCEEDED(hr))
        CoUninitialize();
}
