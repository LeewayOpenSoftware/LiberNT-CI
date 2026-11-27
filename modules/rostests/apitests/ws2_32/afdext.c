/*
 * PROJECT:         ReactOS api tests
 * LICENSE:         GPL - See COPYING in the top level directory
 * PURPOSE:         Tests for the msafd Stage A extension functions
 *                  (DisconnectEx / WSASendMsg), WDM-IO-AUDIT T10 subset
 * PROGRAMMERS:     Ryan (ryan-mw6dhlvx)
 */

#include "ws2_32.h"

#include <mswsock.h>

static SOCKET
CreateTcpListener(struct sockaddr_in *addr)
{
    SOCKET listener;
    int len = sizeof(*addr);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    ok(listener != INVALID_SOCKET, "socket(listener) failed %d\n", WSAGetLastError());
    if (listener == INVALID_SOCKET)
        return INVALID_SOCKET;

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = inet_addr("127.0.0.1");
    ok(bind(listener, (struct sockaddr *)addr, sizeof(*addr)) == 0,
       "bind failed %d\n", WSAGetLastError());
    ok(getsockname(listener, (struct sockaddr *)addr, &len) == 0,
       "getsockname failed %d\n", WSAGetLastError());
    ok(listen(listener, 4) == 0, "listen failed %d\n", WSAGetLastError());
    return listener;
}

static SOCKET
TcpConnect(const struct sockaddr_in *addr)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    ok(s != INVALID_SOCKET, "socket(connector) failed %d\n", WSAGetLastError());
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;
    ok(connect(s, (const struct sockaddr *)addr, sizeof(*addr)) == 0,
       "connect failed %d\n", WSAGetLastError());
    return s;
}

/* Every failed DisconnectEx must fail explicitly and leave the socket usable */
static char big[65536];

static void
CheckSocketStillConnected(SOCKET s)
{
    char buf = 'x';
    ok(send(s, &buf, 1, 0) == 1, "send after failed DisconnectEx %d\n", WSAGetLastError());
}

