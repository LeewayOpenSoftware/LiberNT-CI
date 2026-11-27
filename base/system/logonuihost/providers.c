/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Credential providers: enumeration, the selected credential,
 *              bounded calls with a per-call timeout, serialization hand-off
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include <credprovhost.h>
#include <ntsecapi.h>
#include <credser.h>


#define LUH_MAX_FAULTS 16

/*
 * Every provider call runs on one STA thread owned by the host. The UI thread
 * hands it a request and waits at most LUH_PROVIDER_CALL_MS; on a timeout the
 * thread is considered wedged by that provider: the fault is recorded and
 * reported, the thread abandoned, and a new one re-enumerates without it.
 */
typedef enum _LUH_REQUEST_TYPE
{
    LuhReqNone = 0,
    LuhReqSetString,
    LuhReqGetSerialization,
    LuhReqReportResult,
    LuhReqDeselect,
    LuhReqSelectTile
} LUH_REQUEST_TYPE;

typedef struct _LUH_REQUEST
{
    LUH_REQUEST_TYPE Type;
    DWORD Field;
    WCHAR Text[LUH_FIELD_TEXT];
    NTSTATUS Status;
    NTSTATUS SubStatus;
    /* out */
    HRESULT Hr;
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE Response;
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION Serialization;
    WCHAR StatusText[LUH_FIELD_TEXT];
} LUH_REQUEST;

/* One credential of one provider: an entry in the tile list. */
typedef struct _LUH_TILE
{
    ICredentialProvider *Provider;
    ICredentialProviderCredential *Credential;
    CLSID Clsid;
    WCHAR Name[LUH_TILE_TEXT];
} LUH_TILE;

typedef struct _LUH_EVENTS LUH_EVENTS;

typedef struct _LUH_PROVIDER_THREAD
{
    HANDLE Thread;
    HANDLE RequestEvent;
    HANDLE DoneEvent;
    HANDLE StopEvent;
    LUH_REQUEST Request;
    CLSID CredentialClsid;          /* provider that owns the selected credential */
    BOOL Abandoned;
    /* owned by the thread */
    PCPH_PROVIDER_SET Set;
    ICredentialProvider *Provider;
    ICredentialProviderCredential *Credential;  /* the selected tile's, advised */
    LUH_EVENTS *Events;
    LUH_TILE Tiles[LUH_MAX_TILES];
    DWORD TileCount;
    DWORD Selected;
    BOOL AutoSubmit;                /* the first page only: submit the default without waiting for input */
} LUH_PROVIDER_THREAD, *PLUH_PROVIDER_THREAD;

static PLUH_PROVIDER_THREAD g_Current;
static HWND g_Window;
static CLSID g_Faulted[LUH_MAX_FAULTS];
static ULONG g_FaultCount;

/* ---- events sink: the provider may change a field while the page is up ---- */

struct _LUH_EVENTS
{
    ICredentialProviderCredentialEvents ICredentialProviderCredentialEvents_iface;
    LONG RefCount;
    PLUH_PROVIDER_THREAD Owner;     /* updates from an abandoned thread's credential are dropped */
};

static LUH_EVENTS *
EventsFrom(ICredentialProviderCredentialEvents *iface)
{
    return CONTAINING_RECORD(iface, LUH_EVENTS, ICredentialProviderCredentialEvents_iface);
}

static HRESULT WINAPI Events_QueryInterface(ICredentialProviderCredentialEvents *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_ICredentialProviderCredentialEvents))
    {
        *ppv = iface;
        ICredentialProviderCredentialEvents_AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI Events_AddRef(ICredentialProviderCredentialEvents *iface)
{
    LUH_EVENTS *This = CONTAINING_RECORD(iface, LUH_EVENTS, ICredentialProviderCredentialEvents_iface);
    return InterlockedIncrement(&This->RefCount);
}

static ULONG WINAPI Events_Release(ICredentialProviderCredentialEvents *iface)
{
    LUH_EVENTS *This = CONTAINING_RECORD(iface, LUH_EVENTS, ICredentialProviderCredentialEvents_iface);
    ULONG Ref = InterlockedDecrement(&This->RefCount);
    if (Ref == 0)
        HeapFree(GetProcessHeap(), 0, This);
    return Ref;
}

static VOID
PostFieldUpdate(
    _In_ LUH_EVENTS *Events,
    _In_ DWORD Field,
    _In_ LUH_FIELD_UPDATE_KIND Kind,
    _In_ DWORD Value,
    _In_opt_ PCWSTR Text)
{
    PLUH_FIELD_UPDATE Update;

    if (Events->Owner == NULL || Events->Owner->Abandoned || Events->Owner != g_Current)
        return;
    Update = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Update));
    if (Update == NULL)
        return;
    Update->Owner = Events->Owner;
    Update->Field = Field;
    Update->Kind = Kind;
    Update->Value = Value;
    if (Text)
        StringCchCopyW(Update->Text, ARRAYSIZE(Update->Text), Text);
    if (!PostMessageW(g_Window, WM_LUH_FIELD, 0, (LPARAM)Update))
        HeapFree(GetProcessHeap(), 0, Update);
}

