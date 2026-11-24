/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Credential providers: enumeration, the selected credential,
 *              bounded calls with a per-call timeout, serialization hand-off
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include <credprovhost.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

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
    LuhReqDeselect
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
    ICredentialProviderCredential *Credential;
} LUH_PROVIDER_THREAD, *PLUH_PROVIDER_THREAD;

static PLUH_PROVIDER_THREAD g_Current;
static HWND g_Window;
static CLSID g_Faulted[LUH_MAX_FAULTS];
static ULONG g_FaultCount;

/* ---- events sink: the provider may change a field while the page is up ---- */

typedef struct _LUH_EVENTS
{
    ICredentialProviderCredentialEvents ICredentialProviderCredentialEvents_iface;
    LONG RefCount;
    PLUH_PROVIDER_THREAD Owner;     /* updates from an abandoned thread's credential are dropped */
} LUH_EVENTS;

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
static BOOL
SelectCredential(
    _In_ PLUH_PROVIDER_THREAD T,
    _In_ LUH_EVENTS *Events)
{
    ULONG i;

    for (i = 0; i < T->Set->Count; i++)
    {
        ICredentialProvider *Provider = T->Set->Providers[i].Provider;
        ICredentialProviderCredential *Credential = NULL;
        DWORD Count = 0, Default = 0;
        BOOL AutoLogon = FALSE;

        if (IsFaulted(&T->Set->Providers[i].Clsid))
            continue;
        if (FAILED(ICredentialProvider_GetCredentialCount(Provider, &Count, &Default, &AutoLogon)) || Count == 0)
            continue;
        if (FAILED(ICredentialProvider_GetCredentialAt(Provider, (Default < Count) ? Default : 0, &Credential)))
            continue;
        if (FAILED(ICredentialProviderCredential_Advise(Credential, &Events->ICredentialProviderCredentialEvents_iface)))
        {
            ICredentialProviderCredential_Release(Credential);
            continue;
        }
        ICredentialProviderCredential_SetSelected(Credential, &AutoLogon);
        T->Provider = Provider;
        T->Credential = Credential;
        T->CredentialClsid = T->Set->Providers[i].Clsid;
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

static VOID
ServeRequest(
    _In_ PLUH_PROVIDER_THREAD T)
{
    LUH_REQUEST *R = &T->Request;
    PWSTR Text = NULL;
    CREDENTIAL_PROVIDER_STATUS_ICON Icon = CPSI_NONE;

    R->Hr = E_UNEXPECTED;
    R->StatusText[0] = UNICODE_NULL;

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

    if (FAILED(hr) || T->Set == NULL || !SelectCredential(T, Events))
    {
        WPARAM Why = (T->Set && T->Set->PolicyDeniesPassword) ? LUH_PAGE_POLICY : LUH_PAGE_FAILED;
        ICredentialProviderCredentialEvents_Release(&Events->ICredentialProviderCredentialEvents_iface);
        CredProvHostFreeProviders(T->Set);
        T->Set = NULL;
        /* A thread the UI already gave up on must not steer it; the UI re-checks the owner. */
        if (!T->Abandoned)
            PostMessageW(g_Window, WM_LUH_PAGE, Why, (LPARAM)T);
        CoUninitialize();
        return 0;
    }

    /* The page is handed to the UI thread, which frees it when it tears the page down. */
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
            }
        }
        else if (!T->Abandoned)
        {
            PostMessageW(g_Window, WM_LUH_PAGE, LUH_PAGE_FAILED, (LPARAM)T);
        }
    }

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

    ICredentialProviderCredential_SetDeselected(T->Credential);
    ICredentialProviderCredential_UnAdvise(T->Credential);
    ICredentialProviderCredential_Release(T->Credential);
    ICredentialProviderCredentialEvents_Release(&Events->ICredentialProviderCredentialEvents_iface);
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

VOID
LuhProviderDeselect(VOID)
{
    LUH_REQUEST R;

    ZeroMemory(&R, sizeof(R));
    R.Type = LuhReqDeselect;
    ProviderCall(&R);
}
