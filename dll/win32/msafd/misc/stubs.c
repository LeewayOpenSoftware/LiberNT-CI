/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS Ancillary Function Driver DLL
 * FILE:        dll/win32/msafd/misc/stubs.c
 * PURPOSE:     Stubs
 * PROGRAMMERS: Casper S. Hornstrup (chorns@users.sourceforge.net)
 * REVISIONS:
 *   CSH 01/09-2000 Created
 */

#include <msafd.h>

INT
WSPAPI
WSPCancelBlockingCall(
    OUT LPINT lpErrno)
{
    UNIMPLEMENTED;

    return 0;
}


BOOL
WSPAPI
WSPGetQOSByName(
    IN      SOCKET s,
    IN OUT  LPWSABUF lpQOSName,
    OUT     LPQOS lpQOS,
    OUT     LPINT lpErrno)
{
    UNIMPLEMENTED;

    return FALSE;
}


SOCKET
WSPAPI
WSPJoinLeaf(
    IN  SOCKET s,
    IN  CONST SOCKADDR *name,
    IN  INT namelen,
    IN  LPWSABUF lpCallerData,
    OUT LPWSABUF lpCalleeData,
    IN  LPQOS lpSQOS,
    IN  LPQOS lpGQOS,
    IN  DWORD dwFlags,
    OUT LPINT lpErrno)
{
    UNIMPLEMENTED;

    return (SOCKET)0;
}

BOOL
WSPAPI
WSPAcceptEx(
    IN SOCKET sListenSocket,
    IN SOCKET sAcceptSocket,
    OUT PVOID lpOutputBuffer,
    IN DWORD dwReceiveDataLength,
    IN DWORD dwLocalAddressLength,
    IN DWORD dwRemoteAddressLength,
    OUT LPDWORD lpdwBytesReceived,
    IN OUT LPOVERLAPPED lpOverlapped)
{
    PSOCKET_INFORMATION Socket;
    AFD_SUPER_ACCEPT_INFO AcceptInfo;
    PIO_STATUS_BLOCK IOSB;
    NTSTATUS Status;

    Socket = GetSocketStructure(sListenSocket);
    if (!Socket)
    {
        SetLastError(WSAENOTSOCK);
        return FALSE;
    }

    if (!lpOverlapped || !lpOutputBuffer || !GetSocketStructure(sAcceptSocket))
    {
        SetLastError(WSAEINVAL);
        return FALSE;
    }

    AcceptInfo.AcceptHandle = (HANDLE)sAcceptSocket;
    AcceptInfo.ReceiveDataLength = dwReceiveDataLength;
    AcceptInfo.LocalAddressLength = dwLocalAddressLength;
    AcceptInfo.RemoteAddressLength = dwRemoteAddressLength;

    IOSB = (PIO_STATUS_BLOCK)lpOverlapped;
    IOSB->Status = STATUS_PENDING;

    Status = NtDeviceIoControlFile((HANDLE)sListenSocket,
                                   lpOverlapped->hEvent,
                                   NULL,
                                   lpOverlapped->hEvent ? NULL : lpOverlapped,
                                   IOSB,
                                   IOCTL_AFD_SUPER_ACCEPT,
                                   lpOutputBuffer,
                                   dwReceiveDataLength + dwLocalAddressLength + dwRemoteAddressLength,
                                   &AcceptInfo,
                                   sizeof(AcceptInfo));

    if (Status == STATUS_SUCCESS && lpdwBytesReceived)
        *lpdwBytesReceived = (DWORD)IOSB->Information;

    SetLastError(TranslateNtStatusError(Status));

    return Status == STATUS_SUCCESS;
}