static HRESULT WINAPI Events_SetFieldState(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, CREDENTIAL_PROVIDER_FIELD_STATE cpfs)
{
    PostFieldUpdate(EventsFrom(iface), dwFieldID, LuhFieldState, cpfs, NULL);
    return S_OK;
}

static HRESULT WINAPI Events_SetFieldInteractiveState(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE cpfis)
{
    PostFieldUpdate(EventsFrom(iface), dwFieldID, LuhFieldInteractive, cpfis, NULL);
    return S_OK;
}

static HRESULT WINAPI Events_SetFieldString(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, LPCWSTR psz)
{
    PostFieldUpdate(EventsFrom(iface), dwFieldID, LuhFieldText, 0, psz ? psz : L"");
    return S_OK;
}

static HRESULT WINAPI Events_SetFieldCheckbox(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, BOOL bChecked, LPCWSTR pszLabel)
{
    PostFieldUpdate(EventsFrom(iface), dwFieldID, LuhFieldCheckbox, bChecked ? 1 : 0, pszLabel);
    return S_OK;
}

static HRESULT WINAPI Events_SetFieldBitmap(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, HBITMAP hbmp)
{
    return S_OK;    /* tiles are Phase 4 */
}

static HRESULT WINAPI Events_SetFieldComboBoxSelectedItem(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, DWORD dwSelectedItem)
{
    return S_OK;
}

static HRESULT WINAPI Events_DeleteFieldComboBoxItem(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, DWORD dwItem)
{
    return S_OK;
}

static HRESULT WINAPI Events_AppendFieldComboBoxItem(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, LPCWSTR pszItem)
{
    return S_OK;
}

static HRESULT WINAPI Events_SetFieldSubmitButton(ICredentialProviderCredentialEvents *iface,
    ICredentialProviderCredential *pcpc, DWORD dwFieldID, DWORD dwAdjacentTo)
{
    return S_OK;
}

static HRESULT WINAPI Events_OnCreatingWindow(ICredentialProviderCredentialEvents *iface, HWND *phwndOwner)
{
    *phwndOwner = g_Window;
    return S_OK;
}

static const ICredentialProviderCredentialEventsVtbl EventsVtbl =
{
    Events_QueryInterface, Events_AddRef, Events_Release,
    Events_SetFieldState, Events_SetFieldInteractiveState, Events_SetFieldString,
    Events_SetFieldCheckbox, Events_SetFieldBitmap, Events_SetFieldComboBoxSelectedItem,
    Events_DeleteFieldComboBoxItem, Events_AppendFieldComboBoxItem, Events_SetFieldSubmitButton,
    Events_OnCreatingWindow
};

/* ---- provider thread --------------------------------------------------------- */

static BOOL
IsFaulted(
    _In_ REFCLSID Clsid)
{
    ULONG i;
    for (i = 0; i < g_FaultCount; i++)
        if (IsEqualGUID(&g_Faulted[i], Clsid))
            return TRUE;
    return FALSE;
}

/* Picks the first provider that is not faulted and offers a credential. */
/* The text a tile shows: the first large-text field the credential puts in a deselected tile. */
static VOID
TileName(
    _In_ ICredentialProvider *Provider,
    _In_ ICredentialProviderCredential *Credential,
    _Out_writes_(Chars) PWSTR Name,
    _In_ SIZE_T Chars)
{
    DWORD Count = 0, i;

    Name[0] = UNICODE_NULL;
    if (FAILED(ICredentialProvider_GetFieldDescriptorCount(Provider, &Count)))
        return;
    for (i = 0; i < Count && Name[0] == UNICODE_NULL; i++)
    {
        CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *Desc = NULL;
        CREDENTIAL_PROVIDER_FIELD_STATE State = CPFS_HIDDEN;
        CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE Interactive = CPFIS_NONE;
        PWSTR Value = NULL;
        DWORD Id;
        BOOL Large;

        if (FAILED(ICredentialProvider_GetFieldDescriptorAt(Provider, i, &Desc)) || Desc == NULL)
            continue;
        Id = Desc->dwFieldID;
        Large = Desc->cpft == CPFT_LARGE_TEXT;
        CoTaskMemFree(Desc->pszLabel);
        CoTaskMemFree(Desc);
        if (!Large ||
            FAILED(ICredentialProviderCredential_GetFieldState(Credential, Id, &State, &Interactive)) ||
            (State != CPFS_DISPLAY_IN_DESELECTED_TILE && State != CPFS_DISPLAY_IN_BOTH))
            continue;
        if (SUCCEEDED(ICredentialProviderCredential_GetStringValue(Credential, Id, &Value)) && Value)
        {
            StringCchCopyW(Name, Chars, Value);
            CoTaskMemFree(Value);
        }
    }
}

