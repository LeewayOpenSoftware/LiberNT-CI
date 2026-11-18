/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests authenticated local RPC: what the server learns and which caller identity it gets
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <rpc.h>
#include <rpcdcep.h>

#define LOCAL_ENDPOINT L"LiberNTLocalAuth"
#define NOT_READ 0xdeadbeef

typedef struct _CALLER_REPORT
{
    ULONG AuthStatus;
    ULONG AuthnLevel;
    ULONG AuthnSvc;
    ULONG ImpersonateStatus;
    ULONG TokenError;
    ULONG ImpersonationLevel;
    ULONG RevertStatus;
    BOOL Network;
    BOOL Anonymous;
    ULONG SidLength;
    BYTE Sid[SECURITY_MAX_SID_SIZE];
} CALLER_REPORT;

static void ReadGroups(HANDLE Token, CALLER_REPORT *Report)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID NetworkSid = NULL, AnonymousSid = NULL;
    TOKEN_GROUPS *Groups;
    DWORD Size = 0, i;

    GetTokenInformation(Token, TokenGroups, NULL, 0, &Size);
    Groups = Size ? HeapAlloc(GetProcessHeap(), 0, Size) : NULL;
    if (!Groups)
        return;
    if (GetTokenInformation(Token, TokenGroups, Groups, Size, &Size) &&
        AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_NETWORK_RID, 0, 0, 0, 0, 0, 0, 0, &NetworkSid) &&
        AllocateAndInitializeSid(&NtAuthority, 1, SECURITY_ANONYMOUS_LOGON_RID, 0, 0, 0, 0, 0, 0, 0, &AnonymousSid))
    {
        for (i = 0; i < Groups->GroupCount; i++)
        {
            if (EqualSid(Groups->Groups[i].Sid, NetworkSid))
                Report->Network = TRUE;
            if (EqualSid(Groups->Groups[i].Sid, AnonymousSid))
                Report->Anonymous = TRUE;
        }
    }
    if (AnonymousSid) FreeSid(AnonymousSid);
    if (NetworkSid) FreeSid(NetworkSid);
    HeapFree(GetProcessHeap(), 0, Groups);
}

static void __RPC_STUB ServerReportCaller(PRPC_MESSAGE Message)
{
    BYTE UserBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *User = (TOKEN_USER *)UserBuffer;
    RPC_BINDING_HANDLE Client = Message->Handle;
    SECURITY_IMPERSONATION_LEVEL Level;
    CALLER_REPORT Report;
    HANDLE Token;
    DWORD Size;

    memset(&Report, 0, sizeof(Report));
    Report.AuthnLevel = NOT_READ;
    Report.AuthnSvc = NOT_READ;
    Report.TokenError = NOT_READ;
    Report.ImpersonationLevel = NOT_READ;
    Report.RevertStatus = NOT_READ;

    Report.AuthStatus = RpcBindingInqAuthClientW(Client, NULL, NULL, &Report.AuthnLevel, &Report.AuthnSvc, NULL);
    Report.ImpersonateStatus = RpcImpersonateClient(Client);
    if (Report.ImpersonateStatus == RPC_S_OK)
    {
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &Token))
        {
            Report.TokenError = ERROR_SUCCESS;
            if (GetTokenInformation(Token, TokenUser, User, sizeof(UserBuffer), &Size))
            {
                Report.SidLength = GetLengthSid(User->User.Sid);
                memcpy(Report.Sid, User->User.Sid, Report.SidLength);
            }
            if (GetTokenInformation(Token, TokenImpersonationLevel, &Level, sizeof(Level), &Size))
                Report.ImpersonationLevel = Level;
            ReadGroups(Token, &Report);
            CloseHandle(Token);
        }
        else
        {
            Report.TokenError = GetLastError();
        }
        Report.RevertStatus = RpcRevertToSelfEx(Client);
    }

    Message->BufferLength = sizeof(Report);
    if (I_RpcGetBuffer(Message) == RPC_S_OK)
        memcpy(Message->Buffer, &Report, sizeof(Report));
}

