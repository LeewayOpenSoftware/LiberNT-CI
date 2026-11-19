/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests a local RPC client naming the server it expects (RPC_SECURITY_QOS_V3)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <rpc.h>
#include <rpcdcep.h>

#define SERVER_ENDPOINT L"LiberNTServerIdentity"

static void __RPC_STUB ServerEcho(PRPC_MESSAGE Message)
{
    Message->BufferLength = sizeof(DWORD);
    if (I_RpcGetBuffer(Message) == RPC_S_OK)
        *(DWORD *)Message->Buffer = 0x1234;
}

static RPC_DISPATCH_FUNCTION ServerFunctions[] = { ServerEcho };
static RPC_DISPATCH_TABLE ServerDispatch = { 1, ServerFunctions, 0 };
#define TEST_INTERFACE {{0x4d1c2b77, 0x0a6e, 0x4c3f, {0x91, 0x2a, 0x55, 0x10, 0xee, 0x7b, 0x3c, 0x0a}}, {1, 0}}
#define NDR_SYNTAX {{0x8a885d04, 0x1ceb, 0x11c9, {0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60}}, {2, 0}}
static RPC_SERVER_INTERFACE ServerInterface =
    { sizeof(RPC_SERVER_INTERFACE), TEST_INTERFACE, NDR_SYNTAX, &ServerDispatch, 0, NULL, NULL, NULL, 0 };
static RPC_CLIENT_INTERFACE ClientInterface =
    { sizeof(RPC_CLIENT_INTERFACE), TEST_INTERFACE, NDR_SYNTAX, NULL, 0, NULL, 0, NULL, 0 };

static RPC_STATUS CallServer(const WCHAR *Principal, PSID Sid, ULONG Capabilities, RPC_STATUS *SetStatus)
{
    RPC_BINDING_HANDLE Binding;
    RPC_WSTR StringBinding;
    RPC_SECURITY_QOS_V3_W Qos;
    RPC_MESSAGE Message;
    RPC_STATUS Status;

    Status = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)SERVER_ENDPOINT, NULL, &StringBinding);
    if (Status != RPC_S_OK)
        return Status;
    Status = RpcBindingFromStringBindingW(StringBinding, &Binding);
    RpcStringFreeW(&StringBinding);
    if (Status != RPC_S_OK)
        return Status;

    ZeroMemory(&Qos, sizeof(Qos));
    Qos.Version = RPC_C_SECURITY_QOS_VERSION_3;
    Qos.Capabilities = Capabilities;
    Qos.IdentityTracking = RPC_C_QOS_IDENTITY_STATIC;
    Qos.ImpersonationType = RPC_C_IMP_LEVEL_IDENTIFY;
    Qos.Sid = Sid;
    *SetStatus = RpcBindingSetAuthInfoExW(Binding, (RPC_WSTR)Principal, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                          RPC_C_AUTHN_WINNT, NULL, 0, (RPC_SECURITY_QOS *)&Qos);

    ZeroMemory(&Message, sizeof(Message));
    Message.Handle = Binding;
    Message.DataRepresentation = NDR_LOCAL_DATA_REPRESENTATION;
    Message.RpcInterfaceInformation = &ClientInterface;
    Message.BufferLength = sizeof(DWORD);
    Status = I_RpcGetBuffer(&Message);
    if (Status == RPC_S_OK)
    {
        *(DWORD *)Message.Buffer = 0;
        Status = I_RpcSendReceive(&Message);
        if (Status == RPC_S_OK)
        {
            ok(Message.BufferLength >= sizeof(DWORD) && *(DWORD *)Message.Buffer == 0x1234, "Unexpected reply\n");
            I_RpcFreeBuffer(&Message);
        }
    }
    RpcBindingFree(&Binding);
    return Status;
}

START_TEST(ServerIdentity)
{
    BYTE UserBuffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    TOKEN_USER *User = (TOKEN_USER *)UserBuffer;
    BYTE BadSid[16] = { 9, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0 };
    WCHAR Name[256], Domain[256], Full[520], Lower[520];
    DWORD Size, NameSize = 256, DomainSize = 256;
    SID_NAME_USE Use;
    RPC_STATUS Status, SetStatus;
    HANDLE Token;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token) ||
        !GetTokenInformation(Token, TokenUser, User, sizeof(UserBuffer), &Size))
    {
        skip("Cannot read the process user (%lu)\n", GetLastError());
        return;
    }
    CloseHandle(Token);
    if (!LookupAccountSidW(NULL, User->User.Sid, Name, &NameSize, Domain, &DomainSize, &Use))
    {
        skip("Cannot name the process user (%lu)\n", GetLastError());
        return;
    }
    _snwprintf(Full, 520, L"%s\\%s", Domain, Name);
    wcscpy(Lower, Full);
    _wcslwr(Lower);

    Status = RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)SERVER_ENDPOINT, NULL);
    ok(Status == RPC_S_OK, "RpcServerUseProtseqEpW returned %ld\n", Status);
    Status = RpcServerRegisterIf(&ServerInterface, NULL, NULL);
    ok(Status == RPC_S_OK, "RpcServerRegisterIf returned %ld\n", Status);
    Status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
    ok(Status == RPC_S_OK, "RpcServerListen returned %ld\n", Status);

    /* Version 3, nothing named */
    Status = CallServer(NULL, NULL, RPC_C_QOS_CAPABILITIES_DEFAULT, &SetStatus);
    ok(SetStatus == RPC_S_OK, "SetAuthInfoEx returned %ld\n", SetStatus);
    ok(Status == RPC_S_OK, "Call returned %ld\n", Status);

    /* The Sid of the user the server runs as */
    Status = CallServer(NULL, User->User.Sid, RPC_C_QOS_CAPABILITIES_DEFAULT, &SetStatus);
    ok(SetStatus == RPC_S_OK, "SetAuthInfoEx returned %ld\n", SetStatus);
    ok(Status == RPC_S_OK, "Call returned %ld\n", Status);

    /* The server principal with mutual authentication: DOMAIN\user, a bare user name, any case */
    Status = CallServer(Full, NULL, RPC_C_QOS_CAPABILITIES_MUTUAL_AUTH, &SetStatus);
    ok(SetStatus == RPC_S_OK, "SetAuthInfoEx returned %ld\n", SetStatus);
    ok(Status == RPC_S_OK, "Call returned %ld\n", Status);
    Status = CallServer(Name, NULL, RPC_C_QOS_CAPABILITIES_MUTUAL_AUTH, &SetStatus);
    ok(Status == RPC_S_OK, "Call returned %ld\n", Status);
    Status = CallServer(Lower, NULL, RPC_C_QOS_CAPABILITIES_MUTUAL_AUTH, &SetStatus);
    ok(Status == RPC_S_OK, "Call returned %ld\n", Status);

    /* Both */
    Status = CallServer(Full, User->User.Sid, RPC_C_QOS_CAPABILITIES_MUTUAL_AUTH, &SetStatus);
    ok(Status == RPC_S_OK, "Call returned %ld\n", Status);

    /* An invalid Sid is refused when it is set; the binding stays usable */
    Status = CallServer(NULL, (PSID)BadSid, RPC_C_QOS_CAPABILITIES_DEFAULT, &SetStatus);
    ok(SetStatus == RPC_S_INVALID_ARG, "SetAuthInfoEx returned %ld\n", SetStatus);
    ok(Status == RPC_S_OK, "Call returned %ld\n", Status);

    RpcMgmtStopServerListening(NULL);
    RpcMgmtWaitServerListen();
    RpcServerUnregisterIf(&ServerInterface, NULL, FALSE);
}