/* TRUE for the empty ("Other user") tile: a credential with no user behind it (ICredentialProviderCredential2). */
static BOOL
TileIsEmpty(
    _In_ ICredentialProviderCredential *Credential)
{
    ICredentialProviderCredential2 *Credential2 = NULL;
    LPWSTR Sid = NULL;
    BOOL Empty = FALSE;

    if (SUCCEEDED(ICredentialProviderCredential_QueryInterface(Credential, &IID_ICredentialProviderCredential2,
                                                               (void **)&Credential2)) && Credential2 != NULL)
    {
        Empty = ICredentialProviderCredential2_GetUserSid(Credential2, &Sid) != S_OK || Sid == NULL;
        CoTaskMemFree(Sid);
        ICredentialProviderCredential2_Release(Credential2);
    }
    return Empty;
}

static BOOL
ReadPolicyFlag(
    _In_ PCWSTR Key,
    _In_ PCWSTR Name)
{
    DWORD Value = 0, Size = sizeof(Value);

    return RegGetValueW(HKEY_LOCAL_MACHINE, Key, Name, RRF_RT_REG_DWORD, NULL, &Value, &Size) == ERROR_SUCCESS &&
           Value != 0;
}

static BOOL
ReadPolicyText(
    _In_ PCWSTR Key,
    _In_ PCWSTR Name,
    _Out_writes_(Chars) PWSTR Text,
    _In_ DWORD Chars)
{
    DWORD Size = Chars * sizeof(WCHAR);

    Text[0] = UNICODE_NULL;
    return RegGetValueW(HKEY_LOCAL_MACHINE, Key, Name, RRF_RT_REG_SZ, NULL, Text, &Size) == ERROR_SUCCESS &&
           Text[0] != UNICODE_NULL;
}

/*
 * Every credential of every provider becomes a tile. The default one: the empty tile when
 * DontDisplayLastUserName is set, else the first default a provider names, else the
 * DefaultUserName account the msgina page preselects, else the first tile.
 */
static VOID
CollectTiles(
    _In_ PLUH_PROVIDER_THREAD T)
{
    WCHAR LastUser[LUH_TILE_TEXT];
    DWORD ProviderDefault = MAXDWORD, i, j;
    BOOL HideLast, ProviderAutoLogon = FALSE;

    T->TileCount = 0;
    T->AutoSubmit = FALSE;
    for (i = 0; i < T->Set->Count && T->TileCount < LUH_MAX_TILES; i++)
    {
        ICredentialProvider *Provider = T->Set->Providers[i].Provider;
        DWORD Count = 0, Default = CREDENTIAL_PROVIDER_NO_DEFAULT;
        BOOL AutoLogon = FALSE;

        if (IsFaulted(&T->Set->Providers[i].Clsid))
            continue;
        if (FAILED(ICredentialProvider_GetCredentialCount(Provider, &Count, &Default, &AutoLogon)) || Count == 0)
            continue;
        if (Default >= Count)
            AutoLogon = FALSE;      /* CREDENTIAL_PROVIDER_NO_DEFAULT: nothing to submit */
        for (j = 0; j < Count && T->TileCount < LUH_MAX_TILES; j++)
        {
            LUH_TILE *Tile = &T->Tiles[T->TileCount];

            Tile->Credential = NULL;
            if (FAILED(ICredentialProvider_GetCredentialAt(Provider, j, &Tile->Credential)) || Tile->Credential == NULL)
                continue;
            Tile->Provider = Provider;
            Tile->Clsid = T->Set->Providers[i].Clsid;
            TileName(Provider, Tile->Credential, Tile->Name, ARRAYSIZE(Tile->Name));
            if (j == Default && ProviderDefault == MAXDWORD)
            {
                ProviderDefault = T->TileCount;
                ProviderAutoLogon = AutoLogon;
            }
            T->TileCount++;
        }
    }

    T->Selected = 0;
    HideLast = g_Options.Scenario == CPUS_LOGON &&
               ReadPolicyFlag(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                              L"DontDisplayLastUserName");
    if (HideLast)
    {
        for (i = 0; i < T->TileCount; i++)
        {
            if (TileIsEmpty(T->Tiles[i].Credential))
            {
                T->Selected = i;
                return;
            }
        }
    }
    if (ProviderDefault != MAXDWORD)
    {
        /* Windows submits the provider's default at once when it asks (pbAutoLogonWithDefault). */
        T->Selected = ProviderDefault;
        T->AutoSubmit = ProviderAutoLogon;
        return;
    }
    if (!HideLast &&
        ReadPolicyText(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"DefaultUserName",
                       LastUser, ARRAYSIZE(LastUser)))
    {
        for (i = 0; i < T->TileCount; i++)
        {
            if (!TileIsEmpty(T->Tiles[i].Credential) && !_wcsicmp(T->Tiles[i].Name, LastUser))
            {
                T->Selected = i;
                return;
            }
        }
    }
}