static RPC_DISPATCH_FUNCTION ServerFunctions[] = { ServerReportCaller };
static RPC_DISPATCH_TABLE ServerDispatchTable = { ARRAYSIZE(ServerFunctions), ServerFunctions, 0 };

#define INTERFACE_ID {{0x5e0f7c3a, 0x91d2, 0x4b6e, {0xa8, 0x14, 0x3c, 0x77, 0x02, 0xd9, 0x6b, 0x51}}, {1, 0}}
#define NDR_SYNTAX {{0x8a885d04, 0x1ceb, 0x11c9, {0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60}}, {2, 0}}

static RPC_SERVER_INTERFACE ServerInterface =
{
    sizeof(RPC_SERVER_INTERFACE), INTERFACE_ID, NDR_SYNTAX, &ServerDispatchTable, 0, NULL, NULL, NULL, 0
};

static RPC_CLIENT_INTERFACE ClientInterface =
{
    sizeof(RPC_CLIENT_INTERFACE), INTERFACE_ID, NDR_SYNTAX, NULL, 0, NULL, 0, NULL, 0
};

/* The binding the system's DPAPI and credential clients use: sealed WinNT, identification-level QOS */
static RPC_STATUS OpenBinding(RPC_BINDING_HANDLE *Binding, RPC_STATUS *AuthSetStatus)
{
    RPC_WSTR StringBinding = NULL;
    RPC_SECURITY_QOS Qos;
    RPC_STATUS Status;

    *Binding = NULL;
    *AuthSetStatus = NOT_READ;
    Status = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)LOCAL_ENDPOINT, NULL, &StringBinding);
    if (Status != RPC_S_OK)
        return Status;
    Status = RpcBindingFromStringBindingW(StringBinding, Binding);
    RpcStringFreeW(&StringBinding);
    if (Status != RPC_S_OK)
        return Status;

    Qos.Version = RPC_C_SECURITY_QOS_VERSION;
    Qos.Capabilities = RPC_C_QOS_CAPABILITIES_DEFAULT;
    Qos.IdentityTracking = RPC_C_QOS_IDENTITY_STATIC;
    Qos.ImpersonationType = RPC_C_IMP_LEVEL_IDENTIFY;
    *AuthSetStatus = RpcBindingSetAuthInfoExW(*Binding, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_AUTHN_WINNT, NULL, 0, &Qos);
    return RPC_S_OK;
}

static RPC_STATUS AskCaller(RPC_BINDING_HANDLE Binding, CALLER_REPORT *Report)
{
    RPC_MESSAGE Message;
    RPC_STATUS Status;

    memset(Report, 0, sizeof(*Report));
    memset(&Message, 0, sizeof(Message));
    Message.Handle = Binding;
    Message.DataRepresentation = NDR_LOCAL_DATA_REPRESENTATION;
    Message.RpcInterfaceInformation = &ClientInterface;
    Message.ProcNum = 0;
    Message.BufferLength = sizeof(DWORD);
    Status = I_RpcGetBuffer(&Message);
    if (Status == RPC_S_OK)
    {
        *(DWORD *)Message.Buffer = 0;
        Status = I_RpcSendReceive(&Message);
        if (Status == RPC_S_OK)
        {
            if (Message.BufferLength >= sizeof(*Report))
                memcpy(Report, Message.Buffer, sizeof(*Report));
            else
                Status = RPC_S_INVALID_BOUND;
        }
        I_RpcFreeBuffer(&Message);
    }
    return Status;
}

static BOOL GetOwnSid(BYTE *Sid, DWORD *SidLength)
{
    BYTE UserBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *User = (TOKEN_USER *)UserBuffer;
    BOOL Done = FALSE;
    HANDLE Token;
    DWORD Size;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return FALSE;
    if (GetTokenInformation(Token, TokenUser, User, sizeof(UserBuffer), &Size))
    {
        *SidLength = GetLengthSid(User->User.Sid);
        memcpy(Sid, User->User.Sid, *SidLength);
        Done = TRUE;
    }
    CloseHandle(Token);
    return Done;
}

