/*
 * PROJECT:     LiberNT Windows Firewall service
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Windows Firewall service lifecycle and policy reload orchestration
 * COPYRIGHT:   Copyright 2026 Ostap Brehin <osbre@protonmail.com>
 *              Copyright 2026 LeewayOpenSoftware (profiles, default inbound action)
 */

#define COBJMACROS
#include <windows.h>
#include <winsvc.h>
#include <rpc.h>
#include <fwpmu.h>
#include <ole2.h>
#include <netlistmgr.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "mpssvc_policy.h"

#define MPS_SERVICE_NAME L"MpsSvc"
#define MPS_POLICY_KEY L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy"
#define MPS_RULES_SUBKEY L"FirewallRules"
#define MPS_MAX_RULES 4096
#define MPS_NETWORK_SETTLE_MS 2000

/* The Network List Manager (netprofm): the category of the connected networks */
static const CLSID MpsClsidNetworkListManager =
    { 0xdcb00c01, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };
static const IID MpsIidNetworkListManager =
    { 0xdcb00000, 0x570f, 0x4a9b, { 0x8d, 0x69, 0x19, 0x9f, 0xdb, 0xa5, 0x72, 0x3b } };

static SERVICE_STATUS_HANDLE MpsStatusHandle;
static SERVICE_STATUS MpsStatus;
static HANDLE MpsStopEvent;

static VOID
MpsReportStatus(
    _In_ DWORD State,
    _In_ DWORD Error)
{
    MpsStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    MpsStatus.dwCurrentState = State;
    MpsStatus.dwWin32ExitCode = Error;
    MpsStatus.dwServiceSpecificExitCode = 0;
    MpsStatus.dwControlsAccepted = State == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    MpsStatus.dwCheckPoint = 0;
    MpsStatus.dwWaitHint = 0;
    SetServiceStatus(MpsStatusHandle, &MpsStatus);
}

static DWORD WINAPI
MpsControlHandler(
    _In_ DWORD Control,
    _In_ DWORD EventType,
    _In_opt_ LPVOID EventData,
    _In_opt_ LPVOID Context)
{
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Context);

    if (Control == SERVICE_CONTROL_STOP || Control == SERVICE_CONTROL_SHUTDOWN)
    {
        MpsReportStatus(SERVICE_STOP_PENDING, NO_ERROR);
        if (!SetEvent(MpsStopEvent))
            return GetLastError();
        return NO_ERROR;
    }

    if (Control == SERVICE_CONTROL_INTERROGATE)
    {
        SetServiceStatus(MpsStatusHandle, &MpsStatus);
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

static DWORD
MpsOpenEngine(
    _In_ UINT32 Flags,
    _Out_ HANDLE *Engine)
{
    FWPM_SESSION0 Session;

    ZeroMemory(&Session, sizeof(Session));
    Session.flags = Flags;
    return FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &Session, Engine);
}

static DWORD
MpsAddSubLayer(VOID)
{
    HANDLE Engine;
    DWORD Error;

    Error = MpsOpenEngine(0, &Engine);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = MpsAddFirewallSubLayer(Engine, &FWPM_SUBLAYER_MPSSVC_WF);
    FwpmEngineClose0(Engine);
    return Error;
}

/* The profile in force: the most restrictive category among the connected networks
   (Public, then Private, then Domain); Public when there is none or it cannot be told */
static UINT32
MpsActiveProfile(VOID)
{
    INetworkListManager *List = NULL;
    IEnumNetworks *Networks = NULL;
    INetwork *Network;
    ULONG Fetched;
    UINT32 Mask = 0;

    if (FAILED(CoCreateInstance(&MpsClsidNetworkListManager, NULL, CLSCTX_INPROC_SERVER,
                                &MpsIidNetworkListManager, (void **)&List)))
        return MPS_PROFILE_PUBLIC;
    if (SUCCEEDED(INetworkListManager_GetNetworks(List, NLM_ENUM_NETWORK_CONNECTED, &Networks)) && Networks)
    {
        while (IEnumNetworks_Next(Networks, 1, &Network, &Fetched) == S_OK && Fetched)
        {
            NLM_NETWORK_CATEGORY Category = NLM_NETWORK_CATEGORY_PUBLIC;

            if (FAILED(INetwork_GetCategory(Network, &Category)))
                Category = NLM_NETWORK_CATEGORY_PUBLIC;
            if (Category == NLM_NETWORK_CATEGORY_DOMAIN_AUTHENTICATED)
                Mask |= MPS_PROFILE_DOMAIN;
            else if (Category == NLM_NETWORK_CATEGORY_PRIVATE)
                Mask |= MPS_PROFILE_PRIVATE;
            else
                Mask |= MPS_PROFILE_PUBLIC;
            INetwork_Release(Network);
        }
        IEnumNetworks_Release(Networks);
    }
    INetworkListManager_Release(List);

    if (Mask & MPS_PROFILE_PUBLIC)
        return MPS_PROFILE_PUBLIC;
    if (Mask & MPS_PROFILE_PRIVATE)
        return MPS_PROFILE_PRIVATE;
    if (Mask & MPS_PROFILE_DOMAIN)
        return MPS_PROFILE_DOMAIN;
    return MPS_PROFILE_PUBLIC;
}