static VOID
ReleaseTiles(
    _In_ PLUH_PROVIDER_THREAD T)
{
    DWORD i;

    for (i = 0; i < T->TileCount; i++)
        ICredentialProviderCredential_Release(T->Tiles[i].Credential);
    ZeroMemory(T->Tiles, sizeof(T->Tiles));
    T->TileCount = 0;
}

/* Leaves the selected tile (SetDeselected, UnAdvise) and selects tile Index (Advise, SetSelected). */
static BOOL
SelectTile(
    _In_ PLUH_PROVIDER_THREAD T,
    _In_ DWORD Index)
{
    LUH_TILE *Tile;
    BOOL AutoLogon = FALSE;

    if (Index >= T->TileCount)
        return FALSE;
    if (T->Credential != NULL)
    {
        ICredentialProviderCredential_SetDeselected(T->Credential);
        ICredentialProviderCredential_UnAdvise(T->Credential);
        T->Credential = NULL;
        T->Provider = NULL;
    }
    Tile = &T->Tiles[Index];
    if (FAILED(ICredentialProviderCredential_Advise(Tile->Credential, &T->Events->ICredentialProviderCredentialEvents_iface)))
        return FALSE;
    ICredentialProviderCredential_SetSelected(Tile->Credential, &AutoLogon);
    T->Provider = Tile->Provider;
    T->Credential = Tile->Credential;
    T->CredentialClsid = Tile->Clsid;
    T->Selected = Index;
    return TRUE;
}

/* The first tile that takes the selection, starting at the default. */
static BOOL
SelectCredential(
    _In_ PLUH_PROVIDER_THREAD T)
{
    DWORD i;

    CollectTiles(T);
    if (T->TileCount == 0)
        return FALSE;
    if (SelectTile(T, T->Selected))
        return TRUE;
    for (i = 0; i < T->TileCount; i++)
    {
        if (SelectTile(T, i))
            return TRUE;
    }
    return FALSE;
}

/* Reads the field descriptors and the credential's view of them into the page model. */
static VOID
BuildPage(
    _In_ PLUH_PROVIDER_THREAD T,
    _Out_ PLUH_PAGE Page)
{
    DWORD Count = 0, i;

    ZeroMemory(Page, sizeof(*Page));
    Page->Scenario = g_Options.Scenario;
    for (i = 0; i < T->TileCount; i++)
        StringCchCopyW(Page->TileNames[i], ARRAYSIZE(Page->TileNames[i]), T->Tiles[i].Name);
    Page->TileCount = T->TileCount;
    Page->TileSelected = T->Selected;
    Page->AutoSubmit = T->AutoSubmit;
    T->AutoSubmit = FALSE;
    if (FAILED(ICredentialProvider_GetFieldDescriptorCount(T->Provider, &Count)))
        return;

    for (i = 0; i < Count && Page->Count < LUH_MAX_FIELDS; i++)
    {
        CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR *Desc = NULL;
        CREDENTIAL_PROVIDER_FIELD_STATE State = CPFS_HIDDEN;
        CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE Interactive = CPFIS_NONE;
        PLUH_FIELD Field;
        PWSTR Value = NULL;

        if (FAILED(ICredentialProvider_GetFieldDescriptorAt(T->Provider, i, &Desc)) || Desc == NULL)
            continue;

        Field = &Page->Fields[Page->Count];
        Field->Id = Desc->dwFieldID;
        Field->Type = Desc->cpft;
        if (Desc->pszLabel)
            StringCchCopyW(Field->Label, ARRAYSIZE(Field->Label), Desc->pszLabel);
        CoTaskMemFree(Desc->pszLabel);
        CoTaskMemFree(Desc);

        if (SUCCEEDED(ICredentialProviderCredential_GetFieldState(T->Credential, Field->Id, &State, &Interactive)))
        {
            Field->State = State;
            Field->Interactive = Interactive;
        }
        if ((Field->Type == CPFT_LARGE_TEXT || Field->Type == CPFT_SMALL_TEXT ||
             Field->Type == CPFT_EDIT_TEXT || Field->Type == CPFT_COMMAND_LINK ||
             Field->Type == CPFT_SUBMIT_BUTTON) &&
            SUCCEEDED(ICredentialProviderCredential_GetStringValue(T->Credential, Field->Id, &Value)) && Value)
        {
            StringCchCopyW(Field->Value, ARRAYSIZE(Field->Value), Value);
            CoTaskMemFree(Value);
        }
        if (Field->Type == CPFT_SUBMIT_BUTTON)
        {
            DWORD Adjacent = 0;
            if (SUCCEEDED(ICredentialProviderCredential_GetSubmitButtonValue(T->Credential, Field->Id, &Adjacent)))
                Page->SubmitAdjacentTo = Adjacent;
            Page->SubmitField = Field->Id;
        }
        Page->Count++;
    }
}

