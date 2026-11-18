/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMB2 client engine over TCP/445 (MS-SMB2), NTLM authentication through SSPI
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Strictly request/response: one outstanding request per connection, so credit tracking reduces
 * to asking for more credits than a request consumes.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SECURITY_WIN32
#include <sspi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smb2client.h"
#include "smb2enc.h"

#define SMB2_MAX_MESSAGE    (SMB2_MAX_IO + 4096)

typedef struct _SMB2_PENDING
{
    struct _SMB2_PENDING *Next;
    uint64_t MessageId;
    uint32_t TreeId;
    HANDLE Event;
    uint8_t *Resp;
    size_t RespLen;
    int Done;
    int Async;
    uint64_t AsyncId;
} SMB2_PENDING;

struct _SMB2_CONN
{
    SOCKET Sock;
    CRITICAL_SECTION Lock;          /* send order, message ids and the pending list */
    SMB2_PENDING *Pending;
    HANDLE Reader;
    volatile LONG Mux;              /* responses are matched by a reader thread (after session setup) */
    volatile LONG Dead;
    uint16_t Dialect;
    uint32_t SecurityMode;
    uint64_t SessionId;
    uint64_t NextMessageId;
    uint8_t SigningKey[16];
    int Signing;                /* sign requests */
    uint32_t MaxRead;
    uint32_t MaxTransact;
    uint32_t SessionFlags;
    CredHandle Cred;
    CtxtHandle Ctx;
    int HaveCred;
    int HaveCtx;
};

uint16_t Smb2ConnDialect(const SMB2_CONN *Conn)
{
    return Conn->Dialect;
}

int Smb2ConnSigned(const SMB2_CONN *Conn)
{
    return Conn->Signing;
}

/* ---------------- transport ---------------- */