BOOL
WSPAPI
WSPDisconnectEx(
  IN SOCKET hSocket,
  IN LPOVERLAPPED lpOverlapped,
  IN DWORD dwFlags,
  IN DWORD reserved)
{
    PIO_STATUS_BLOCK        IOSB;
    IO_STATUS_BLOCK         DummyIOSB;
    AFD_DISCONNECT_INFO     DisconnectInfo;
    PSOCKET_INFORMATION     Socket;
    NTSTATUS                Status;
    PVOID                   APCContext;
    HANDLE                  Event = NULL;
    HANDLE                  SockEvent;
    BOOLEAN                 SockEventShared, SockEventStale = FALSE;

    /* Get the Socket Structure associate to this Socket */
    Socket = GetSocketStructure(hSocket);
    if (!Socket)
    {
        WSASetLastError(WSAENOTSOCK);
        return FALSE;
    }

    /* The socket must be connected */
    if (Socket->SharedData->State != SocketConnected)
    {
        WSASetLastError(WSAENOTCONN);
        return FALSE;
    }

    /* Only a plain graceful disconnect is supported; every other request
       must fail explicitly, never succeed silently */
    if (reserved != 0 || (dwFlags & ~TF_REUSE_SOCKET) != 0)
    {
        WSASetLastError(WSAEINVAL);
        return FALSE;
    }
    if (dwFlags & TF_REUSE_SOCKET)
    {
        /* Socket reuse requires AFD support (Stage B) */
        WSASetLastError(WSAEOPNOTSUPP);
        return FALSE;
    }

    Status = MsafdAcquireEvent(&SockEvent, &SockEventShared);
    if (!NT_SUCCESS(Status))
    {
        WSASetLastError(WSAENOBUFS);
        return FALSE;
    }

    /* Graceful disconnect: a FIN once the pending sends drain */
    DisconnectInfo.DisconnectType = AFD_DISCONNECT_SEND;
    DisconnectInfo.Timeout.QuadPart = -1000000LL;
    Socket->SharedData->SendShutdown = TRUE;

    if (!lpOverlapped)
    {
        /* Synchronous: block on our own event */
        APCContext = NULL;
        Event = SockEvent;
        IOSB = &DummyIOSB;
    }
    else
    {
        /* Overlapped request for non overlapped opened socket */
        if ((Socket->SharedData->CreateFlags & SO_SYNCHRONOUS_NONALERT) != 0)
        {
            TRACE("Opened without flag WSA_FLAG_OVERLAPPED. Do nothing.\n");
            MsafdReleaseEvent(SockEvent, SockEventShared, FALSE);
            return TRUE;
        }

        IOSB = (PIO_STATUS_BLOCK)lpOverlapped;
        Event = lpOverlapped->hEvent;
        APCContext = Event ? NULL : (PVOID)lpOverlapped;
    }

    IOSB->Information = 0;
    IOSB->Status = STATUS_PENDING;

    /* Send IOCTL */
    Status = NtDeviceIoControlFile((HANDLE)hSocket,
                                   Event,
                                   NULL,
                                   APCContext,
                                   IOSB,
                                   IOCTL_AFD_DISCONNECT,
                                   &DisconnectInfo,
                                   sizeof(DisconnectInfo),
                                   NULL,
                                   0);

    if (!lpOverlapped)
    {
        if (Status == STATUS_PENDING)
            Status = MsafdWaitForCompletion(Socket, SockEvent, IOSB, Socket->SharedData->SendTimeout);
        else
            SockEventStale = TRUE;
    }

    MsafdReleaseEvent(SockEvent, SockEventShared, SockEventStale);

    if (lpOverlapped && Status == STATUS_PENDING)
    {
        /* Queued: FALSE + WSA_IO_PENDING as Win11; exactly one completion follows */
        WSASetLastError(WSA_IO_PENDING);
        return FALSE;
    }

    if (!NT_SUCCESS(Status))
    {
        WSASetLastError(TranslateNtStatusError(Status));
        return FALSE;
    }

    return TRUE;
}

VOID
WSPAPI
WSPGetAcceptExSockaddrs(
    IN PVOID lpOutputBuffer,
    IN DWORD dwReceiveDataLength,
    IN DWORD dwLocalAddressLength,
    IN DWORD dwRemoteAddressLength,
    OUT struct sockaddr **LocalSockaddr,
    OUT LPINT LocalSockaddrLength,
    OUT struct sockaddr **RemoteSockaddr,
    OUT LPINT RemoteSockaddrLength)
{
    PCHAR Buffer = (PCHAR)lpOutputBuffer + dwReceiveDataLength;

    *LocalSockaddrLength = *(PINT)Buffer;
    *LocalSockaddr = (struct sockaddr *)(Buffer + sizeof(INT));

    Buffer += dwLocalAddressLength;
    *RemoteSockaddrLength = *(PINT)Buffer;
    *RemoteSockaddr = (struct sockaddr *)(Buffer + sizeof(INT));
}

/* EOF */