/* The page is handed to the UI thread, which frees it when it tears the page down.
 * A thread the UI already gave up on drops its late page right here, traced the
 * same way as a drop in the UI's WM_LUH_PAGE handler. */
static VOID
PostPage(
    _In_ PLUH_PROVIDER_THREAD T)
{
    PLUH_PAGE Page = T->Abandoned ? NULL : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Page));

    if (Page != NULL)
    {
        BuildPage(T, Page);
        Page->Owner = T;
        if (T->Abandoned || !PostMessageW(g_Window, WM_LUH_PAGE, LUH_PAGE_READY, (LPARAM)Page))
        {
            SecureZeroMemory(Page, sizeof(*Page));
            HeapFree(GetProcessHeap(), 0, Page);
            if (T->Abandoned)
                LuhTrace("page-dropped", NULL);
        }
    }
    else if (!T->Abandoned)
    {
        PostMessageW(g_Window, WM_LUH_PAGE, LUH_PAGE_FAILED, (LPARAM)T);
    }
    else
    {
        LuhTrace("page-dropped", NULL);
    }
}

static VOID
ServeRequest(
    _In_ PLUH_PROVIDER_THREAD T)
{
    LUH_REQUEST *R = &T->Request;
    PWSTR Text = NULL;
    CREDENTIAL_PROVIDER_STATUS_ICON Icon = CPSI_NONE;

    R->Hr = E_UNEXPECTED;
    R->StatusText[0] = UNICODE_NULL;
    if (T->Credential == NULL && R->Type != LuhReqSelectTile)
    {
        SecureZeroMemory(R->Text, sizeof(R->Text));
        return;                     /* a failed switch left no tile selected */
    }

    switch (R->Type)
    {
        case LuhReqSetString:
            R->Hr = ICredentialProviderCredential_SetStringValue(T->Credential, R->Field, R->Text);
            break;

        case LuhReqGetSerialization:
            ZeroMemory(&R->Serialization, sizeof(R->Serialization));
            R->Response = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
            R->Hr = ICredentialProviderCredential_GetSerialization(T->Credential, &R->Response,
                                                                   &R->Serialization, &Text, &Icon);
            break;

        case LuhReqReportResult:
            R->Hr = ICredentialProviderCredential_ReportResult(T->Credential, R->Status, R->SubStatus, &Text, &Icon);
            break;

        case LuhReqDeselect:
            R->Hr = ICredentialProviderCredential_SetDeselected(T->Credential);
            break;

        case LuhReqSelectTile:
        {
            DWORD Previous = T->Selected;

            if (R->Field == Previous || R->Field >= T->TileCount)
            {
                R->Hr = E_INVALIDARG;
                break;
            }
            /* A tile that refuses the selection puts the previous one back. */
            R->Hr = SelectTile(T, R->Field) ? S_OK : (SelectTile(T, Previous) ? S_FALSE : E_FAIL);
            if (R->Hr == S_OK)
                PostPage(T);
            break;
        }

        default:
            break;
    }

    if (Text)
    {
        StringCchCopyW(R->StatusText, ARRAYSIZE(R->StatusText), Text);
        CoTaskMemFree(Text);
    }
    SecureZeroMemory(R->Text, sizeof(R->Text));
}

/* A forced change names its account: the providers get a logon serialization that carries
 * only DOMAIN\user and no password, so the page comes up with the name filled in. */
static VOID
PrefillProviders(
    _In_ PCPH_PROVIDER_SET Set,
    _In_ PCWSTR Prefill)
{
    WCHAR Domain[ARRAYSIZE(g_Options.PrefillUser)];
    BYTE Buffer[4096];
    ULONG Size = sizeof(Buffer);
    PCWSTR User = Prefill;
    PCWSTR Slash = wcschr(Prefill, L'\\');
    ULONG i;

    Domain[0] = UNICODE_NULL;
    if (Slash != NULL)
    {
        StringCchCopyNW(Domain, ARRAYSIZE(Domain), Prefill, Slash - Prefill);
        User = Slash + 1;
    }
    if (User[0] != UNICODE_NULL &&
        CredSerPackLogon(KerbInteractiveLogon, Domain[0] ? Domain : NULL, User, L"", NULL, Buffer, &Size) >= 0)
    {
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION Serialization;

        ZeroMemory(&Serialization, sizeof(Serialization));
        Serialization.clsidCredentialProvider = GUID_NULL;
        Serialization.rgbSerialization = Buffer;
        Serialization.cbSerialization = Size;
        for (i = 0; i < Set->Count; i++)
            ICredentialProvider_SetSerialization(Set->Providers[i].Provider, &Serialization);
    }
    SecureZeroMemory(Buffer, sizeof(Buffer));
}

