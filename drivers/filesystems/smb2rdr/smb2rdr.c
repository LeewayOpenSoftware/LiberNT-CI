/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMB2 mini-redirector: driver entry, RDBSS registration, daemon control
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The driver holds no protocol code: every network operation is an upcall to smb2d (see upcall.c).
 * It registers with Mup through RDBSS, takes the control IOCTLs from the daemon, resolves server names
 * (SrvCall) and shares (VNetRoot) through the daemon, and serves open, close, read, directory listing and
 * basic file/volume information. The mount is read-only: everything that writes fails with
 * STATUS_ACCESS_DENIED or STATUS_NOT_IMPLEMENTED until the write path step.
 */

#include "smb2rdr.h"

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD Smb2RdrUnload;
DRIVER_DISPATCH Smb2RdrFsdDispatch;

PRDBSS_DEVICE_OBJECT Smb2RdrDevice;
static MINIRDR_DISPATCH Smb2RdrOps;
static volatile LONG Smb2State = SMB2RDR_STATE_STOPPED;

/* ---------------- start / stop ---------------- */

static NTSTATUS NTAPI Smb2RdrStart(IN OUT PRX_CONTEXT RxContext, IN OUT PRDBSS_DEVICE_OBJECT Dev)
{
    UNREFERENCED_PARAMETER(RxContext);
    UNREFERENCED_PARAMETER(Dev);
    Smb2State = SMB2RDR_STATE_STARTED;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2RdrStop(IN OUT PRX_CONTEXT RxContext, IN OUT PRDBSS_DEVICE_OBJECT Dev)
{
    UNREFERENCED_PARAMETER(RxContext);
    UNREFERENCED_PARAMETER(Dev);
    Smb2State = SMB2RDR_STATE_STOPPED;
    return STATUS_SUCCESS;
}

/* Control IOCTLs from the daemon (reached through RDBSS device control dispatch) */
static NTSTATUS NTAPI Smb2RdrDevFcbXXXControlFile(IN OUT PRX_CONTEXT RxContext)
{
    PLOWIO_CONTEXT io = &RxContext->LowIoContext;
    ULONG code = io->ParamsFor.FsCtl.FsControlCode;
    ULONG inLen = io->ParamsFor.IoCtl.InputBufferLength;
    ULONG outLen = io->ParamsFor.IoCtl.OutputBufferLength;
    PVOID in = io->ParamsFor.IoCtl.pInputBuffer;
    PVOID out = io->ParamsFor.IoCtl.pOutputBuffer;
    NTSTATUS status;

    if (RxContext->MajorFunction != IRP_MJ_DEVICE_CONTROL &&
        RxContext->MajorFunction != IRP_MJ_INTERNAL_DEVICE_CONTROL)
        return STATUS_INVALID_DEVICE_REQUEST;

    switch (code)
    {
        case IOCTL_SMB2RDR_START:
        {
            ULONG version = 0;

            if (in && inLen >= sizeof(ULONG))
                version = *(PULONG)in;
            if (version != SMB2RDR_PROTOCOL_VERSION)
            {
                Smb2Trace("daemon protocol %lu, driver %u\n", version, SMB2RDR_PROTOCOL_VERSION);
                return STATUS_REVISION_MISMATCH;
            }
            Smb2UpcallSetRunning(TRUE);
            InterlockedExchange(&Smb2State, SMB2RDR_STATE_STARTING);
            status = RxStartMinirdr(RxContext, &RxContext->PostRequest);
            if (status == STATUS_REDIRECTOR_STARTED)
                status = STATUS_SUCCESS;
            else if (status == STATUS_PENDING && RxContext->PostRequest == TRUE)
                status = STATUS_MORE_PROCESSING_REQUIRED;
            if (!NT_SUCCESS(status) && status != STATUS_MORE_PROCESSING_REQUIRED)
            {
                Smb2UpcallSetRunning(FALSE);
                InterlockedExchange(&Smb2State, SMB2RDR_STATE_STOPPED);
            }
            return status;
        }

        case IOCTL_SMB2RDR_STOP:
        {
            /* input: optional ULONG force; without it a redirector with open files refuses to stop */
            ULONG force = (in && inLen >= sizeof(ULONG)) ? *(PULONG)in : 0;

            if (!force && RxContext->RxDeviceObject->NumberOfActiveFcbs > 0)
                return STATUS_REDIRECTOR_HAS_OPEN_HANDLES;
            Smb2UpcallShutdown();
            status = RxStopMinirdr(RxContext, &RxContext->PostRequest);
            if (status == STATUS_PENDING && RxContext->PostRequest == TRUE)
                status = STATUS_MORE_PROCESSING_REQUIRED;
            return status;
        }

        case IOCTL_SMB2RDR_GETSTATE:
            if (!out || outLen < sizeof(ULONG))
                return STATUS_BUFFER_TOO_SMALL;
            *(PULONG)out = (ULONG)Smb2State;
            RxContext->InformationToReturn = sizeof(ULONG);
            return STATUS_SUCCESS;

        case IOCTL_SMB2RDR_GETREQ:
        {
            ULONG written = 0;

            if (!out)
                return STATUS_INVALID_PARAMETER;
            status = Smb2HandleGetRequest(out, outLen, &written);
            if (NT_SUCCESS(status))
                RxContext->InformationToReturn = written;
            return status;
        }

        case IOCTL_SMB2RDR_PUTREPLY:
            if (!in)
                return STATUS_INVALID_PARAMETER;
            return Smb2HandlePutReply(in, inLen);

        default:
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}

/* ---------------- name resolution ---------------- */

/*
 * MRxCreateSrvCall: RDBSS asks whether this minirdr can reach the named server. We ask the daemon to
 * connect and negotiate; its status is the answer. Completion goes through the RDBSS callback.
 */
static NTSTATUS NTAPI Smb2CreateSrvCallWorker(PVOID Context)
{
    PMRX_SRVCALL_CALLBACK_CONTEXT cbctx = Context;
    PMRX_SRVCALLDOWN_STRUCTURE down = (PMRX_SRVCALLDOWN_STRUCTURE)cbctx->SrvCalldownStructure;
    PMRX_SRV_CALL srvCall = down->SrvCall;
    PSMB2RDR_SERVER server = NULL;
    NTSTATUS status;

    if (srvCall->pSrvCallName->Length < 2 * sizeof(WCHAR) ||
        srvCall->pSrvCallName->Length > SMB2RDR_MAX_SERVER_NAME * sizeof(WCHAR))
    {
        status = STATUS_BAD_NETWORK_PATH;
        goto done;
    }

    server = RxAllocatePoolWithTag(PagedPool, sizeof(*server), SMB2RDR_TAG);
    if (!server)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    RtlZeroMemory(server, sizeof(*server));
    server->Name.Buffer = server->NameBuffer;
    server->Name.Length = srvCall->pSrvCallName->Length - sizeof(WCHAR);       /* drop the leading backslash */
    server->Name.MaximumLength = sizeof(server->NameBuffer);
    RtlCopyMemory(server->NameBuffer, srvCall->pSrvCallName->Buffer + 1, server->Name.Length);

    status = Smb2Upcall(SMB2RDR_UP_CONNECT, server->Name.Buffer, server->Name.Length, NULL, 0, NULL);
    if (!NT_SUCCESS(status))
    {
        RxFreePool(server);
        server = NULL;
        goto done;
    }
    server->SrvCall = srvCall;
    cbctx->RecommunicateContext = server;

done:
    cbctx->Status = status;
    down->CallBack(cbctx);
    return status;
}

static VOID NTAPI Smb2CreateSrvCallWorkItem(PVOID Context)
{
    Smb2CreateSrvCallWorker(Context);
}

static NTSTATUS NTAPI Smb2CreateSrvCall(PMRX_SRV_CALL SrvCall, PMRX_SRVCALL_CALLBACK_CONTEXT CallbackContext)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(SrvCall);
    /* The upcall blocks: never run it on the caller's thread */
    status = RxDispatchToWorkerThread(Smb2RdrDevice, DelayedWorkQueue, Smb2CreateSrvCallWorkItem, CallbackContext);
    if (!NT_SUCCESS(status))
    {
        CallbackContext->Status = status;
        CallbackContext->SrvCalldownStructure->CallBack(CallbackContext);
    }
    /* RDBSS expects STATUS_PENDING: completion is reported through the callback */
    return STATUS_PENDING;
}

static NTSTATUS NTAPI Smb2SrvCallWinnerNotify(IN OUT PMRX_SRV_CALL SrvCall, IN BOOLEAN ThisMinirdrIsTheWinner,
                                              IN OUT PVOID SrvCallContext)
{
    PSMB2RDR_SERVER server = SrvCallContext;

    if (ThisMinirdrIsTheWinner)
    {
        SrvCall->Context = server;
    }
    else if (server)
    {
        RxFreePool(server);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2FinalizeSrvCall(PMRX_SRV_CALL SrvCall, BOOLEAN Force)
{
    UNREFERENCED_PARAMETER(Force);
    if (SrvCall->Context)
    {
        RxFreePool(SrvCall->Context);
        SrvCall->Context = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2FinalizeNetRoot(IN OUT PMRX_NET_ROOT NetRoot, IN PBOOLEAN ForceDisconnect)
{
    UNREFERENCED_PARAMETER(NetRoot);
    UNREFERENCED_PARAMETER(ForceDisconnect);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2FinalizeVNetRoot(IN OUT PMRX_V_NET_ROOT VNetRoot, IN PBOOLEAN ForceDisconnect)
{
    PSMB2RDR_VNETROOT_EXT ext = Smb2GetVNetRootExt(VNetRoot);

    UNREFERENCED_PARAMETER(ForceDisconnect);
    if (ext && ext->TreeHandle)
    {
        SMB2RDR_HANDLE_REQ req;

        req.Handle = ext->TreeHandle;
        ext->TreeHandle = 0;
        Smb2Upcall(SMB2RDR_UP_TREE_DISCONNECT, &req, sizeof(req), NULL, 0, NULL);
    }
    return STATUS_SUCCESS;
}

/* The net root of \server\share\path is \share (first component after the server). */
static VOID NTAPI Smb2ExtractNetRootName(IN PUNICODE_STRING FilePathName, IN PMRX_SRV_CALL SrvCall,
                                         OUT PUNICODE_STRING NetRootName, OUT PUNICODE_STRING RestOfName OPTIONAL)
{
    PWCHAR start = FilePathName->Buffer + SrvCall->pSrvCallName->Length / sizeof(WCHAR);
    PWCHAR limit = FilePathName->Buffer + FilePathName->Length / sizeof(WCHAR);
    PWCHAR w = start;

    while (w < limit)
    {
        if (*w == L'\\' && w != start)
            break;
        w++;
    }
    NetRootName->Buffer = start;
    NetRootName->Length = NetRootName->MaximumLength = (USHORT)((PCHAR)w - (PCHAR)start);
    if (RestOfName)
    {
        RestOfName->Buffer = w;
        RestOfName->Length = RestOfName->MaximumLength = (USHORT)((PCHAR)limit - (PCHAR)w);
    }
}

/*
 * MRxCreateVNetRoot: connect to the share. The net root name is "\share"; the server comes from the
 * srv call ("\server"). The daemon returns an opaque tree handle. Completion goes through the callback
 * and the routine must return STATUS_PENDING.
 */
static NTSTATUS NTAPI Smb2CreateVNetRoot(IN OUT PMRX_CREATENETROOT_CONTEXT Context)
{
    PMRX_V_NET_ROOT vnetroot = (PMRX_V_NET_ROOT)Context->pVNetRoot;
    PMRX_NET_ROOT netroot = vnetroot->pNetRoot;
    PMRX_SRV_CALL srvcall = netroot->pSrvCall;
    PSMB2RDR_VNETROOT_EXT ext = Smb2GetVNetRootExt(vnetroot);
    SMB2RDR_TREE_CONNECT_REPLY reply;
    PSMB2RDR_TREE_CONNECT_REQ req = NULL;
    ULONG serverBytes, shareBytes, total, replyLen = 0;
    NTSTATUS status;

    if (netroot->Type != NET_ROOT_DISK && netroot->Type != NET_ROOT_WILD)
    {
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    /* "\server" + "\share" -> "server\share" */
    serverBytes = srvcall->pSrvCallName->Length - sizeof(WCHAR);
    shareBytes = netroot->pNetRootName->Length;
    if (srvcall->pSrvCallName->Length < 2 * sizeof(WCHAR) || shareBytes < 2 * sizeof(WCHAR) ||
        serverBytes + shareBytes > 2 * SMB2RDR_MAX_SERVER_NAME * sizeof(WCHAR))
    {
        status = STATUS_BAD_NETWORK_PATH;
        goto done;
    }
    total = sizeof(*req) + serverBytes + shareBytes;
    req = RxAllocatePoolWithTag(PagedPool, total, SMB2RDR_TAG);
    if (!req)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    RtlZeroMemory(req, sizeof(*req));
    req->NameLength = serverBytes + shareBytes;
    RtlCopyMemory(req + 1, srvcall->pSrvCallName->Buffer + 1, serverBytes);
    RtlCopyMemory((PUCHAR)(req + 1) + serverBytes, netroot->pNetRootName->Buffer, shareBytes);

    status = Smb2Upcall(SMB2RDR_UP_TREE_CONNECT, req, total, &reply, sizeof(reply), &replyLen);
    if (NT_SUCCESS(status) && replyLen != sizeof(reply))
        status = STATUS_INVALID_NETWORK_RESPONSE;
    if (NT_SUCCESS(status))
    {
        ext->TreeHandle = reply.TreeHandle;
        netroot->MRxNetRootState = MRX_NET_ROOT_STATE_GOOD;
        netroot->DeviceType = FILE_DEVICE_DISK;
    }

done:
    if (req)
        RxFreePool(req);
    Context->VirtualNetRootStatus = status;
    Context->NetRootStatus = status;
    Context->Callback(Context);
    /* RDBSS expects STATUS_PENDING on success and failure alike */
    return STATUS_PENDING;
}

/* ---------------- opens ---------------- */

#define SMB2_WRITE_ACCESS_MASK \
    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | DELETE | \
     WRITE_DAC | WRITE_OWNER)

/* Close the daemon's file handle of an open that RDBSS will never see */
static VOID Smb2CloseRemote(ULONGLONG FileHandle)
{
    SMB2RDR_HANDLE_REQ req;

    req.Handle = FileHandle;
    Smb2Upcall(SMB2RDR_UP_CLOSE, &req, sizeof(req), NULL, 0, NULL);
}

/*
 * MRxCreate. Read-only for now: any write, delete or security-write access, or a disposition other
 * than FILE_OPEN, is refused with STATUS_ACCESS_DENIED (as a read-only share would).
 */
static NTSTATUS NTAPI Smb2Create(IN OUT PRX_CONTEXT RxContext)
{
    PNT_CREATE_PARAMETERS params = &RxContext->Create.NtCreateParameters;
    PMRX_SRV_OPEN srvOpen = RxContext->pRelevantSrvOpen;
    PSMB2RDR_VNETROOT_EXT vroot = Smb2GetVNetRootExt(srvOpen->pVNetRoot);
    PMRX_FCB fcb = RxContext->pFcb;
    PUNICODE_STRING name = srvOpen->pAlreadyPrefixedName;
    PSMB2RDR_OPEN_REQ req;
    SMB2RDR_OPEN_REPLY reply;
    PSMB2RDR_FOBX_EXT fobx;
    ULONG replyLen = 0, reqLen, pathBytes;
    PWCHAR path;
    NTSTATUS status;

    if (!vroot || !vroot->TreeHandle)
        return STATUS_CONNECTION_DISCONNECTED;
    if ((params->DesiredAccess & SMB2_WRITE_ACCESS_MASK) || params->Disposition != FILE_OPEN ||
        (params->CreateOptions & (FILE_DELETE_ON_CLOSE | FILE_OPEN_BY_FILE_ID)))
        return STATUS_ACCESS_DENIED;

    /* The name is relative to the net root with a leading backslash; SMB2 wants none ("" is the root). */
    path = name->Buffer;
    pathBytes = name->Length;
    if (pathBytes >= sizeof(WCHAR) && path[0] == L'\\')
    {
        path++;
        pathBytes -= sizeof(WCHAR);
    }
    if (pathBytes > 1024 * sizeof(WCHAR))
        return STATUS_OBJECT_NAME_INVALID;

    reqLen = sizeof(*req) + pathBytes;
    req = RxAllocatePoolWithTag(PagedPool, reqLen, SMB2RDR_TAG);
    if (!req)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(req, sizeof(*req));
    req->TreeHandle = vroot->TreeHandle;
    req->DesiredAccess = params->DesiredAccess;
    req->ShareAccess = params->ShareAccess;
    req->FileAttributes = params->FileAttributes;
    req->Disposition = params->Disposition;
    req->CreateOptions = params->CreateOptions & 0x00FFFFFF;
    req->PathLength = pathBytes;
    RtlCopyMemory(req + 1, path, pathBytes);

    status = Smb2Upcall(SMB2RDR_UP_OPEN, req, reqLen, &reply, sizeof(reply), &replyLen);
    RxFreePool(req);
    if (!NT_SUCCESS(status))
        return status;
    if (replyLen != sizeof(reply))
        return STATUS_INVALID_NETWORK_RESPONSE;

    if (!RxIsFcbAcquiredExclusive(fcb))
        RxAcquireExclusiveFcbResourceInMRx(fcb);

    RxContext->pFobx = RxCreateNetFobx(RxContext, srvOpen);
    if (!RxContext->pFobx)
    {
        Smb2CloseRemote(reply.FileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    fobx = Smb2GetFobxExt(RxContext->pFobx);
    fobx->FileHandle = reply.FileHandle;
    fobx->IsDirectory = (reply.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (fcb->OpenCount == 0)
    {
        FCB_INIT_PACKET initPacket;
        ULONG links = 1;
        ULONG attributes = reply.FileAttributes;
        LARGE_INTEGER creation, access, write, change, alloc, eof;
        RX_FILE_TYPE type = fobx->IsDirectory ? FileTypeDirectory : FileTypeFile;

        creation.QuadPart = reply.CreationTime;
        access.QuadPart = reply.LastAccessTime;
        write.QuadPart = reply.LastWriteTime;
        change.QuadPart = reply.ChangeTime;
        alloc.QuadPart = reply.AllocationSize;
        eof.QuadPart = reply.EndOfFile;
        RxFormInitPacket(initPacket, &attributes, &links, &creation, &access, &write, &change, &alloc, &eof, &eof);
        RxFinishFcbInitialization(fcb, RDBSS_STORAGE_NTC(type), &initPacket);
    }

    /* No client-side caching yet: every read goes to the server */
    srvOpen->BufferingFlags = FCB_STATE_DISABLE_LOCAL_BUFFERING;

    RxContext->Create.ReturnedCreateInformation = reply.CreateAction;
    RxContext->pFobx->OffsetOfNextEaToReturn = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2CloseSrvOpen(IN OUT PRX_CONTEXT RxContext)
{
    PSMB2RDR_FOBX_EXT fobx = Smb2GetFobxExt(RxContext->pFobx);

    if (fobx && fobx->FileHandle)
    {
        Smb2CloseRemote(fobx->FileHandle);
        fobx->FileHandle = 0;
    }
    /* The handle is gone from the user's point of view whatever the network says */
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2CollapseOpen(IN OUT PRX_CONTEXT RxContext)
{
    UNREFERENCED_PARAMETER(RxContext);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS NTAPI Smb2ShouldTryToCollapseThisOpen(IN OUT PRX_CONTEXT RxContext)
{
    return RxContext->pRelevantSrvOpen == NULL ? STATUS_SUCCESS : STATUS_MORE_PROCESSING_REQUIRED;
}

/* ---------------- directory, file and volume information ---------------- */

static NTSTATUS NTAPI Smb2QueryDirectory(IN OUT PRX_CONTEXT RxContext)
{
    FILE_INFORMATION_CLASS infoClass = RxContext->Info.FileInformationClass;
    PSMB2RDR_FOBX_EXT fobx = Smb2GetFobxExt(RxContext->pFobx);
    PUNICODE_STRING filter = &RxContext->pFobx->UnicodeQueryTemplate;
    PSMB2RDR_QUERYDIR_REQ req;
    ULONG reqLen, patternBytes, got = 0, length;
    NTSTATUS status;

    if (!fobx || !fobx->FileHandle || !fobx->IsDirectory)
        return STATUS_INVALID_PARAMETER;
    if (!RxContext->Info.Buffer)
        return STATUS_INVALID_USER_BUFFER;
    switch (infoClass)
    {
        case FileNamesInformation:
        case FileDirectoryInformation:
        case FileFullDirectoryInformation:
        case FileIdFullDirectoryInformation:
        case FileBothDirectoryInformation:
        case FileIdBothDirectoryInformation:
            break;
        default:
            return STATUS_NOT_SUPPORTED;
    }

    length = RxContext->Info.LengthRemaining;
    if (length > SMB2RDR_MAX_IO)
        length = SMB2RDR_MAX_IO;
    patternBytes = (filter && filter->Buffer) ? filter->Length : 0;
    if (patternBytes > 255 * sizeof(WCHAR))
        return STATUS_OBJECT_NAME_INVALID;
    reqLen = sizeof(*req) + patternBytes;
    req = RxAllocatePoolWithTag(PagedPool, reqLen, SMB2RDR_TAG);
    if (!req)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(req, sizeof(*req));
    req->FileHandle = fobx->FileHandle;
    req->InfoClass = (ULONG)infoClass;
    req->OutputLength = length;
    req->PatternLength = patternBytes;
    if (RxContext->QueryDirectory.InitialQuery || RxContext->QueryDirectory.RestartScan)
        req->Flags |= SMB2RDR_QD_RESTART;
    if (RxContext->QueryDirectory.ReturnSingleEntry)
        req->Flags |= SMB2RDR_QD_SINGLE;
    if (patternBytes)
        RtlCopyMemory(req + 1, filter->Buffer, patternBytes);

    status = Smb2Upcall(SMB2RDR_UP_QUERYDIR, req, reqLen, RxContext->Info.Buffer, length, &got);
    RxFreePool(req);
    if (NT_SUCCESS(status))
    {
        RxContext->Info.LengthRemaining -= got;
        return STATUS_SUCCESS;
    }
    return status;
}

static NTSTATUS NTAPI Smb2Read(IN OUT PRX_CONTEXT RxContext)
{
    PLOWIO_CONTEXT low = &RxContext->LowIoContext;
    PSMB2RDR_FOBX_EXT fobx = Smb2GetFobxExt(RxContext->pFobx);
    PMDL mdl = low->ParamsFor.ReadWrite.Buffer;
    ULONG count = low->ParamsFor.ReadWrite.ByteCount, total = 0;
    ULONGLONG offset = low->ParamsFor.ReadWrite.ByteOffset;
    PUCHAR system;
    NTSTATUS status = STATUS_SUCCESS;

    RxContext->IoStatusBlock.Information = 0;
    if (!fobx || !fobx->FileHandle || fobx->IsDirectory)
        return STATUS_INVALID_DEVICE_REQUEST;
    if (!count)
        return STATUS_SUCCESS;
    if (!mdl)
        return STATUS_INVALID_USER_BUFFER;
    system = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
    if (!system)
        return STATUS_INSUFFICIENT_RESOURCES;

    while (total < count)
    {
        SMB2RDR_READ_REQ req;
        ULONG chunk = count - total, got = 0;

        if (chunk > SMB2RDR_MAX_IO)
            chunk = SMB2RDR_MAX_IO;
        RtlZeroMemory(&req, sizeof(req));
        req.FileHandle = fobx->FileHandle;
        req.Offset = offset + total;
        req.Length = chunk;
        status = Smb2Upcall(SMB2RDR_UP_READ, &req, sizeof(req), system + total, chunk, &got);
        if (!NT_SUCCESS(status))
            break;
        total += got;
        if (got < chunk)
            break;                      /* short read: end of file */
    }
    if (total)
        status = STATUS_SUCCESS;        /* data already delivered; a later error or EOF shows on the next read */
    else if (status == STATUS_SUCCESS)
        status = STATUS_END_OF_FILE;
    RxContext->IoStatusBlock.Information = total;
    return status;
}

/* Basic and standard information come from what the open reported and RDBSS cached in the FCB. */
static NTSTATUS NTAPI Smb2QueryFileInfo(IN OUT PRX_CONTEXT RxContext)
{
    PFCB fcb = (PFCB)RxContext->pFcb;
    PVOID buffer = RxContext->Info.Buffer;
    LONG remaining = RxContext->Info.LengthRemaining;

    if (!buffer)
        return STATUS_INVALID_USER_BUFFER;
    switch (RxContext->Info.FileInformationClass)
    {
        case FileBasicInformation:
        {
            PFILE_BASIC_INFORMATION info = buffer;

            if (remaining < (LONG)sizeof(*info))
                return STATUS_BUFFER_OVERFLOW;
            info->CreationTime = fcb->CreationTime;
            info->LastAccessTime = fcb->LastAccessTime;
            info->LastWriteTime = fcb->LastWriteTime;
            info->ChangeTime = fcb->LastChangeTime;
            info->FileAttributes = fcb->Attributes;
            RxContext->Info.LengthRemaining -= sizeof(*info);
            return STATUS_SUCCESS;
        }
        case FileStandardInformation:
        {
            PFILE_STANDARD_INFORMATION info = buffer;

            if (remaining < (LONG)sizeof(*info))
                return STATUS_BUFFER_OVERFLOW;
            info->AllocationSize = fcb->Header.AllocationSize;
            info->EndOfFile = fcb->Header.FileSize;
            info->NumberOfLinks = fcb->NumberOfLinks;
            info->DeletePending = FALSE;
            info->Directory = (fcb->Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            RxContext->Info.LengthRemaining -= sizeof(*info);
            return STATUS_SUCCESS;
        }
        default:
            return STATUS_NOT_SUPPORTED;
    }
}

/* Volume identity is synthetic for now (a later step queries the server). */
static NTSTATUS NTAPI Smb2QueryVolumeInfo(IN OUT PRX_CONTEXT RxContext)
{
    static const WCHAR label[] = L"SMB2";
    PVOID buffer = RxContext->Info.Buffer;
    LONG remaining = RxContext->Info.LengthRemaining;

    if (!buffer)
        return STATUS_INVALID_USER_BUFFER;
    switch (RxContext->Info.FsInformationClass)
    {
        case FileFsVolumeInformation:
        {
            PFILE_FS_VOLUME_INFORMATION info = buffer;
            ULONG need = FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + sizeof(label) - sizeof(WCHAR);

            if (remaining < (LONG)need)
                return STATUS_BUFFER_OVERFLOW;
            RtlZeroMemory(info, need);
            info->VolumeSerialNumber = 0x534D4232;
            info->VolumeLabelLength = sizeof(label) - sizeof(WCHAR);
            RtlCopyMemory(info->VolumeLabel, label, sizeof(label) - sizeof(WCHAR));
            RxContext->Info.LengthRemaining -= need;
            return STATUS_SUCCESS;
        }
        case FileFsAttributeInformation:
        {
            PFILE_FS_ATTRIBUTE_INFORMATION info = buffer;
            ULONG need = FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + sizeof(label) - sizeof(WCHAR);

            if (remaining < (LONG)need)
                return STATUS_BUFFER_OVERFLOW;
            RtlZeroMemory(info, need);
            info->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_READ_ONLY_VOLUME;
            info->MaximumComponentNameLength = 255;
            info->FileSystemNameLength = sizeof(label) - sizeof(WCHAR);
            RtlCopyMemory(info->FileSystemName, label, sizeof(label) - sizeof(WCHAR));
            RxContext->Info.LengthRemaining -= need;
            return STATUS_SUCCESS;
        }
        default:
            return STATUS_NOT_SUPPORTED;
    }
}

/* ---------------- everything else: not yet ---------------- */

static NTSTATUS NTAPI Smb2NotImplemented(PRX_CONTEXT RxContext)
{
    UNREFERENCED_PARAMETER(RxContext);
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS NTAPI Smb2AreFilesAliased(PFCB A, PFCB B)
{
    UNREFERENCED_PARAMETER(A);
    UNREFERENCED_PARAMETER(B);
    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS NTAPI Smb2IsValidDirectory(IN OUT PRX_CONTEXT RxContext, IN PUNICODE_STRING DirectoryName)
{
    UNREFERENCED_PARAMETER(RxContext);
    UNREFERENCED_PARAMETER(DirectoryName);
    return STATUS_NOT_IMPLEMENTED;
}

/* Nothing is cached client-side, so there is nothing to flush */
static NTSTATUS NTAPI Smb2Flush(IN OUT PRX_CONTEXT RxContext)
{
    UNREFERENCED_PARAMETER(RxContext);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2DeallocateForFcb(IN OUT PMRX_FCB Fcb)
{
    UNREFERENCED_PARAMETER(Fcb);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2DeallocateForFobx(IN OUT PMRX_FOBX Fobx)
{
    UNREFERENCED_PARAMETER(Fobx);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2IsLockRealizable(IN OUT PMRX_FCB Fcb, IN PLARGE_INTEGER ByteOffset,
                                           IN PLARGE_INTEGER Length, IN ULONG LowIoLockFlags)
{
    UNREFERENCED_PARAMETER(Fcb);
    UNREFERENCED_PARAMETER(ByteOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(LowIoLockFlags);
    return STATUS_NOT_IMPLEMENTED;
}

static ULONG NTAPI Smb2ExtendFile(IN OUT PRX_CONTEXT RxContext, IN OUT PLARGE_INTEGER NewFileSize,
                                  OUT PLARGE_INTEGER NewAllocationSize)
{
    UNREFERENCED_PARAMETER(RxContext);
    UNREFERENCED_PARAMETER(NewFileSize);
    UNREFERENCED_PARAMETER(NewAllocationSize);
    return (ULONG)STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS NTAPI Smb2CompleteBufferingStateChange(IN OUT PRX_CONTEXT RxContext, IN OUT PMRX_SRV_OPEN SrvOpen,
                                                       IN PVOID MRxContext)
{
    UNREFERENCED_PARAMETER(RxContext);
    UNREFERENCED_PARAMETER(SrvOpen);
    UNREFERENCED_PARAMETER(MRxContext);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI Smb2ComputeNewBufferingState(IN OUT PMRX_SRV_OPEN SrvOpen, IN PVOID MRxContext,
                                                   OUT PULONG NewBufferingState)
{
    UNREFERENCED_PARAMETER(SrvOpen);
    UNREFERENCED_PARAMETER(MRxContext);
    *NewBufferingState = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS Smb2InitOps(VOID)
{
    ZeroAndInitializeNodeType(&Smb2RdrOps, RDBSS_NTC_MINIRDR_DISPATCH, sizeof(MINIRDR_DISPATCH));

    Smb2RdrOps.MRxFlags = RDBSS_MANAGE_V_NET_ROOT_EXTENSION | RDBSS_MANAGE_FOBX_EXTENSION;
    Smb2RdrOps.MRxSrvCallSize = 0;
    Smb2RdrOps.MRxNetRootSize = 0;
    Smb2RdrOps.MRxVNetRootSize = sizeof(SMB2RDR_VNETROOT_EXT);
    Smb2RdrOps.MRxFcbSize = 0;
    Smb2RdrOps.MRxFobxSize = sizeof(SMB2RDR_FOBX_EXT);
    Smb2RdrOps.MRxCancel = NULL;

    Smb2RdrOps.MRxStart = Smb2RdrStart;
    Smb2RdrOps.MRxStop = Smb2RdrStop;
    Smb2RdrOps.MRxDevFcbXXXControlFile = Smb2RdrDevFcbXXXControlFile;

    Smb2RdrOps.MRxCreateSrvCall = Smb2CreateSrvCall;
    Smb2RdrOps.MRxSrvCallWinnerNotify = Smb2SrvCallWinnerNotify;
    Smb2RdrOps.MRxCreateVNetRoot = Smb2CreateVNetRoot;
    Smb2RdrOps.MRxExtractNetRootName = Smb2ExtractNetRootName;
    Smb2RdrOps.MRxFinalizeSrvCall = Smb2FinalizeSrvCall;
    Smb2RdrOps.MRxFinalizeNetRoot = Smb2FinalizeNetRoot;
    Smb2RdrOps.MRxFinalizeVNetRoot = Smb2FinalizeVNetRoot;

    Smb2RdrOps.MRxCreate = Smb2Create;
    Smb2RdrOps.MRxCollapseOpen = Smb2CollapseOpen;
    Smb2RdrOps.MRxShouldTryToCollapseThisOpen = Smb2ShouldTryToCollapseThisOpen;
    Smb2RdrOps.MRxExtendForCache = Smb2ExtendFile;
    Smb2RdrOps.MRxExtendForNonCache = Smb2ExtendFile;
    Smb2RdrOps.MRxCloseSrvOpen = Smb2CloseSrvOpen;
    Smb2RdrOps.MRxFlush = Smb2Flush;
    Smb2RdrOps.MRxDeallocateForFcb = Smb2DeallocateForFcb;
    Smb2RdrOps.MRxDeallocateForFobx = Smb2DeallocateForFobx;
    Smb2RdrOps.MRxIsLockRealizable = Smb2IsLockRealizable;
    Smb2RdrOps.MRxQueryDirectory = Smb2QueryDirectory;
    Smb2RdrOps.MRxQueryVolumeInfo = Smb2QueryVolumeInfo;
    Smb2RdrOps.MRxQueryEaInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxSetEaInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxQuerySdInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxSetSdInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxQueryFileInfo = Smb2QueryFileInfo;
    Smb2RdrOps.MRxSetFileInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxComputeNewBufferingState = Smb2ComputeNewBufferingState;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_READ] = Smb2Read;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_WRITE] = Smb2NotImplemented;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_SHAREDLOCK] = Smb2NotImplemented;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_EXCLUSIVELOCK] = Smb2NotImplemented;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_UNLOCK] = Smb2NotImplemented;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_UNLOCK_MULTIPLE] = Smb2NotImplemented;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_FSCTL] = Smb2NotImplemented;
    Smb2RdrOps.MRxCompleteBufferingStateChangeRequest = Smb2CompleteBufferingStateChange;
    Smb2RdrOps.MRxIsValidDirectory = Smb2IsValidDirectory;
    Smb2RdrOps.MRxTruncate = Smb2NotImplemented;
    Smb2RdrOps.MRxZeroExtend = Smb2NotImplemented;
    Smb2RdrOps.MRxAreFilesAliased = Smb2AreFilesAliased;
    Smb2RdrOps.MRxQueryQuotaInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxSetQuotaInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxSetVolumeInfo = Smb2NotImplemented;
    return STATUS_SUCCESS;
}

/* ---------------- driver entry / unload ---------------- */

NTSTATUS NTAPI Smb2RdrFsdDispatch(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
    if (DeviceObject != (PDEVICE_OBJECT)Smb2RdrDevice)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return RxFsdDispatch((PRDBSS_DEVICE_OBJECT)DeviceObject, Irp);
}

VOID NTAPI Smb2RdrUnload(IN PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING link;
    PRX_CONTEXT rxContext;

    Smb2UpcallShutdown();
    rxContext = RxCreateRxContext(NULL, Smb2RdrDevice, RX_CONTEXT_FLAG_IN_FSP);
    if (rxContext)
    {
        RxStopMinirdr(rxContext, &rxContext->PostRequest);
        RxDereferenceAndDeleteRxContext(rxContext);
    }
    RtlInitUnicodeString(&link, SMB2RDR_SHADOW_DEVICE_NAME);
    IoDeleteSymbolicLink(&link);
    RxUnload(DriverObject);
    Smb2Trace("unloaded\n");
}

NTSTATUS NTAPI DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING devName, linkName;
    PSMB2RDR_DEVICE_EXTENSION ext;
    ULONG i;

    status = RxDriverEntry(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status))
    {
        Smb2Trace("RxDriverEntry failed: %08lx\n", status);
        return status;
    }

    Smb2UpcallInit();
    status = Smb2InitOps();
    if (!NT_SUCCESS(status))
        return status;

    RtlInitUnicodeString(&devName, SMB2RDR_DEVICE_NAME);
    status = RxRegisterMinirdr(&Smb2RdrDevice, DriverObject, &Smb2RdrOps,
                               RX_REGISTERMINI_FLAG_DONT_PROVIDE_MAILSLOTS, &devName,
                               sizeof(SMB2RDR_DEVICE_EXTENSION), FILE_DEVICE_NETWORK_FILE_SYSTEM,
                               FILE_REMOTE_DEVICE);
    if (!NT_SUCCESS(status))
    {
        Smb2Trace("RxRegisterMinirdr failed: %08lx\n", status);
        return status;
    }

    ext = (PSMB2RDR_DEVICE_EXTENSION)((PUCHAR)Smb2RdrDevice + sizeof(RDBSS_DEVICE_OBJECT));
    RxDefineNode(ext, SMB2RDR_DEVICE_EXTENSION);
    ext->DeviceObject = Smb2RdrDevice;

    RtlInitUnicodeString(&linkName, SMB2RDR_SHADOW_DEVICE_NAME);
    status = IoCreateSymbolicLink(&linkName, &devName);
    if (!NT_SUCCESS(status))
    {
        Smb2Trace("IoCreateSymbolicLink failed: %08lx\n", status);
        RxUnregisterMinirdr(Smb2RdrDevice);
        return status;
    }

    DriverObject->DriverUnload = Smb2RdrUnload;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        DriverObject->MajorFunction[i] = Smb2RdrFsdDispatch;

    Smb2Trace("loaded\n");
    return STATUS_SUCCESS;
}