START_TEST(afdext)
{
    struct sockaddr_in addr;
    WSABUF buffer;
    WSAMSG msg;
    WSACMSGHDR *cmsg;
    char data[16], control[WSA_CMSG_SPACE(sizeof(struct in_pktinfo))];
    OVERLAPPED overlapped;
    WSADATA wsaData;
    DWORD bytes, flags;
    LPFN_DISCONNECTEX pDisconnectEx = NULL;
    LPFN_WSASENDMSG pWSASendMsg = NULL;
    LPFN_TRANSMITFILE pTransmitFile = NULL;
    GUID disconnectExGuid = WSAID_DISCONNECTEX;
    GUID sendMsgGuid = WSAID_WSASENDMSG;
    GUID transmitFileGuid = WSAID_TRANSMITFILE;
    SOCKET listener, connector, accepted, fresh;
    WSAPOLLFD pollfd;
    BOOL bret;
    INT ret, i;
    u_long nonblock;
    DWORD sent, received, timeout;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        skip("WSAStartup failed %u\n", WSAGetLastError());
        return;
    }

    /* --- Extension function pointer queries (T10) --- */
    listener = CreateTcpListener(&addr);
    if (listener == INVALID_SOCKET)
    {
        WSACleanup();
        return;
    }

    ret = WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER,
                   &disconnectExGuid, sizeof(disconnectExGuid),
                   &pDisconnectEx, sizeof(pDisconnectEx), &bytes, NULL, NULL);
    ok(ret == 0 && pDisconnectEx != NULL,
       "DisconnectEx query failed %d\n", WSAGetLastError());

    ret = WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER,
                   &sendMsgGuid, sizeof(sendMsgGuid),
                   &pWSASendMsg, sizeof(pWSASendMsg), &bytes, NULL, NULL);
    ok(ret == 0 && pWSASendMsg != NULL,
       "WSASendMsg query failed %d\n", WSAGetLastError());

    /* TransmitFile stays a Stage B gap; it must fail explicitly */
    ret = WSAIoctl(listener, SIO_GET_EXTENSION_FUNCTION_POINTER,
                   &transmitFileGuid, sizeof(transmitFileGuid),
                   &pTransmitFile, sizeof(pTransmitFile), &bytes, NULL, NULL);
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP,
       "TransmitFile query got %d err %d\n", ret, WSAGetLastError());

    if (!pDisconnectEx || !pWSASendMsg)
    {
        skip("extension function pointers unavailable\n");
        closesocket(listener);
        return;
    }

    /* --- WSAPoll sanity on the T10 loopback pair --- */
    pollfd.fd = listener;
    pollfd.events = POLLIN;
    pollfd.revents = 0;
    ret = WSAPoll(&pollfd, 1, 0);
    ok(ret == 0, "WSAPoll(listener) got %d err %d\n", ret, WSAGetLastError());

    /* --- DisconnectEx argument validation --- */
    WSASetLastError(0);
    memset(&overlapped, 0, sizeof(overlapped));
    bret = pDisconnectEx(INVALID_SOCKET, &overlapped, 0, 0);
    ok(!bret && WSAGetLastError() == WSAENOTSOCK,
       "DisconnectEx(INVALID_SOCKET) got %d err %d\n", bret, WSAGetLastError());

    fresh = socket(AF_INET, SOCK_STREAM, 0);
    ok(fresh != INVALID_SOCKET, "socket(fresh) failed %d\n", WSAGetLastError());
    WSASetLastError(0);
    bret = pDisconnectEx(fresh, &overlapped, 0, 0);
    ok(!bret && WSAGetLastError() == WSAENOTCONN,
       "DisconnectEx(unconnected) got %d err %d\n", bret, WSAGetLastError());
    closesocket(fresh);

    connector = TcpConnect(&addr);
    accepted = accept(listener, NULL, NULL);
    ok(accepted != INVALID_SOCKET, "accept failed %d\n", WSAGetLastError());
    if (connector == INVALID_SOCKET || accepted == INVALID_SOCKET)
    {
        skip("loopback connection failed\n");
        closesocket(listener);
        return;
    }

    /* TF_REUSE_SOCKET is a Stage B gap: explicit failure, no silent success.
       Win11 supports reuse; this is a recorded Stage A divergence. */
    WSASetLastError(0);
    bret = pDisconnectEx(connector, &overlapped, TF_REUSE_SOCKET, 0);
    ok(!bret && WSAGetLastError() == WSAEOPNOTSUPP,
       "DisconnectEx(TF_REUSE_SOCKET) got %d err %d\n", bret, WSAGetLastError());
    CheckSocketStillConnected(connector);

    /* reserved must be 0 */
    WSASetLastError(0);
    bret = pDisconnectEx(connector, &overlapped, 0, 1);
    ok(!bret && WSAGetLastError() == WSAEINVAL,
       "DisconnectEx(reserved=1) got %d err %d\n", bret, WSAGetLastError());
    CheckSocketStillConnected(connector);

    /* unknown flags are rejected */
    WSASetLastError(0);
    bret = pDisconnectEx(connector, &overlapped, 0x80000000, 0);
    ok(!bret && WSAGetLastError() == WSAEINVAL,
       "DisconnectEx(flags=0x80000000) got %d err %d\n", bret, WSAGetLastError());
    CheckSocketStillConnected(connector);

    /* The peer drains the three bytes the checks above sent */
    for (bytes = 0; bytes < 3; bytes += ret)
    {
        ret = recv(accepted, data, 3 - bytes, 0);
        if (ret <= 0)
            break;
    }
    ok(bytes == 3, "peer drained %lu bytes, expected 3\n", bytes);

    /* --- DisconnectEx overlapped on a connected socket --- */
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = WSACreateEvent();
    ok(overlapped.hEvent != WSA_INVALID_EVENT, "WSACreateEvent failed\n");

    bret = pDisconnectEx(connector, &overlapped, 0, 0);
    if (bret)
    {
        /* completed at once */
        ok(overlapped.Internal == 0,
           "expected a completed request, got %08lx\n", (ULONG)overlapped.Internal);
    }
    else
    {
        ok(WSAGetLastError() == ERROR_IO_PENDING,
           "expected ERROR_IO_PENDING, got %d\n", WSAGetLastError());
        bret = WSAGetOverlappedResult(connector, &overlapped, &bytes, TRUE, &flags);
    }
    ok(bret, "DisconnectEx(overlapped) failed %d\n", WSAGetLastError());
    WSACloseEvent(overlapped.hEvent);

    /* The peer sees a graceful EOF */
    ret = recv(accepted, data, sizeof(data), 0);
    ok(ret == 0, "recv after DisconnectEx got %d err %d\n", ret, WSAGetLastError());

    /* The connection state survives, so a reconnect must refuse */
    ret = connect(connector, (const struct sockaddr *)&addr, sizeof(addr));
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAEISCONN,
       "reconnect got %d err %d\n", ret, WSAGetLastError());

    closesocket(accepted);
    closesocket(connector);

    /* --- DisconnectEx synchronous on a fresh connection --- */
    connector = TcpConnect(&addr);
    accepted = accept(listener, NULL, NULL);
    ok(accepted != INVALID_SOCKET, "accept failed %d\n", WSAGetLastError());

    bret = pDisconnectEx(connector, NULL, 0, 0);
    ok(bret, "DisconnectEx(sync) failed %d\n", WSAGetLastError());
    ret = recv(accepted, data, sizeof(data), 0);
    ok(ret == 0, "peer recv after sync DisconnectEx got %d\n", ret);
    ret = connect(connector, (const struct sockaddr *)&addr, sizeof(addr));
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAEISCONN,
       "reconnect after sync got %d err %d\n", ret, WSAGetLastError());

    closesocket(accepted);
    closesocket(connector);

    /* --- DisconnectEx overlapped behind a full send window pends --- */
    connector = TcpConnect(&addr);
    accepted = accept(listener, NULL, NULL);
    ok(accepted != INVALID_SOCKET, "accept failed %d\n", WSAGetLastError());
    timeout = 10000;
    setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    nonblock = 1;
    ok(ioctlsocket(connector, FIONBIO, &nonblock) == 0, "FIONBIO failed %d\n", WSAGetLastError());
    sent = 0;
    ret = 0;
    for (i = 0; i < 1024; i++)
    {
        ret = send(connector, big, sizeof(big), 0);
        if (ret == SOCKET_ERROR)
            break;
        sent += ret;
    }
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK,
       "filling the send window got %d err %d after %lu bytes\n", ret, WSAGetLastError(), sent);

    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = WSACreateEvent();
    WSASetLastError(0);
    bret = pDisconnectEx(connector, &overlapped, 0, 0);
    ok(!bret && WSAGetLastError() == WSA_IO_PENDING,
       "DisconnectEx(full window) got %d err %d\n", bret, WSAGetLastError());
    received = 0;
    while ((ret = recv(accepted, big, sizeof(big), 0)) > 0)
        received += ret;
    ok(ret == 0 && received == sent, "peer drained %lu of %lu bytes, last %d err %d\n",
       received, sent, ret, WSAGetLastError());
    ok(WaitForSingleObject(overlapped.hEvent, 10000) == WAIT_OBJECT_0,
       "DisconnectEx(full window) did not complete\n");
    ok(WSAGetOverlappedResult(connector, &overlapped, &bytes, FALSE, &flags),
       "DisconnectEx(full window) completion %d\n", WSAGetLastError());
    WSACloseEvent(overlapped.hEvent);
    closesocket(accepted);
    closesocket(connector);

    /* --- WSASendMsg validation --- */
    memset(&msg, 0, sizeof(msg));
    buffer.buf = data;
    buffer.len = sizeof(data);
    msg.lpBuffers = &buffer;
    msg.dwBufferCount = 1;

    WSASetLastError(0);
    ret = pWSASendMsg(INVALID_SOCKET, &msg, 0, NULL, NULL, NULL);
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK,
       "WSASendMsg(INVALID_SOCKET) got %d err %d\n", ret, WSAGetLastError());

    connector = TcpConnect(&addr);
    accepted = accept(listener, NULL, NULL);
    ok(accepted != INVALID_SOCKET, "accept failed %d\n", WSAGetLastError());

    WSASetLastError(0);
    ret = pWSASendMsg(connector, NULL, 0, NULL, NULL, NULL);
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAEFAULT,
       "WSASendMsg(NULL msg) got %d err %d\n", ret, WSAGetLastError());

    /* A destination name on a connection-oriented socket is a Win11 EINVAL */
    msg.name = (struct sockaddr *)&addr;
    msg.namelen = sizeof(addr);
    WSASetLastError(0);
    ret = pWSASendMsg(connector, &msg, 0, &bytes, NULL, NULL);
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL,
       "WSASendMsg(TCP with name) got %d err %d\n", ret, WSAGetLastError());
    msg.name = NULL;
    msg.namelen = 0;

    /* Ancillary data needs AFD support (Stage B); explicit failure recorded
       pending the Win11 column. */
    memset(control, 0, sizeof(control));
    cmsg = (WSACMSGHDR *)control;
    cmsg->cmsg_len = WSA_CMSG_LEN(sizeof(struct in_pktinfo));
    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_PKTINFO;
    msg.Control.buf = control;
    msg.Control.len = (ULONG)WSA_CMSG_LEN(sizeof(struct in_pktinfo));
    WSASetLastError(0);
    ret = pWSASendMsg(connector, &msg, 0, &bytes, NULL, NULL);
    ok(ret == SOCKET_ERROR && WSAGetLastError() == WSAEOPNOTSUPP,
       "WSASendMsg(control) got %d err %d\n", ret, WSAGetLastError());
    msg.Control.buf = NULL;
    msg.Control.len = 0;

    /* Control.len == 0 goes down the WSPSend path */
    strcpy(data, "afdext-t10");
    buffer.len = (ULONG)strlen(data);
    WSASetLastError(0);
    bytes = 0;
    ret = pWSASendMsg(connector, &msg, 0, &bytes, NULL, NULL);
    ok(ret == 0 && bytes == strlen(data),
       "WSASendMsg(TCP) got %d bytes %lu err %d\n", ret, bytes, WSAGetLastError());
    memset(data, 0, sizeof(data));
    ret = recv(accepted, data, sizeof(data) - 1, 0);
    ok(ret == (INT)strlen("afdext-t10") && strcmp(data, "afdext-t10") == 0,
       "peer recv got %d \"%s\"\n", ret, data);

    closesocket(accepted);
    closesocket(connector);
    closesocket(listener);

    WSACleanup();

    /* keep the query referenced */
    (void)pTransmitFile;
}