static void CheckReport(const char *Stage, RPC_STATUS Status, RPC_STATUS AuthSetStatus, const CALLER_REPORT *Report,
                        const BYTE *OwnSid, DWORD OwnSidLength)
{
    ok(AuthSetStatus == RPC_S_OK, "%s: RpcBindingSetAuthInfoExW returned %ld\n", Stage, AuthSetStatus);
    ok(Status == RPC_S_OK, "%s: the call failed with %ld\n", Stage, Status);
    if (Status != RPC_S_OK)
        return;
    ok(Report->AuthStatus == RPC_S_OK, "%s: RpcBindingInqAuthClientW returned %lu\n", Stage, Report->AuthStatus);
    ok(Report->AuthnLevel == RPC_C_AUTHN_LEVEL_PKT_PRIVACY, "%s: authentication level %lu\n", Stage, Report->AuthnLevel);
    ok(Report->AuthnSvc == RPC_C_AUTHN_WINNT, "%s: authentication service %lu\n", Stage, Report->AuthnSvc);
    ok(Report->ImpersonateStatus == RPC_S_OK, "%s: RpcImpersonateClient returned %lu\n", Stage, Report->ImpersonateStatus);
    ok(Report->TokenError == ERROR_SUCCESS, "%s: no thread token, error %lu\n", Stage, Report->TokenError);
    ok(Report->ImpersonationLevel == SecurityIdentification, "%s: impersonation level %lu\n", Stage,
       Report->ImpersonationLevel);
    ok(Report->SidLength == OwnSidLength && !memcmp(Report->Sid, OwnSid, OwnSidLength),
       "%s: the server did not see this user\n", Stage);
    ok(!Report->Network, "%s: the caller token has the NETWORK group\n", Stage);
    ok(!Report->Anonymous, "%s: the caller token has the ANONYMOUS LOGON group\n", Stage);
    ok(Report->RevertStatus == RPC_S_OK, "%s: RpcRevertToSelfEx returned %lu\n", Stage, Report->RevertStatus);
}

static void TestLocalAuth(const BYTE *OwnSid, DWORD OwnSidLength)
{
    RPC_BINDING_HANDLE Binding;
    RPC_STATUS Status, AuthSetStatus;
    CALLER_REPORT Report;
    int i;

    for (i = 0; i < 2; i++)
    {
        Status = OpenBinding(&Binding, &AuthSetStatus);
        ok(Status == RPC_S_OK, "Binding failed with %ld\n", Status);
        if (Status != RPC_S_OK)
            return;
        Status = AskCaller(Binding, &Report);
        CheckReport(i ? "second binding" : "first binding", Status, AuthSetStatus, &Report, OwnSid, OwnSidLength);
        RpcBindingFree(&Binding);
    }
}

START_TEST(LocalAuth)
{
    BYTE OwnSid[SECURITY_MAX_SID_SIZE];
    DWORD OwnSidLength = 0;
    RPC_STATUS Status;

    if (!GetOwnSid(OwnSid, &OwnSidLength))
    {
        skip("The user of this process cannot be read, error %lu\n", GetLastError());
        return;
    }

    /* The server registers no authentication service: local RPC authenticates through the transport */
    Status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)LOCAL_ENDPOINT, NULL);
    ok(Status == RPC_S_OK, "Listening on local RPC failed with %ld\n", Status);
    if (Status != RPC_S_OK)
        return;
    Status = RpcServerRegisterIf(&ServerInterface, NULL, NULL);
    ok(Status == RPC_S_OK, "RpcServerRegisterIf failed with %ld\n", Status);
    Status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    ok(Status == RPC_S_OK, "RpcServerListen failed with %ld\n", Status);
    if (Status != RPC_S_OK)
        return;

    TestLocalAuth(OwnSid, OwnSidLength);

    RpcMgmtStopServerListening(NULL);
    RpcMgmtWaitServerListen();
    RpcServerUnregisterIf(&ServerInterface, NULL, TRUE);
}