/* Windows order: SetUsageScenario (done by the enumeration), then SetUserArray, then the
 * first GetCredentialCount. Logon and unlock only; the other scenarios keep one empty tile. */
static VOID
SetUserArrays(
    _In_ PCPH_PROVIDER_SET Set)
{
    ICredentialProviderUserArray *Users = NULL;
    WCHAR Num[16];
    DWORD Count = 0;
    ULONG i;

    if (g_Options.Scenario != CPUS_LOGON && g_Options.Scenario != CPUS_UNLOCK_WORKSTATION)
        return;
    if (FAILED(LuhUsersCreate(g_Options.Scenario == CPUS_UNLOCK_WORKSTATION, g_Options.PrefillUser, &Users)))
        return;
    ICredentialProviderUserArray_GetCount(Users, &Count);
    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%lu", Count);
    LuhTrace("users", Num);
    for (i = 0; i < Set->Count; i++)
    {
        ICredentialProviderSetUserArray *Sink = NULL;

        if (IsFaulted(&Set->Providers[i].Clsid))
            continue;
        if (SUCCEEDED(ICredentialProvider_QueryInterface(Set->Providers[i].Provider, &IID_ICredentialProviderSetUserArray,
                                                         (void **)&Sink)) && Sink != NULL)
        {
            ICredentialProviderSetUserArray_SetUserArray(Sink, Users);
            ICredentialProviderSetUserArray_Release(Sink);
        }
    }
    ICredentialProviderUserArray_Release(Users);
}

static DWORD WINAPI
ProviderThread(
    _In_ LPVOID Parameter)
{
    PLUH_PROVIDER_THREAD T = Parameter;
    LUH_EVENTS *Events;
    HRESULT hr;
    HANDLE Waits[2];
    WCHAR Num[16];

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
    {
        PostMessageW(g_Window, WM_LUH_PAGE, LUH_PAGE_FAILED, (LPARAM)T);
        return 0;
    }

    Events = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Events));
    if (Events == NULL)
    {
        PostMessageW(g_Window, WM_LUH_PAGE, LUH_PAGE_FAILED, (LPARAM)T);
        CoUninitialize();
        return 0;
    }
    Events->ICredentialProviderCredentialEvents_iface.lpVtbl = &EventsVtbl;
    Events->RefCount = 1;
    Events->Owner = T;

    /* The host runs as SYSTEM and never impersonates (Q3), so the class lookup is not
     * steerable by a user hive; the fallback never touches the class registration (Q5). */
    hr = CredProvHostEnumerateProviders((CREDENTIAL_PROVIDER_USAGE_SCENARIO)g_Options.Scenario, 0,
                                        g_Options.Fallback ? CPH_FLAG_FALLBACK : 0, &T->Set);
    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%lu", T->Set ? T->Set->Count : 0);
    LuhTrace("providers", Num);
    /* On unlock the name only picks the account for the user array; Other user stays empty. */
    if (SUCCEEDED(hr) && T->Set != NULL && g_Options.PrefillUser[0] != UNICODE_NULL &&
        g_Options.Scenario != CPUS_UNLOCK_WORKSTATION)
        PrefillProviders(T->Set, g_Options.PrefillUser);
    if (SUCCEEDED(hr) && T->Set != NULL)
        SetUserArrays(T->Set);

    T->Events = Events;
    if (FAILED(hr) || T->Set == NULL || !SelectCredential(T))
    {
        WPARAM Why = (T->Set && T->Set->PolicyDeniesPassword) ? LUH_PAGE_POLICY : LUH_PAGE_FAILED;
        ReleaseTiles(T);
        ICredentialProviderCredentialEvents_Release(&Events->ICredentialProviderCredentialEvents_iface);
        T->Events = NULL;
        CredProvHostFreeProviders(T->Set);
        T->Set = NULL;
        /* A thread the UI already gave up on must not steer it; the UI re-checks the owner. */
        if (!T->Abandoned)
            PostMessageW(g_Window, WM_LUH_PAGE, Why, (LPARAM)T);
        CoUninitialize();
        return 0;
    }

    PostPage(T);

    Waits[0] = T->StopEvent;
    Waits[1] = T->RequestEvent;
    for (;;)
    {
        DWORD Which = WaitForMultipleObjects(2, Waits, FALSE, INFINITE);
        if (Which != WAIT_OBJECT_0 + 1)
            break;
        ServeRequest(T);
        SetEvent(T->DoneEvent);
    }

    if (T->Credential != NULL)
    {
        ICredentialProviderCredential_SetDeselected(T->Credential);
        ICredentialProviderCredential_UnAdvise(T->Credential);
    }
    ReleaseTiles(T);
    ICredentialProviderCredentialEvents_Release(&Events->ICredentialProviderCredentialEvents_iface);
    T->Events = NULL;
    CredProvHostFreeProviders(T->Set);
    CoUninitialize();
    return 0;
}

