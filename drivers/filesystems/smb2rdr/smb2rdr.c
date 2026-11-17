/*
 * PROJECT:     LiberNT SMB2 client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SMB2 mini-redirector: driver entry, RDBSS registration, daemon control
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * The driver holds no protocol code: every network operation is an upcall to smb2d (see upcall.c).
 * This file is the skeleton: it registers with Mup through RDBSS, takes the control IOCTLs from the
 * daemon and resolves server names (SrvCall) through the daemon. File operations follow in later steps;
 * until then every file callback fails with STATUS_NOT_IMPLEMENTED.
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
            if (RxContext->RxDeviceObject->NumberOfActiveFcbs > 0)
                return STATUS_REDIRECTOR_HAS_OPEN_HANDLES;
            Smb2UpcallShutdown();
            status = RxStopMinirdr(RxContext, &RxContext->PostRequest);
            if (status == STATUS_PENDING && RxContext->PostRequest == TRUE)
                status = STATUS_MORE_PROCESSING_REQUIRED;
            return status;

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
    UNREFERENCED_PARAMETER(VNetRoot);
    UNREFERENCED_PARAMETER(ForceDisconnect);
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

static NTSTATUS NTAPI Smb2CreateVNetRoot(IN OUT PMRX_CREATENETROOT_CONTEXT Context)
{
    /* Tree connect arrives with the file operations step. */
    UNREFERENCED_PARAMETER(Context);
    return STATUS_NOT_IMPLEMENTED;
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

    Smb2RdrOps.MRxFlags = 0;
    Smb2RdrOps.MRxSrvCallSize = 0;
    Smb2RdrOps.MRxNetRootSize = 0;
    Smb2RdrOps.MRxVNetRootSize = 0;
    Smb2RdrOps.MRxFcbSize = 0;
    Smb2RdrOps.MRxFobxSize = 0;
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

    Smb2RdrOps.MRxCreate = Smb2NotImplemented;
    Smb2RdrOps.MRxCollapseOpen = Smb2NotImplemented;
    Smb2RdrOps.MRxShouldTryToCollapseThisOpen = Smb2NotImplemented;
    Smb2RdrOps.MRxExtendForCache = Smb2ExtendFile;
    Smb2RdrOps.MRxExtendForNonCache = Smb2ExtendFile;
    Smb2RdrOps.MRxCloseSrvOpen = Smb2NotImplemented;
    Smb2RdrOps.MRxFlush = Smb2NotImplemented;
    Smb2RdrOps.MRxDeallocateForFcb = Smb2DeallocateForFcb;
    Smb2RdrOps.MRxDeallocateForFobx = Smb2DeallocateForFobx;
    Smb2RdrOps.MRxIsLockRealizable = Smb2IsLockRealizable;
    Smb2RdrOps.MRxQueryDirectory = Smb2NotImplemented;
    Smb2RdrOps.MRxQueryVolumeInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxQueryEaInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxSetEaInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxQuerySdInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxSetSdInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxQueryFileInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxSetFileInfo = Smb2NotImplemented;
    Smb2RdrOps.MRxComputeNewBufferingState = Smb2ComputeNewBufferingState;
    Smb2RdrOps.MRxLowIOSubmit[LOWIO_OP_READ] = Smb2NotImplemented;
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