static DWORD
MpsReadPolicyDword(
    _In_ HKEY PolicyKey,
    _In_ PCWSTR Profile,
    _In_ PCWSTR Name,
    _In_ DWORD Default)
{
    DWORD Value, Size = sizeof(Value);

    if (RegGetValueW(PolicyKey, Profile, Name, RRF_RT_REG_DWORD, NULL, &Value, &Size) != ERROR_SUCCESS)
        return Default;
    return Value;
}

/* {Domain,Standard,Public}Profile, as Windows keeps them. Nothing set means off, so a system
   without a firewall policy behaves as before */
static VOID
MpsReadProfileSettings(
    _In_opt_ HKEY PolicyKey,
    _In_ UINT32 ProfileBit,
    _Out_ MPS_PROFILE_SETTINGS *Settings)
{
    PCWSTR Profile = ProfileBit == MPS_PROFILE_DOMAIN ? L"DomainProfile" :
                     ProfileBit == MPS_PROFILE_PRIVATE ? L"StandardProfile" : L"PublicProfile";

    ZeroMemory(Settings, sizeof(*Settings));
    if (PolicyKey == NULL)
        return;
    Settings->Enabled = MpsReadPolicyDword(PolicyKey, Profile, L"EnableFirewall", 0) != 0;
    Settings->BlockInbound = MpsReadPolicyDword(PolicyKey, Profile, L"DefaultInboundAction", 1) != 0;
    Settings->NoExceptions = MpsReadPolicyDword(PolicyKey, Profile, L"DoNotAllowExceptions", 0) != 0;
}

static DWORD
MpsOpenAndLoadEngine(
    _In_opt_ HKEY PolicyKey,
    _Out_ HANDLE *Engine)
{
    HANDLE NewEngine = NULL;
    MPS_PROFILE_SETTINGS Settings;
    UINT32 Profile;
    DWORD Error;

    *Engine = NULL;

    Error = MpsOpenEngine(FWPM_SESSION_FLAG_DYNAMIC, &NewEngine);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = MpsAddLoopbackPermit(NewEngine, &FWPM_SUBLAYER_MPSSVC_WF);

    /* the rules and the default inbound action hold only while the active profile is on */
    Profile = MpsActiveProfile();
    MpsReadProfileSettings(PolicyKey, Profile, &Settings);
    if (Error == ERROR_SUCCESS && Settings.Enabled)
    {
        HKEY RulesKey;

        if (Settings.BlockInbound)
            Error = MpsAddDefaultInboundBlock(NewEngine, &FWPM_SUBLAYER_MPSSVC_WF);
        if (Error == ERROR_SUCCESS &&
            RegOpenKeyExW(PolicyKey, MPS_RULES_SUBKEY, 0, KEY_QUERY_VALUE, &RulesKey) == ERROR_SUCCESS)
        {
            Error = MpsLoadFirewallRulesFromKey(NewEngine, &FWPM_SUBLAYER_MPSSVC_WF, RulesKey, Profile,
                                                !Settings.NoExceptions);
            RegCloseKey(RulesKey);
        }
    }

    if (Error != ERROR_SUCCESS)
    {
        FwpmEngineClose0(NewEngine);
        return Error;
    }

    *Engine = NewEngine;
    return ERROR_SUCCESS;
}

/* An interface or an address came or went: the active profile may have changed */
static VOID NETIOAPI_API_
MpsInterfaceChanged(
    _In_ PVOID Context,
    _In_opt_ PMIB_IPINTERFACE_ROW Row,
    _In_ MIB_NOTIFICATION_TYPE Type)
{
    UNREFERENCED_PARAMETER(Row);
    UNREFERENCED_PARAMETER(Type);
    SetEvent((HANDLE)Context);
}

static VOID NETIOAPI_API_
MpsAddressChanged(
    _In_ PVOID Context,
    _In_opt_ PMIB_UNICASTIPADDRESS_ROW Row,
    _In_ MIB_NOTIFICATION_TYPE Type)
{
    UNREFERENCED_PARAMETER(Row);
    UNREFERENCED_PARAMETER(Type);
    SetEvent((HANDLE)Context);
}