static PLUH_PROVIDER_THREAD
StartProviderThread(VOID)
{
    PLUH_PROVIDER_THREAD T = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*T));

    if (T == NULL)
        return NULL;
    T->RequestEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    T->DoneEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    T->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (T->RequestEvent && T->DoneEvent && T->StopEvent)
        T->Thread = CreateThread(NULL, 0, ProviderThread, T, 0, NULL);
    if (T->Thread == NULL)
    {
        if (T->RequestEvent) CloseHandle(T->RequestEvent);
        if (T->DoneEvent) CloseHandle(T->DoneEvent);
        if (T->StopEvent) CloseHandle(T->StopEvent);
        HeapFree(GetProcessHeap(), 0, T);
        return NULL;
    }
    return T;
}

/* ---- UI-thread side ----------------------------------------------------------- */

BOOL
LuhProvidersStart(
    _In_ HWND Window)
{
    g_Window = Window;
    g_Current = StartProviderThread();
    return g_Current != NULL;
}

VOID
LuhProvidersStop(VOID)
{
    PLUH_PROVIDER_THREAD T = g_Current;

    g_Current = NULL;
    if (T == NULL)
        return;
    SetEvent(T->StopEvent);
    if (!T->Abandoned)
        WaitForSingleObject(T->Thread, 3000);
    /* An abandoned thread keeps its handles; the process is about to leave. */
}

BOOL
LuhProvidersIsCurrent(
    _In_ PVOID Owner)
{
    return Owner != NULL && Owner == g_Current && !g_Current->Abandoned;
}

/* A provider that does not answer in time is dropped; the host carries on without it. */
static VOID
ProviderFault(
    _In_ PLUH_PROVIDER_THREAD T)
{
    WCHAR Text[64];

    if (g_FaultCount < LUH_MAX_FAULTS)
        g_Faulted[g_FaultCount++] = T->CredentialClsid;
    StringCchPrintfW(Text, ARRAYSIZE(Text), L"{%08lX-...} timeout", T->CredentialClsid.Data1);
    LuhTrace("provider-fault", Text);
    WluiClientReportProviderFault(&g_Connection, &T->CredentialClsid, LUH_FAULT_TIMEOUT);

    T->Abandoned = TRUE;
    SetEvent(T->StopEvent);
    if (g_Current == T)
        g_Current = StartProviderThread();   /* re-enumerates without the faulted provider */
}

/*
 * No page arrived in time, so some provider wedged during start-up where the
 * CLSID cannot be told. Try once more with the fallback tier (the direct-loaded
 * PasswordProvider only); when that also fails there is nothing left to show.
 */
BOOL
LuhProvidersStartupTimeout(VOID)
{
    PLUH_PROVIDER_THREAD T = g_Current;

    LuhTrace("provider-fault", L"startup timeout");
    WluiClientReportProviderFault(&g_Connection, &CLSID_NULL, LUH_FAULT_STARTUP);
    if (T != NULL)
    {
        T->Abandoned = TRUE;
        SetEvent(T->StopEvent);
    }
    if (g_Options.Fallback)
    {
        g_Current = NULL;
        return FALSE;
    }
    g_Options.Fallback = TRUE;
    g_Current = StartProviderThread();
    return g_Current != NULL;
}

/*
 * Waits for the provider thread with a bound, servicing sent messages so provider
 * UI owned by our window cannot deadlock against us, and sending the heartbeat from
 * inside the wait: this thread is provably alive here, and winlogon's hang window
 * (three missed 2 s beats) is shorter than the 10 s call bound.
 */
static BOOL
WaitForProvider(
    _In_ PLUH_PROVIDER_THREAD T)
{
    ULONGLONG Start = GetTickCount64(), LastBeat = Start;

    for (;;)
    {
        ULONGLONG Elapsed = GetTickCount64() - Start;
        DWORD Slice;
        DWORD Result;

        if (Elapsed >= LUH_PROVIDER_CALL_MS)
            return FALSE;
        Slice = (DWORD)min(LUH_PROVIDER_CALL_MS - Elapsed, 500);
        Result = MsgWaitForMultipleObjects(1, &T->DoneEvent, FALSE, Slice, QS_SENDMESSAGE);
        if (Result == WAIT_OBJECT_0)
            return TRUE;
        if (Result == WAIT_OBJECT_0 + 1)
        {
            MSG Msg;
            PeekMessageW(&Msg, NULL, 0, 0, PM_NOREMOVE);   /* delivers sent messages only */
        }
        if (GetTickCount64() - LastBeat >= LUH_HEARTBEAT_MS)
        {
            LuhIpcHeartbeat();
            LastBeat = GetTickCount64();
        }
    }
}