static int SendAll(SOCKET s, const uint8_t *p, size_t n)
{
    while (n)
    {
        int r = send(s, (const char *)p, (int)n, 0);

        if (r <= 0)
            return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int RecvAll(SOCKET s, uint8_t *p, size_t n)
{
    while (n)
    {
        int r = recv(s, (char *)p, (int)n, 0);

        if (r <= 0)
            return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

/* Direct TCP framing (2.1): one zero byte then a 24-bit big-endian length. */
static int SendFrame(SMB2_CONN *c, const uint8_t *msg, size_t len)
{
    uint8_t hdr[4];

    hdr[0] = 0;
    hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)len;
    return SendAll(c->Sock, hdr, 4) && SendAll(c->Sock, msg, len);
}

static uint8_t *RecvFrame(SMB2_CONN *c, size_t *len)
{
    uint8_t hdr[4], *buf;
    size_t n;

    if (!RecvAll(c->Sock, hdr, 4))
        return NULL;
    n = ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | hdr[3];
    if (hdr[0] != 0 || n < SMB2_HEADER_SIZE || n > SMB2_MAX_MESSAGE)
        return NULL;
    buf = (uint8_t *)malloc(n);
    if (!buf)
        return NULL;
    if (!RecvAll(c->Sock, buf, n))
    {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

/*
 * Sends one request (header + body) and returns the matching response.
 * Resp is malloc'ed (caller frees); the status is the NTSTATUS in the response header.
 * Interim STATUS_PENDING responses are skipped.
 */
/* With signing in force every response must carry a valid signature: an unsigned one is a stripped one */
static int ResponseSignatureOk(SMB2_CONN *c, uint16_t Command, const uint8_t *r, size_t rlen)
{
    if (!c->Signing || Command == SMB2_SESSION_SETUP || Command == SMB2_NEGOTIATE)
        return 1;
    if (!(Smb2Get32(r, SMB2H_FLAGS) & SMB2_FLAGS_SIGNED))
        return 0;
    return Smb2VerifyMessage(c->SigningKey, r, rlen);
}

static uint32_t TransactSerial(SMB2_CONN *c, uint16_t Command, uint32_t TreeId, const uint8_t *Body, size_t BodyLen,
                               uint8_t **Resp, size_t *RespLen)
{
    uint8_t *req;
    size_t total = SMB2_HEADER_SIZE + BodyLen, rlen;
    uint64_t mid = c->NextMessageId++;
    uint8_t *r;
    uint32_t status;

    *Resp = NULL;
    req = (uint8_t *)calloc(1, total);
    if (!req)
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    req[0] = 0xFE; req[1] = 'S'; req[2] = 'M'; req[3] = 'B';
    Smb2Put16(req, SMB2H_STRUCTSIZE, SMB2_HEADER_SIZE);
    Smb2Put16(req, SMB2H_CREDITCHARGE, c->Dialect >= SMB2_DIALECT_210 ? 1 : 0);
    Smb2Put16(req, SMB2H_COMMAND, Command);
    Smb2Put16(req, SMB2H_CREDITS, 16);
    Smb2Put64(req, SMB2H_MESSAGEID, mid);
    Smb2Put32(req, SMB2H_TREEID, TreeId);
    Smb2Put64(req, SMB2H_SESSIONID, c->SessionId);
    if (BodyLen)
        memcpy(req + SMB2_HEADER_SIZE, Body, BodyLen);
    if (c->Signing && Command != SMB2_NEGOTIATE && c->SessionId != 0)
        Smb2SignMessage(c->SigningKey, req, total);

    if (!SendFrame(c, req, total))
    {
        free(req);
        return SMB2_STATUS_CONNECTION_DISCONNECTED;
    }
    free(req);

    for (;;)
    {
        r = RecvFrame(c, &rlen);
        if (!r)
            return SMB2_STATUS_CONNECTION_DISCONNECTED;
        if (Smb2Get32(r, 0) != 0x424D53FEu || Smb2Get64(r, SMB2H_MESSAGEID) != mid ||
            !(Smb2Get32(r, SMB2H_FLAGS) & SMB2_FLAGS_SERVER_TO_REDIR))
        {
            free(r);
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        }
        status = Smb2Get32(r, SMB2H_STATUS);
        if (status == SMB2_STATUS_PENDING)
        {
            free(r);
            continue;
        }
        break;
    }

    if (!ResponseSignatureOk(c, Command, r, rlen))
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    *Resp = r;
    *RespLen = rlen;
    return status;
}

static uint32_t FinishResponse(SMB2_CONN *c, uint16_t Command, uint8_t *r, size_t rlen, uint8_t **Resp,
                               size_t *RespLen)
{
    uint32_t status = Smb2Get32(r, SMB2H_STATUS);

    if (!ResponseSignatureOk(c, Command, r, rlen))
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    *Resp = r;
    *RespLen = rlen;
    return status;
}

static void BuildHeader(SMB2_CONN *c, uint8_t *req, uint16_t Command, uint32_t TreeId, uint64_t mid)
{
    req[0] = 0xFE; req[1] = 'S'; req[2] = 'M'; req[3] = 'B';
    Smb2Put16(req, SMB2H_STRUCTSIZE, SMB2_HEADER_SIZE);
    Smb2Put16(req, SMB2H_CREDITCHARGE, c->Dialect >= SMB2_DIALECT_210 ? 1 : 0);
    Smb2Put16(req, SMB2H_COMMAND, Command);
    Smb2Put16(req, SMB2H_CREDITS, 16);
    Smb2Put64(req, SMB2H_MESSAGEID, mid);
    Smb2Put32(req, SMB2H_TREEID, TreeId);
    Smb2Put64(req, SMB2H_SESSIONID, c->SessionId);
}

/* Demultiplexes the responses of a session: one reader, any number of requests in flight (2.2.1.2 AsyncId) */
static DWORD WINAPI ReaderProc(LPVOID Param)
{
    SMB2_CONN *c = (SMB2_CONN *)Param;
    SMB2_PENDING *p;
    uint8_t *r;
    size_t rlen;

    for (;;)
    {
        uint64_t mid;
        uint32_t status, flags;

        r = RecvFrame(c, &rlen);
        if (!r)
            break;
        mid = Smb2Get64(r, SMB2H_MESSAGEID);
        status = Smb2Get32(r, SMB2H_STATUS);
        flags = Smb2Get32(r, SMB2H_FLAGS);
        if (Smb2Get32(r, 0) != 0x424D53FEu || !(flags & SMB2_FLAGS_SERVER_TO_REDIR))
        {
            free(r);
            continue;
        }
        EnterCriticalSection(&c->Lock);
        for (p = c->Pending; p && p->MessageId != mid; p = p->Next)
            ;
        if (!p || p->Done)
        {
            free(r);
        }
        else if (status == SMB2_STATUS_PENDING)
        {
            if (flags & SMB2_FLAGS_ASYNC_COMMAND)
            {
                p->AsyncId = Smb2Get64(r, SMB2H_ASYNCID);
                p->Async = 1;
            }
            free(r);
        }
        else
        {
            p->Resp = r;
            p->RespLen = rlen;
            p->Done = 1;
            SetEvent(p->Event);
        }
        LeaveCriticalSection(&c->Lock);
    }
    EnterCriticalSection(&c->Lock);
    InterlockedExchange(&c->Dead, 1);
    for (p = c->Pending; p; p = p->Next)
    {
        if (!p->Done)
        {
            p->Done = 1;
            SetEvent(p->Event);
        }
    }
    LeaveCriticalSection(&c->Lock);
    return 0;
}

static uint32_t TransactEx(SMB2_CONN *c, uint16_t Command, uint32_t TreeId, const uint8_t *Body, size_t BodyLen,
                           uint8_t **Resp, size_t *RespLen, SMB2_OP *Op)
{
    SMB2_PENDING *p, **link;
    uint8_t *req, *r;
    size_t total = SMB2_HEADER_SIZE + BodyLen, rlen;
    uint64_t mid;
    int sent;

    if (!c->Mux)
        return TransactSerial(c, Command, TreeId, Body, BodyLen, Resp, RespLen);
    *Resp = NULL;
    req = (uint8_t *)calloc(1, total);
    p = (SMB2_PENDING *)calloc(1, sizeof(*p));
    if (!req || !p)
    {
        free(req);
        free(p);
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    }
    p->Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!p->Event)
    {
        free(req);
        free(p);
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    }
    p->TreeId = TreeId;
    if (BodyLen)
        memcpy(req + SMB2_HEADER_SIZE, Body, BodyLen);

    EnterCriticalSection(&c->Lock);
    if (c->Dead)
    {
        LeaveCriticalSection(&c->Lock);
        free(req);
        CloseHandle(p->Event);
        free(p);
        return SMB2_STATUS_CONNECTION_DISCONNECTED;
    }
    mid = c->NextMessageId++;
    p->MessageId = mid;
    BuildHeader(c, req, Command, TreeId, mid);
    if (c->Signing && c->SessionId != 0)
        Smb2SignMessage(c->SigningKey, req, total);
    p->Next = c->Pending;
    c->Pending = p;
    if (Op)
        InterlockedExchange64((volatile LONG64 *)&Op->MessageId, (LONG64)mid);
    sent = SendFrame(c, req, total);
    LeaveCriticalSection(&c->Lock);
    free(req);

    if (!sent)
    {
        InterlockedExchange(&c->Dead, 1);
        shutdown(c->Sock, SD_BOTH);
    }
    else if (Op && Op->Cancelled)
    {
        Smb2OpCancel(Op);
    }
    WaitForSingleObject(p->Event, INFINITE);

    EnterCriticalSection(&c->Lock);
    for (link = &c->Pending; *link && *link != p; link = &(*link)->Next)
        ;
    if (*link)
        *link = p->Next;
    LeaveCriticalSection(&c->Lock);
    CloseHandle(p->Event);
    r = p->Resp;
    rlen = p->RespLen;
    free(p);
    if (!r)
        return SMB2_STATUS_CONNECTION_DISCONNECTED;
    return FinishResponse(c, Command, r, rlen, Resp, RespLen);
}

static uint32_t Transact(SMB2_CONN *c, uint16_t Command, uint32_t TreeId, const uint8_t *Body, size_t BodyLen,
                         uint8_t **Resp, size_t *RespLen)
{
    return TransactEx(c, Command, TreeId, Body, BodyLen, Resp, RespLen, NULL);
}

/* 2.2.30 CANCEL: no response; by AsyncId once the server answered "pending", else by MessageId */
static void SendCancel(SMB2_CONN *c, uint64_t mid)
{
    uint8_t msg[SMB2_HEADER_SIZE + 4];
    SMB2_PENDING *p;

    EnterCriticalSection(&c->Lock);
    for (p = c->Pending; p && p->MessageId != mid; p = p->Next)
        ;
    if (p && !p->Done)
    {
        memset(msg, 0, sizeof(msg));
        BuildHeader(c, msg, SMB2_CANCEL, p->TreeId, mid);
        Smb2Put16(msg, SMB2H_CREDITCHARGE, 0);
        Smb2Put16(msg, SMB2H_CREDITS, 1);
        Smb2Put16(msg, SMB2_HEADER_SIZE, 4);
        if (p->Async)
        {
            Smb2Put32(msg, SMB2H_FLAGS, SMB2_FLAGS_ASYNC_COMMAND);
            Smb2Put32(msg, SMB2H_TREEID, 0);
            Smb2Put64(msg, SMB2H_ASYNCID, p->AsyncId);
        }
        if (c->Signing && c->SessionId != 0)
            Smb2SignMessage(c->SigningKey, msg, sizeof(msg));
        SendFrame(c, msg, sizeof(msg));
    }
    LeaveCriticalSection(&c->Lock);
}

void Smb2OpInit(SMB2_OP *Op, SMB2_CONN *Conn)
{
    Op->Conn = Conn;
    Op->Cancelled = 0;
    Op->MessageId = 0;
}

void Smb2OpCancel(SMB2_OP *Op)
{
    LONG64 mid;

    InterlockedExchange(&Op->Cancelled, 1);
    mid = InterlockedCompareExchange64((volatile LONG64 *)&Op->MessageId, 0, 0);
    if (mid && Op->Conn)
        SendCancel(Op->Conn, (uint64_t)mid);
}

/* ---------------- connection ---------------- */

static int g_WsaUp;

uint32_t Smb2Connect(const WCHAR *Host, unsigned short Port, SMB2_CONN **Conn)
{
    ADDRINFOW hints, *res = NULL, *ai;
    WCHAR portstr[8];
    SOCKET s = INVALID_SOCKET;
    SMB2_CONN *c;
    WSADATA wsa;

    if (!g_WsaUp)
    {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return SMB2_STATUS_UNSUCCESSFUL;
        g_WsaUp = 1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    _snwprintf(portstr, 8, L"%u", Port ? Port : 445);
    if (GetAddrInfoW(Host, portstr, &hints, &res) != 0)
        return SMB2_STATUS_BAD_NETWORK_NAME;
    for (ai = res; ai; ai = ai->ai_next)
    {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    FreeAddrInfoW(res);
    if (s == INVALID_SOCKET)
        return SMB2_STATUS_CONNECTION_DISCONNECTED;

    c = (SMB2_CONN *)calloc(1, sizeof(*c));
    if (!c)
    {
        closesocket(s);
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    }
    c->Sock = s;
    InitializeCriticalSection(&c->Lock);
    *Conn = c;
    return SMB2_STATUS_SUCCESS;
}

void Smb2Disconnect(SMB2_CONN *c)
{
    if (!c)
        return;
    if (c->Reader)
    {
        shutdown(c->Sock, SD_BOTH);
        WaitForSingleObject(c->Reader, 5000);
        CloseHandle(c->Reader);
    }
    if (c->HaveCtx)
        DeleteSecurityContext(&c->Ctx);
    if (c->HaveCred)
        FreeCredentialsHandle(&c->Cred);
    if (c->Sock != INVALID_SOCKET)
        closesocket(c->Sock);
    DeleteCriticalSection(&c->Lock);
    memset(c->SigningKey, 0, sizeof(c->SigningKey));
    free(c);
}

/* 2.2.3 NEGOTIATE request, 2.2.4 response */
uint32_t Smb2Negotiate(SMB2_CONN *c)
{
    uint8_t body[36 + 4], *r;
    size_t rlen;
    uint32_t st;
    uint16_t dialect;

    memset(body, 0, sizeof(body));
    Smb2Put16(body, 0, 36);                         /* StructureSize */
    Smb2Put16(body, 2, 2);                          /* DialectCount */
    Smb2Put16(body, 4, SMB2_NEGOTIATE_SIGNING_ENABLED);
    /* Capabilities = 0, ClientGuid: random per connection */
    {
        unsigned i;
        LARGE_INTEGER t;

        QueryPerformanceCounter(&t);
        srand((unsigned)(t.LowPart ^ GetCurrentProcessId()));
        for (i = 0; i < 16; i++)
            body[12 + i] = (uint8_t)rand();
    }
    Smb2Put16(body, 36, SMB2_DIALECT_202);
    Smb2Put16(body, 38, SMB2_DIALECT_210);

    st = Transact(c, SMB2_NEGOTIATE, 0, body, sizeof(body), &r, &rlen);
    if (st != SMB2_STATUS_SUCCESS)
        return st;
    if (rlen < SMB2_HEADER_SIZE + 64 || Smb2Get16(r, SMB2_HEADER_SIZE) != 65)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    dialect = Smb2Get16(r, SMB2_HEADER_SIZE + 4);
    if (dialect != SMB2_DIALECT_202 && dialect != SMB2_DIALECT_210)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    c->Dialect = dialect;
    c->SecurityMode = Smb2Get16(r, SMB2_HEADER_SIZE + 2);
    c->MaxTransact = Smb2Get32(r, SMB2_HEADER_SIZE + 28);
    c->MaxRead = Smb2Get32(r, SMB2_HEADER_SIZE + 32);
    free(r);
    return SMB2_STATUS_SUCCESS;
}

/* 2.2.5 SESSION_SETUP request, 2.2.6 response; the security blob is an NTLMSSP token from SSPI. */
static uint32_t SessionSetupRound(SMB2_CONN *c, const uint8_t *Token, size_t TokenLen,
                                  uint8_t **Resp, size_t *RespLen, uint8_t **OutToken, size_t *OutTokenLen)
{
    uint8_t *body = (uint8_t *)calloc(1, 24 + TokenLen + 1), *r;
    uint32_t st;
    uint16_t off, len;

    *OutToken = NULL;
    *OutTokenLen = 0;
    if (!body)
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    Smb2Put16(body, 0, 25);
    body[3] = (c->SecurityMode & SMB2_NEGOTIATE_SIGNING_REQUIRED) ? SMB2_NEGOTIATE_SIGNING_REQUIRED
                                                                   : SMB2_NEGOTIATE_SIGNING_ENABLED;
    Smb2Put16(body, 12, SMB2_HEADER_SIZE + 24);     /* SecurityBufferOffset */
    Smb2Put16(body, 14, (uint16_t)TokenLen);
    memcpy(body + 24, Token, TokenLen);

    st = Transact(c, SMB2_SESSION_SETUP, 0, body, 24 + TokenLen, &r, RespLen);
    free(body);
    *Resp = r;
    if (!r)
        return st;
    if (st != SMB2_STATUS_SUCCESS && st != SMB2_STATUS_MORE_PROCESSING)
        return st;
    if (*RespLen < SMB2_HEADER_SIZE + 8 || Smb2Get16(r, SMB2_HEADER_SIZE) != 9)
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    c->SessionId = Smb2Get64(r, SMB2H_SESSIONID);
    c->SessionFlags = Smb2Get16(r, SMB2_HEADER_SIZE + 2);
    off = Smb2Get16(r, SMB2_HEADER_SIZE + 4);
    len = Smb2Get16(r, SMB2_HEADER_SIZE + 6);
    if (len)
    {
        if (off < SMB2_HEADER_SIZE + 8 || (size_t)off + len > *RespLen)
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        *OutToken = r + off;
        *OutTokenLen = len;
    }
    return st;
}

uint32_t Smb2SessionSetup(SMB2_CONN *c, const WCHAR *Host, const WCHAR *User, const WCHAR *Domain, const WCHAR *Password)
{
    SEC_WINNT_AUTH_IDENTITY_W ident, *pident = NULL;
    SecBuffer outbuf, inbuf;
    SecBufferDesc outdesc, indesc;
    SECURITY_STATUS ss;
    ULONG attrs;
    TimeStamp ts;
    WCHAR target[300];
    const uint8_t *intok = NULL;
    size_t inlen = 0;
    uint8_t *resp, *prev = NULL, *tok, *final = NULL;
    size_t resplen, toklen, finallen = 0;
    uint32_t st = SMB2_STATUS_LOGON_FAILURE;
    int first = 1;

    if (User)
    {
        memset(&ident, 0, sizeof(ident));
        ident.User = (unsigned short *)User;
        ident.UserLength = (unsigned long)wcslen(User);
        ident.Domain = (unsigned short *)(Domain ? Domain : L"");
        ident.DomainLength = (unsigned long)(Domain ? wcslen(Domain) : 0);
        ident.Password = (unsigned short *)(Password ? Password : L"");
        ident.PasswordLength = (unsigned long)(Password ? wcslen(Password) : 0);
        ident.Flags = SEC_WINNT_AUTH_IDENTITY_UNICODE;
        pident = &ident;
    }
    ss = AcquireCredentialsHandleW(NULL, (SEC_WCHAR *)L"NTLM", SECPKG_CRED_OUTBOUND, NULL, pident, NULL, NULL,
                                   &c->Cred, &ts);
    if (ss != SEC_E_OK)
        return SMB2_STATUS_LOGON_FAILURE;
    c->HaveCred = 1;
    _snwprintf(target, 300, L"cifs/%ls", Host);

    for (;;)
    {
        outbuf.BufferType = SECBUFFER_TOKEN;
        outbuf.cbBuffer = 0;
        outbuf.pvBuffer = NULL;
        outdesc.ulVersion = SECBUFFER_VERSION;
        outdesc.cBuffers = 1;
        outdesc.pBuffers = &outbuf;
        if (intok)
        {
            inbuf.BufferType = SECBUFFER_TOKEN;
            inbuf.cbBuffer = (ULONG)inlen;
            inbuf.pvBuffer = (void *)intok;
            indesc.ulVersion = SECBUFFER_VERSION;
            indesc.cBuffers = 1;
            indesc.pBuffers = &inbuf;
        }
        ss = InitializeSecurityContextW(&c->Cred, first ? NULL : &c->Ctx, target,
                                        ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONNECTION | ISC_REQ_INTEGRITY |
                                        ISC_REQ_CONFIDENTIALITY,
                                        0, SECURITY_NATIVE_DREP, intok ? &indesc : NULL, 0, &c->Ctx, &outdesc,
                                        &attrs, &ts);
        /* SSPI no longer needs the previous server token */
        free(prev);
        prev = NULL;
        intok = NULL;

        if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED)
        {
            st = SMB2_STATUS_LOGON_FAILURE;
            break;
        }
        c->HaveCtx = 1;
        first = 0;
        if (!outbuf.cbBuffer)
        {
            st = (ss == SEC_E_OK) ? SMB2_STATUS_SUCCESS : SMB2_STATUS_LOGON_FAILURE;
            break;
        }

        resp = NULL;
        tok = NULL;
        toklen = 0;
        st = SessionSetupRound(c, (const uint8_t *)outbuf.pvBuffer, outbuf.cbBuffer, &resp, &resplen, &tok, &toklen);
        FreeContextBuffer(outbuf.pvBuffer);
        if (st == SMB2_STATUS_MORE_PROCESSING && ss == SEC_I_CONTINUE_NEEDED && tok)
        {
            prev = resp;
            intok = tok;
            inlen = toklen;
            continue;
        }
        if (st == SMB2_STATUS_SUCCESS && ss == SEC_E_OK)
        {
            final = resp;
            finallen = resplen;
        }
        else
        {
            free(resp);
        }
        if (st == SMB2_STATUS_SUCCESS && ss != SEC_E_OK)
            st = SMB2_STATUS_LOGON_FAILURE;
        break;
    }
    free(prev);

    if (st == SMB2_STATUS_SUCCESS)
    {
        SecPkgContext_SessionKey sk;

        memset(&sk, 0, sizeof(sk));
        if (QueryContextAttributesW(&c->Ctx, SECPKG_ATTR_SESSION_KEY, &sk) == SEC_E_OK && sk.SessionKeyLength)
        {
            size_t n = sk.SessionKeyLength < 16 ? sk.SessionKeyLength : 16;

            memset(c->SigningKey, 0, 16);
            memcpy(c->SigningKey, sk.SessionKey, n);
            FreeContextBuffer(sk.SessionKey);
        }
        /* Sign when the server demands it; guest and null sessions have no key to sign with. */
        c->Signing = (c->SecurityMode & SMB2_NEGOTIATE_SIGNING_REQUIRED) != 0 &&
                     !(c->SessionFlags & (SMB2_SESSION_FLAG_IS_GUEST | SMB2_SESSION_FLAG_IS_NULL));
        if (final && !(c->SessionFlags & (SMB2_SESSION_FLAG_IS_GUEST | SMB2_SESSION_FLAG_IS_NULL)))
        {
            int signed_ = (Smb2Get32(final, SMB2H_FLAGS) & SMB2_FLAGS_SIGNED) != 0;

            if ((c->Signing && !signed_) || (signed_ && !Smb2VerifyMessage(c->SigningKey, final, finallen)))
                st = SMB2_STATUS_ACCESS_DENIED;
        }
        if (st != SMB2_STATUS_SUCCESS)
        {
            c->Signing = 0;
            free(final);
            return st;
        }
        c->Reader = CreateThread(NULL, 0, ReaderProc, c, 0, NULL);
        if (c->Reader)
            InterlockedExchange(&c->Mux, 1);
    }
    free(final);
    return st;
}

/* 2.2.9 TREE_CONNECT: path is \\host\share in UTF-16 */
uint32_t Smb2TreeConnect(SMB2_CONN *c, const WCHAR *Host, const WCHAR *Share, uint32_t *TreeId)
{
    WCHAR path[600];
    uint8_t *body, *r;
    size_t pathbytes, rlen;
    uint32_t st;

    _snwprintf(path, 600, L"\\\\%ls\\%ls", Host, Share);
    pathbytes = wcslen(path) * sizeof(WCHAR);
    body = (uint8_t *)calloc(1, 8 + pathbytes);
    if (!body)
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    Smb2Put16(body, 0, 9);
    Smb2Put16(body, 4, SMB2_HEADER_SIZE + 8);
    Smb2Put16(body, 6, (uint16_t)pathbytes);
    memcpy(body + 8, path, pathbytes);
    st = Transact(c, SMB2_TREE_CONNECT, 0, body, 8 + pathbytes, &r, &rlen);
    free(body);
    if (st != SMB2_STATUS_SUCCESS)
    {
        free(r);
        return st;
    }
    if (rlen < SMB2_HEADER_SIZE + 16 || Smb2Get16(r, SMB2_HEADER_SIZE) != 16)
        st = SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    else
        *TreeId = Smb2Get32(r, SMB2H_TREEID);
    free(r);
    return st;
}

static uint32_t SimpleRequest4(SMB2_CONN *c, uint16_t Cmd, uint32_t TreeId)
{
    uint8_t body[4], *r;
    size_t rlen;
    uint32_t st;

    memset(body, 0, sizeof(body));
    Smb2Put16(body, 0, 4);
    st = Transact(c, Cmd, TreeId, body, sizeof(body), &r, &rlen);
    free(r);
    return st;
}

uint32_t Smb2TreeDisconnect(SMB2_CONN *c, uint32_t TreeId)
{
    return SimpleRequest4(c, SMB2_TREE_DISCONNECT, TreeId);
}

uint32_t Smb2Logoff(SMB2_CONN *c)
{
    return SimpleRequest4(c, SMB2_LOGOFF, 0);
}

/* 2.2.13 CREATE, 2.2.14 response */
uint32_t Smb2CreateEx(SMB2_CONN *c, uint32_t TreeId, const WCHAR *Path, uint32_t DesiredAccess,
                      uint32_t ShareAccess, uint32_t FileAttributes, uint32_t CreateDisposition,
                      uint32_t CreateOptions, SMB2_CREATE_RESULT *Res)
{
    size_t namebytes = wcslen(Path) * sizeof(WCHAR), bodylen = 56 + (namebytes ? namebytes : 1), rlen;
    uint8_t *body, *r;
    uint32_t st;

    if (namebytes > 0xFFFF)
        return SMB2_STATUS_INVALID_PARAMETER;
    body = (uint8_t *)calloc(1, bodylen);
    if (!body)
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    Smb2Put16(body, 0, 57);
    body[3] = SMB2_OPLOCK_NONE;
    Smb2Put32(body, 4, SMB2_IMPERSONATION);
    Smb2Put32(body, 24, DesiredAccess);
    Smb2Put32(body, 28, FileAttributes);
    Smb2Put32(body, 32, ShareAccess);
    Smb2Put32(body, 36, CreateDisposition);
    Smb2Put32(body, 40, CreateOptions);
    Smb2Put16(body, 44, SMB2_HEADER_SIZE + 56);
    Smb2Put16(body, 46, (uint16_t)namebytes);
    memcpy(body + 56, Path, namebytes);

    st = Transact(c, SMB2_CREATE, TreeId, body, bodylen, &r, &rlen);
    free(body);
    if (st != SMB2_STATUS_SUCCESS)
    {
        free(r);
        return st;
    }
    if (rlen < SMB2_HEADER_SIZE + 88 || Smb2Get16(r, SMB2_HEADER_SIZE) != 89)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    Res->CreateAction = Smb2Get32(r, SMB2_HEADER_SIZE + 4);
    Res->CreationTime = Smb2Get64(r, SMB2_HEADER_SIZE + 8);
    Res->LastAccessTime = Smb2Get64(r, SMB2_HEADER_SIZE + 16);
    Res->LastWriteTime = Smb2Get64(r, SMB2_HEADER_SIZE + 24);
    Res->ChangeTime = Smb2Get64(r, SMB2_HEADER_SIZE + 32);
    Res->AllocationSize = Smb2Get64(r, SMB2_HEADER_SIZE + 40);
    Res->EndOfFile = Smb2Get64(r, SMB2_HEADER_SIZE + 48);
    Res->Attributes = Smb2Get32(r, SMB2_HEADER_SIZE + 56);
    Res->FileId.Persistent = Smb2Get64(r, SMB2_HEADER_SIZE + 64);
    Res->FileId.Volatile = Smb2Get64(r, SMB2_HEADER_SIZE + 72);
    free(r);
    return SMB2_STATUS_SUCCESS;
}

uint32_t Smb2Create(SMB2_CONN *c, uint32_t TreeId, const WCHAR *Path, uint32_t DesiredAccess,
                    uint32_t CreateDisposition, uint32_t CreateOptions, SMB2_FILE_ID *FileId,
                    uint64_t *EndOfFile, uint32_t *Attributes)
{
    SMB2_CREATE_RESULT res;
    uint32_t st = Smb2CreateEx(c, TreeId, Path, DesiredAccess, 7, 0, CreateDisposition, CreateOptions, &res);

    if (st != SMB2_STATUS_SUCCESS)
        return st;
    *FileId = res.FileId;
    if (EndOfFile)
        *EndOfFile = res.EndOfFile;
    if (Attributes)
        *Attributes = res.Attributes;
    return SMB2_STATUS_SUCCESS;
}

/* 2.2.15 CLOSE */
uint32_t Smb2Close(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId)
{
    uint8_t body[24], *r;
    size_t rlen;
    uint32_t st;

    memset(body, 0, sizeof(body));
    Smb2Put16(body, 0, 24);
    Smb2Put64(body, 8, FileId->Persistent);
    Smb2Put64(body, 16, FileId->Volatile);
    st = Transact(c, SMB2_CLOSE, TreeId, body, sizeof(body), &r, &rlen);
    free(r);
    return st;
}

/* 2.2.19 READ, 2.2.20 response. Length is clamped to one request (SMB2_MAX_IO). */
uint32_t Smb2Read(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset,
                  uint32_t Length, void *Buffer, uint32_t *BytesRead)
{
    uint8_t body[49], *r;
    size_t rlen;
    uint32_t st, dlen;
    uint8_t doff;

    *BytesRead = 0;
    if (Length > SMB2_MAX_IO)
        Length = SMB2_MAX_IO;
    memset(body, 0, sizeof(body));
    Smb2Put16(body, 0, 49);
    Smb2Put32(body, 4, Length);
    Smb2Put64(body, 8, Offset);
    Smb2Put64(body, 16, FileId->Persistent);
    Smb2Put64(body, 24, FileId->Volatile);
    st = Transact(c, SMB2_READ, TreeId, body, sizeof(body), &r, &rlen);
    if (st != SMB2_STATUS_SUCCESS)
    {
        free(r);
        return st;
    }
    if (rlen < SMB2_HEADER_SIZE + 16 || Smb2Get16(r, SMB2_HEADER_SIZE) != 17)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    doff = r[SMB2_HEADER_SIZE + 2];
    dlen = Smb2Get32(r, SMB2_HEADER_SIZE + 4);
    if (doff < SMB2_HEADER_SIZE + 16 || (size_t)doff + dlen > rlen || dlen > Length)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    memcpy(Buffer, r + doff, dlen);
    *BytesRead = dlen;
    free(r);
    return SMB2_STATUS_SUCCESS;
}

/* 2.2.33 QUERY_DIRECTORY with FileDirectoryInformation (2.2.34 response, MS-FSCC 2.4.10 entries) */
uint32_t Smb2QueryDirectory(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId,
                            SMB2_DIR_CALLBACK Callback, void *Context)
{
    static const WCHAR pattern[] = L"*";
    int restart = 1;

    for (;;)
    {
        uint8_t body[32 + sizeof(pattern)], *r;
        size_t rlen;
        uint32_t st;
        uint16_t off;
        uint32_t len, pos;

        memset(body, 0, sizeof(body));
        Smb2Put16(body, 0, 33);
        body[2] = SMB2_FILE_DIRECTORY_INFORMATION;
        body[3] = restart ? SMB2_RESTART_SCANS : 0;
        Smb2Put64(body, 8, FileId->Persistent);
        Smb2Put64(body, 16, FileId->Volatile);
        Smb2Put16(body, 24, SMB2_HEADER_SIZE + 32);
        Smb2Put16(body, 26, 2);                          /* "*" without the terminator */
        Smb2Put32(body, 28, SMB2_MAX_IO / 2);
        memcpy(body + 32, pattern, 2);
        restart = 0;

        st = Transact(c, SMB2_QUERY_DIRECTORY, TreeId, body, 34, &r, &rlen);
        if (st == SMB2_STATUS_NO_MORE_FILES)
        {
            free(r);
            return SMB2_STATUS_SUCCESS;
        }
        if (st != SMB2_STATUS_SUCCESS)
        {
            free(r);
            return st;
        }
        if (rlen < SMB2_HEADER_SIZE + 8 || Smb2Get16(r, SMB2_HEADER_SIZE) != 9)
        {
            free(r);
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        }
        off = Smb2Get16(r, SMB2_HEADER_SIZE + 2);
        len = Smb2Get32(r, SMB2_HEADER_SIZE + 4);
        if (off < SMB2_HEADER_SIZE + 8 || (size_t)off + len > rlen)
        {
            free(r);
            return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
        }
        for (pos = 0; pos + 64 <= len;)
        {
            const uint8_t *e = r + off + pos;
            uint32_t next = Smb2Get32(e, 0), nlen = Smb2Get32(e, 60);
            SMB2_DIRENT d;

            if (nlen > sizeof(d.Name) - sizeof(WCHAR) || pos + 64 + nlen > len)
            {
                free(r);
                return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
            }
            d.LastWriteTime = Smb2Get64(e, 24);
            d.EndOfFile = Smb2Get64(e, 40);
            d.AllocationSize = Smb2Get64(e, 48);
            d.Attributes = Smb2Get32(e, 56);
            memcpy(d.Name, e + 64, nlen);
            d.Name[nlen / sizeof(WCHAR)] = 0;
            if (!Callback(&d, Context))
            {
                free(r);
                return SMB2_STATUS_SUCCESS;
            }
            if (!next)
                break;
            pos += next;
        }
        free(r);
    }
}

/* 2.2.33 QUERY_DIRECTORY: one round, the response entries are returned unparsed (MS-FSCC layouts) */
uint32_t Smb2QueryDirectoryRaw(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint8_t InfoClass,
                               uint8_t Flags, const WCHAR *Pattern, uint32_t OutputLength, void *Buffer,
                               uint32_t *Returned)
{
    static const WCHAR star[] = L"*";
    size_t patbytes, bodylen;
    uint8_t *body, *r;
    size_t rlen;
    uint32_t st, len;
    uint16_t off;

    *Returned = 0;
    if (!Pattern)
        Pattern = star;
    patbytes = wcslen(Pattern) * sizeof(WCHAR);
    if (patbytes > 0xFFFF || OutputLength > SMB2_MAX_IO)
        return SMB2_STATUS_INVALID_PARAMETER;
    bodylen = 32 + (patbytes ? patbytes : 1);
    body = (uint8_t *)calloc(1, bodylen);
    if (!body)
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    Smb2Put16(body, 0, 33);
    body[2] = InfoClass;
    body[3] = Flags;
    Smb2Put64(body, 8, FileId->Persistent);
    Smb2Put64(body, 16, FileId->Volatile);
    Smb2Put16(body, 24, SMB2_HEADER_SIZE + 32);
    Smb2Put16(body, 26, (uint16_t)patbytes);
    Smb2Put32(body, 28, OutputLength);
    memcpy(body + 32, Pattern, patbytes);
    st = Transact(c, SMB2_QUERY_DIRECTORY, TreeId, body, bodylen, &r, &rlen);
    free(body);
    if (st != SMB2_STATUS_SUCCESS)
    {
        free(r);
        return st;
    }
    if (rlen < SMB2_HEADER_SIZE + 8 || Smb2Get16(r, SMB2_HEADER_SIZE) != 9)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    off = Smb2Get16(r, SMB2_HEADER_SIZE + 2);
    len = Smb2Get32(r, SMB2_HEADER_SIZE + 4);
    if (off < SMB2_HEADER_SIZE + 8 || (size_t)off + len > rlen || len > OutputLength)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    memcpy(Buffer, r + off, len);
    *Returned = len;
    free(r);
    return SMB2_STATUS_SUCCESS;
}

/* 2.2.21 WRITE, 2.2.22 response */
uint32_t Smb2Write(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset,
                   const void *Data, uint32_t Length, uint32_t *Written)
{
    size_t bodylen = 48 + (Length ? Length : 1), rlen;
    uint8_t *body, *r;
    uint32_t st;

    *Written = 0;
    if (Length > SMB2_MAX_IO)
        return SMB2_STATUS_INVALID_PARAMETER;
    body = (uint8_t *)calloc(1, bodylen);
    if (!body)
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    Smb2Put16(body, 0, 49);
    Smb2Put16(body, 2, SMB2_HEADER_SIZE + 48);          /* DataOffset */
    Smb2Put32(body, 4, Length);
    Smb2Put64(body, 8, Offset);
    Smb2Put64(body, 16, FileId->Persistent);
    Smb2Put64(body, 24, FileId->Volatile);
    if (Length)
        memcpy(body + 48, Data, Length);
    st = Transact(c, SMB2_WRITE, TreeId, body, bodylen, &r, &rlen);
    free(body);
    if (st != SMB2_STATUS_SUCCESS)
    {
        free(r);
        return st;
    }
    if (rlen < SMB2_HEADER_SIZE + 16 || Smb2Get16(r, SMB2_HEADER_SIZE) != 17 ||
        Smb2Get32(r, SMB2_HEADER_SIZE + 4) > Length)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    *Written = Smb2Get32(r, SMB2_HEADER_SIZE + 4);
    free(r);
    return SMB2_STATUS_SUCCESS;
}

/* 2.2.17 FLUSH */
uint32_t Smb2Flush(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId)
{
    uint8_t body[24], *r;
    size_t rlen;
    uint32_t st;

    memset(body, 0, sizeof(body));
    Smb2Put16(body, 0, 24);
    Smb2Put64(body, 8, FileId->Persistent);
    Smb2Put64(body, 16, FileId->Volatile);
    st = Transact(c, SMB2_FLUSH, TreeId, body, sizeof(body), &r, &rlen);
    free(r);
    return st;
}

/* 2.2.26 LOCK */
uint32_t Smb2Lock(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset, uint64_t Length,
                  uint32_t Flags)
{
    uint8_t body[SMB2ENC_LOCK_BODY], *r;
    size_t rlen;
    uint32_t st;

    st = Smb2ValidateLockRange(Offset, Length, Flags);
    if (st != SMB2_STATUS_SUCCESS)
        return st;
    Smb2BuildLockBody(body, FileId->Persistent, FileId->Volatile, Offset, Length, Flags, 0);
    st = Transact(c, SMB2_LOCK, TreeId, body, sizeof(body), &r, &rlen);
    free(r);
    return st;
}

/* As Smb2Lock, but the server may hold the request: returns when the range is granted, refused, or Op was cancelled */
uint32_t Smb2LockWait(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint64_t Offset, uint64_t Length,
                      uint32_t Flags, SMB2_OP *Op)
{
    uint8_t body[SMB2ENC_LOCK_BODY], *r;
    size_t rlen;
    uint32_t st;

    if (!c->Mux)
        return SMB2_STATUS_NOT_SUPPORTED;
    st = Smb2ValidateLockRange(Offset, Length, Flags);
    if (st != SMB2_STATUS_SUCCESS)
        return st;
    Smb2BuildLockBody(body, FileId->Persistent, FileId->Volatile, Offset, Length, Flags, 1);
    st = TransactEx(c, SMB2_LOCK, TreeId, body, sizeof(body), &r, &rlen, Op);
    free(r);
    return st;
}

/* 2.2.35 CHANGE_NOTIFY */
uint32_t Smb2ChangeNotify(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint32_t Filter,
                          int WatchTree, uint32_t OutputLength, void *Buffer, uint32_t *Returned, SMB2_OP *Op)
{
    uint8_t body[SMB2ENC_NOTIFY_BODY], *r;
    size_t rlen;
    uint32_t st, len;
    uint16_t off;

    *Returned = 0;
    if (!c->Mux)
        return SMB2_STATUS_NOT_SUPPORTED;
    if (OutputLength == 0 || OutputLength > SMB2_MAX_IO)
        return SMB2_STATUS_INVALID_PARAMETER;
    Smb2BuildNotifyBody(body, FileId->Persistent, FileId->Volatile, Filter, WatchTree, OutputLength);
    st = TransactEx(c, SMB2_CHANGE_NOTIFY, TreeId, body, sizeof(body), &r, &rlen, Op);
    if (st != SMB2_STATUS_SUCCESS)
    {
        free(r);
        return st;
    }
    if (rlen < SMB2_HEADER_SIZE + 8 || Smb2Get16(r, SMB2_HEADER_SIZE) != 9)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    off = Smb2Get16(r, SMB2_HEADER_SIZE + 2);
    len = Smb2Get32(r, SMB2_HEADER_SIZE + 4);
    if (len > OutputLength || (len && (off < SMB2_HEADER_SIZE + 8 || (size_t)off + len > rlen)) ||
        Smb2ValidateNotifyChain(r + off, len) != SMB2_STATUS_SUCCESS)
    {
        free(r);
        return SMB2_STATUS_INVALID_NETWORK_RESPONSE;
    }
    if (len)
        memcpy(Buffer, r + off, len);
    *Returned = len;
    free(r);
    return SMB2_STATUS_SUCCESS;
}

/* 2.2.39 SET_INFO (file information) */
uint32_t Smb2SetInfo(SMB2_CONN *c, uint32_t TreeId, const SMB2_FILE_ID *FileId, uint8_t InfoClass,
                     const void *Buffer, uint32_t BufferLength)
{
    size_t bodylen = 32 + (BufferLength ? BufferLength : 1), rlen;
    uint8_t *body, *r;
    uint32_t st;

    if (BufferLength > SMB2_MAX_IO)
        return SMB2_STATUS_INVALID_PARAMETER;
    body = (uint8_t *)calloc(1, bodylen);
    if (!body)
        return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    Smb2Put16(body, 0, 33);
    body[2] = SMB2_0_INFO_FILE;
    body[3] = InfoClass;
    Smb2Put32(body, 4, BufferLength);
    Smb2Put16(body, 8, SMB2_HEADER_SIZE + 32);          /* BufferOffset */
    Smb2Put64(body, 16, FileId->Persistent);
    Smb2Put64(body, 24, FileId->Volatile);
    if (BufferLength)
        memcpy(body + 32, Buffer, BufferLength);
    st = Transact(c, SMB2_SET_INFO, TreeId, body, bodylen, &r, &rlen);
    free(body);
    free(r);
    return st;
}