static VOID WINAPI
MpsServiceMain(
    _In_ DWORD ArgumentCount,
    _In_reads_(ArgumentCount) LPWSTR *Arguments)
{
    HANDLE Engine = NULL;
    HANDLE ChangeEvent = NULL;
    HANDLE NetworkEvent = NULL;
    HANDLE InterfaceNotify = NULL, AddressNotify = NULL;
    HANDLE WaitHandles[3];
    HKEY PolicyKey = NULL;
    DWORD Error;
    DWORD WaitResult;
    BOOL StopRequested = FALSE;
    BOOL WatchPolicy;
    BOOL ComReady;

    UNREFERENCED_PARAMETER(ArgumentCount);
    UNREFERENCED_PARAMETER(Arguments);

    MpsStatusHandle = RegisterServiceCtrlHandlerExW(MPS_SERVICE_NAME,
                                                    MpsControlHandler,
                                                    NULL);
    if (MpsStatusHandle == NULL)
        return;

    MpsReportStatus(SERVICE_START_PENDING, NO_ERROR);
    MpsStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (MpsStopEvent == NULL)
    {
        MpsReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    ChangeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    NetworkEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (ChangeEvent == NULL || NetworkEvent == NULL)
    {
        Error = GetLastError();
        if (ChangeEvent != NULL)
            CloseHandle(ChangeEvent);
        if (NetworkEvent != NULL)
            CloseHandle(NetworkEvent);
        CloseHandle(MpsStopEvent);
        MpsStopEvent = NULL;
        MpsReportStatus(SERVICE_STOPPED, Error);
        return;
    }

    ComReady = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));

    /* the whole policy is watched (profiles and rules); an empty key changes nothing, it only
       lets a policy written later take effect without a restart */
    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                            MPS_POLICY_KEY,
                            0,
                            NULL,
                            0,
                            KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY,
                            NULL,
                            &PolicyKey,
                            NULL);
    if (Error != ERROR_SUCCESS)
    {
        PolicyKey = NULL;
        Error = ERROR_SUCCESS;
    }

    if (NotifyIpInterfaceChange(AF_UNSPEC, MpsInterfaceChanged, NetworkEvent, FALSE, &InterfaceNotify) != NO_ERROR)
        InterfaceNotify = NULL;
    if (NotifyUnicastIpAddressChange(AF_UNSPEC, MpsAddressChanged, NetworkEvent, FALSE, &AddressNotify) != NO_ERROR)
        AddressNotify = NULL;

    if (Error == ERROR_SUCCESS)
        Error = MpsAddSubLayer();

    if (Error == ERROR_SUCCESS)
        Error = MpsOpenAndLoadEngine(PolicyKey, &Engine);

    if (Error == ERROR_SUCCESS)
    {
        MpsReportStatus(SERVICE_RUNNING, NO_ERROR);

        WaitHandles[0] = MpsStopEvent;
        WaitHandles[1] = NetworkEvent;
        WaitHandles[2] = ChangeEvent;

        while (!StopRequested)
        {
            WatchPolicy = PolicyKey != NULL &&
                          RegNotifyChangeKeyValue(PolicyKey,
                                                  TRUE,
                                                  REG_NOTIFY_CHANGE_NAME |
                                                  REG_NOTIFY_CHANGE_LAST_SET,
                                                  ChangeEvent,
                                                  TRUE) == ERROR_SUCCESS;

            WaitResult = WaitForMultipleObjects(WatchPolicy ? 3 : 2,
                                                WaitHandles,
                                                FALSE,
                                                INFINITE);
            if (WaitResult == WAIT_OBJECT_0)
            {
                StopRequested = TRUE;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1 || WaitResult == WAIT_OBJECT_0 + 2)
            {
                HANDLE NewEngine = NULL;

                /* the Network List Manager sees a network change a moment after the stack */
                if (WaitResult == WAIT_OBJECT_0 + 1 &&
                    WaitForSingleObject(MpsStopEvent, MPS_NETWORK_SETTLE_MS) == WAIT_OBJECT_0)
                {
                    StopRequested = TRUE;
                    continue;
                }

                if (MpsOpenAndLoadEngine(PolicyKey, &NewEngine) == ERROR_SUCCESS)
                {
                    if (Engine != NULL)
                        FwpmEngineClose0(Engine);
                    Engine = NewEngine;
                }
            }
            else
            {
                Error = WaitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
                break;
            }
        }
    }

    if (InterfaceNotify != NULL)
        CancelMibChangeNotify2(InterfaceNotify);
    if (AddressNotify != NULL)
        CancelMibChangeNotify2(AddressNotify);

    if (PolicyKey != NULL)
        RegCloseKey(PolicyKey);

    if (Engine != NULL)
    {
        DWORD CloseError = FwpmEngineClose0(Engine);
        if (Error == ERROR_SUCCESS)
            Error = CloseError;
    }

    if (ComReady)
        CoUninitialize();
    CloseHandle(NetworkEvent);
    CloseHandle(ChangeEvent);
    CloseHandle(MpsStopEvent);
    MpsStopEvent = NULL;
    MpsReportStatus(SERVICE_STOPPED, Error);
}

int
wmain(
    int ArgumentCount,
    wchar_t *Arguments[])
{
    SERVICE_TABLE_ENTRYW DispatchTable[] =
    {
        { (LPWSTR)MPS_SERVICE_NAME, MpsServiceMain },
        { NULL, NULL }
    };

    UNREFERENCED_PARAMETER(ArgumentCount);
    UNREFERENCED_PARAMETER(Arguments);

    if (!StartServiceCtrlDispatcherW(DispatchTable))
        return (int)GetLastError();

    return 0;
}