static BOOL
ProviderCall(
    _Inout_ LUH_REQUEST *Request)
{
    PLUH_PROVIDER_THREAD T = g_Current;

    if (T == NULL || T->Abandoned)
        return FALSE;

    T->Request = *Request;
    SetEvent(T->RequestEvent);
    if (!WaitForProvider(T))
    {
        ProviderFault(T);
        return FALSE;
    }
    *Request = T->Request;
    SecureZeroMemory(&T->Request, sizeof(T->Request));
    return TRUE;
}

BOOL
LuhProviderSetString(
    _In_ DWORD Field,
    _In_ PCWSTR Text)
{
    LUH_REQUEST R;
    BOOL Ok;

    ZeroMemory(&R, sizeof(R));
    R.Type = LuhReqSetString;
    R.Field = Field;
    StringCchCopyW(R.Text, ARRAYSIZE(R.Text), Text);
    Ok = ProviderCall(&R) && SUCCEEDED(R.Hr);
    SecureZeroMemory(&R, sizeof(R));
    return Ok;
}

/* Asks the credential for its serialization and hands it to winlogon. */
LUH_SUBMIT_RESULT
LuhProviderSubmit(
    _Out_writes_(StatusChars) PWSTR StatusText,
    _In_ SIZE_T StatusChars)
{
    LUH_REQUEST R;
    LUH_SUBMIT_RESULT Result = LuhSubmitFailed;
    DWORD Error;

    StatusText[0] = UNICODE_NULL;
    ZeroMemory(&R, sizeof(R));
    R.Type = LuhReqGetSerialization;
    if (!ProviderCall(&R))
        return LuhSubmitProviderGone;

    StringCchCopyW(StatusText, StatusChars, R.StatusText);
    if (FAILED(R.Hr))
    {
        Result = LuhSubmitFailed;
    }
    else if (R.Response == CPGSR_RETURN_CREDENTIAL_FINISHED && R.Serialization.rgbSerialization != NULL)
    {
        Error = WluiClientSubmitSerialization(&g_Connection, g_Options.Scenario,
                                              &R.Serialization.clsidCredentialProvider,
                                              R.Serialization.ulAuthenticationPackage,
                                              R.Serialization.rgbSerialization,
                                              R.Serialization.cbSerialization);
        LuhTrace("submitted", Error == ERROR_SUCCESS ? L"ok" : L"refused");
        Result = (Error == ERROR_SUCCESS) ? LuhSubmitSent : LuhSubmitRefused;
    }
    else
    {
        Result = LuhSubmitNotFinished;  /* the provider wants more input; its text says what */
    }

    if (R.Serialization.rgbSerialization != NULL)
    {
        SecureZeroMemory(R.Serialization.rgbSerialization, R.Serialization.cbSerialization);
        CoTaskMemFree(R.Serialization.rgbSerialization);
    }
    SecureZeroMemory(&R, sizeof(R));
    return Result;
}

BOOL
LuhProviderReportResult(
    _In_ NTSTATUS Status,
    _In_ NTSTATUS SubStatus,
    _Out_writes_(StatusChars) PWSTR StatusText,
    _In_ SIZE_T StatusChars)
{
    LUH_REQUEST R;
    BOOL Ok;

    StatusText[0] = UNICODE_NULL;
    ZeroMemory(&R, sizeof(R));
    R.Type = LuhReqReportResult;
    R.Status = Status;
    R.SubStatus = SubStatus;
    Ok = ProviderCall(&R);
    if (Ok)
        StringCchCopyW(StatusText, StatusChars, R.StatusText);
    SecureZeroMemory(&R, sizeof(R));
    return Ok;
}

/* The user picked another tile: the provider thread switches and posts the new page. */
BOOL
LuhProviderSelectTile(
    _In_ DWORD Index)
{
    LUH_REQUEST R;

    ZeroMemory(&R, sizeof(R));
    R.Type = LuhReqSelectTile;
    R.Field = Index;
    return ProviderCall(&R) && SUCCEEDED(R.Hr);
}

VOID
LuhProviderDeselect(VOID)
{
    LUH_REQUEST R;

    ZeroMemory(&R, sizeof(R));
    R.Type = LuhReqDeselect;
    ProviderCall(&R);
}
