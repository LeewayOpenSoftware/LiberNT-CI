/*
 * lnrspoc.sys - Leeway experimental Rust kernel-driver PoC host (v2).
 *
 * A minimal WDM driver whose only job is to exercise the Rust staticlib
 * (leeway_rs_poc) across a C ABI. EXPERIMENTAL, off by default, never placed on
 * installation media. ntoskrnl is not touched. v2 adds pool-allocator and SpinLock
 * exercisers (IOCTLs 0x802-0x805), all on the non-bugcheck path.
 *
 * Normal operation never bugchecks: DriverEntry runs a self-test on a known-good
 * record and logs the result; IOCTL_LNRSPOC_VALIDATE forwards a caller buffer to the
 * Rust validator and returns its status + CRC. The controlled bugcheck path is
 * compiled in ONLY under -DLNRSPOC_TEST_IOCTLS (matching the staticlib's
 * `test-ioctls` feature) and is reachable solely through an explicit test IOCTL,
 * intended for a disposable VM.
 */
#include <ntddk.h>
#include "leeway_rs_poc.h"

#define LNRSPOC_DEVICE_NAME L"\\Device\\Lnrspoc"
#define LNRSPOC_DOS_NAME    L"\\DosDevices\\Lnrspoc"

/* METHOD_BUFFERED IOCTLs. 0x800+ is the customer/driver-defined function range. */
#define IOCTL_LNRSPOC_VALIDATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#ifdef LNRSPOC_TEST_IOCTLS
#define IOCTL_LNRSPOC_FORCE_PANIC \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
/* v2 exercisers (always present; safe, non-destructive). Input (where used): one ULONG
 * iteration count. Output: one LONG result code (0 = PASS, negative = failure). */
#define IOCTL_LNRSPOC_ALLOC_STRESS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LNRSPOC_LOCK_STRESS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LNRSPOC_OOM_PROBE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LNRSPOC_V2_SELFTEST \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* R1b (D-2): the swap-equivalent mount-point validator. Input = the raw REPARSE_DATA_BUFFER bytes;
 * output = the LONG NTSTATUS from leeway_rs_poc_validate_mount_point. Never bugchecks. */
#define IOCTL_LNRSPOC_VALIDATE_MP \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* R-1 item (1): exercise the no-heap Rust validators + the SpinLock AT DISPATCH_LEVEL from a
 * DPC, proving the IRQL-agnostic/no-heap claims on the real DISPATCH path. Output = LONG rc
 * (0 = all checks passed at DISPATCH). Never bugchecks. */
#define IOCTL_LNRSPOC_DISPATCH_SELFTEST \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* R-1 item (2): in-kernel OOM assertion. Exhaust the nonpaged pool via the Rust fallible
 * allocator and assert the failure was a RETURNED Err (a positive block count), not a crash.
 * Output = LONG rc (0 = graceful OOM confirmed). Never bugchecks. Runs at PASSIVE. */
#define IOCTL_LNRSPOC_OOM_ASSERT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

static PDEVICE_OBJECT g_DeviceObject = NULL;

static VOID
LnrspocSelfTest(VOID)
{
    /* Known-good record: length prefix 4 (LE), payload "test". Must return 0 and
     * never fault - this runs on every load and must stay on the non-bugcheck path. */
    unsigned char rec[8] = { 0x04, 0x00, 0x00, 0x00, 't', 'e', 's', 't' };
    unsigned long crc = 0;
    int rc = leeway_rs_poc_validate(rec, sizeof(rec), &crc);
    int v2;
    DbgPrint("lnrspoc: self-test validate rc=%d crc=0x%08lx\n", rc, crc);

    /* v2 self-test: exercise the pool allocator and SpinLock once at load. This stays
     * on the non-bugcheck path (allocation/lock failure is returned, never trapped). */
    v2 = leeway_rs_poc_v2_selftest();
    DbgPrint("lnrspoc: v2 self-test (pool+spinlock) rc=%d\n", v2);
}

static NTSTATUS
LnrspocCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* R-1 item (1): DISPATCH_LEVEL self-test context + DPC. The context is nonpaged (touched at
 * DISPATCH). The DPC runs the no-heap validators and the SpinLock exerciser at DISPATCH and
 * records a result code; 0 means every check passed and we were genuinely at DISPATCH_LEVEL. */
typedef struct _LNRSPOC_DISPATCH_CTX
{
    KEVENT Done;
    KDPC Dpc;
    int Result;        /* 0 = pass; negative = failing stage */
    KIRQL IrqlSeen;    /* must be DISPATCH_LEVEL */
    unsigned char good_mp[24];
    unsigned char bad_mp[24];
    unsigned char rec[8];
} LNRSPOC_DISPATCH_CTX, *PLNRSPOC_DISPATCH_CTX;

static VOID
LnrspocDispatchDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    PLNRSPOC_DISPATCH_CTX c = (PLNRSPOC_DISPATCH_CTX)Context;
    unsigned long crc = 0;
    int rc = 0;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    /* Prove we are genuinely at DISPATCH_LEVEL (a DPC runs there). */
    c->IrqlSeen = KeGetCurrentIrql();
    if (c->IrqlSeen != DISPATCH_LEVEL)
        rc = -1;

    /* No-heap mount-point validator at DISPATCH: good must pass, bad must be rejected. */
    if (leeway_rs_poc_validate_mount_point(c->good_mp, (unsigned int)sizeof c->good_mp) != 0)
        rc = rc ? rc : -2;
    if (leeway_rs_poc_validate_mount_point(c->bad_mp, (unsigned int)sizeof c->bad_mp)
        != (int)0xC0000278)
        rc = rc ? rc : -3;

    /* No-heap v1 CRC validator at DISPATCH. */
    if (leeway_rs_poc_validate(c->rec, (unsigned long)sizeof c->rec, &crc) != 0)
        rc = rc ? rc : -4;

    /* SpinLock acquire/release at DISPATCH (the IRQL path; raise-to-DPC from DISPATCH is a
     * no-op, release restores DISPATCH). lock_stress is no-heap (stack SpinLock). */
    if (leeway_rs_poc_lock_stress(2000) != 0)
        rc = rc ? rc : -5;

    c->Result = rc;
    KeSetEvent(&c->Done, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS
LnrspocDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (sp->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_LNRSPOC_VALIDATE:
    {
        /* METHOD_BUFFERED: SystemBuffer holds input on entry, output on exit. */
        unsigned char *buf = (unsigned char *)Irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
        ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
        unsigned long crc = 0;
        int rc;

        if (buf == NULL || outLen < sizeof(unsigned long))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        rc = leeway_rs_poc_validate(buf, inLen, &crc);
        if (rc == 0)
        {
            *(unsigned long *)Irp->AssociatedIrp.SystemBuffer = crc;
            info = sizeof(unsigned long);
            status = STATUS_SUCCESS;
        }
        else
        {
            /* Expected, non-fatal: malformed input. Map to a status, never fault. */
            status = STATUS_DATA_ERROR;
        }
        break;
    }
    case IOCTL_LNRSPOC_ALLOC_STRESS:
    case IOCTL_LNRSPOC_LOCK_STRESS:
    case IOCTL_LNRSPOC_OOM_PROBE:
    case IOCTL_LNRSPOC_V2_SELFTEST:
    {
        /* All four return a LONG result in the output buffer (0 = PASS). The two stress
         * IOCTLs read a ULONG iteration count from the input buffer. None can bugcheck. */
        unsigned long *in = (unsigned long *)Irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
        ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
        ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
        unsigned long iters = 0;
        int rc;

        if (Irp->AssociatedIrp.SystemBuffer == NULL || outLen < sizeof(int))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (code == IOCTL_LNRSPOC_ALLOC_STRESS || code == IOCTL_LNRSPOC_LOCK_STRESS)
        {
            if (inLen < sizeof(unsigned long))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            iters = *in;
        }

        if (code == IOCTL_LNRSPOC_ALLOC_STRESS)
            rc = leeway_rs_poc_alloc_stress(iters);
        else if (code == IOCTL_LNRSPOC_LOCK_STRESS)
            rc = leeway_rs_poc_lock_stress(iters);
        else if (code == IOCTL_LNRSPOC_OOM_PROBE)
            rc = leeway_rs_poc_oom_probe();
        else
            rc = leeway_rs_poc_v2_selftest();

        DbgPrint("lnrspoc: v2 ioctl 0x%lx -> rc=%d\n", code, rc);
        *(int *)Irp->AssociatedIrp.SystemBuffer = rc;
        info = sizeof(int);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_LNRSPOC_VALIDATE_MP:
    {
        /* METHOD_BUFFERED: input = raw REPARSE_DATA_BUFFER bytes; output = LONG NTSTATUS.
         * The swap-equivalent of IopValidateMountPointBuffer. Never bugchecks. */
        unsigned char *buf = (unsigned char *)Irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
        ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
        long nt;

        if (buf == NULL || outLen < sizeof(long))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        nt = leeway_rs_poc_validate_mount_point(buf, inLen);
        DbgPrint("lnrspoc: validate_mp len=%lu -> 0x%lx\n", (unsigned long)inLen, (unsigned long)nt);
        *(long *)Irp->AssociatedIrp.SystemBuffer = nt;
        info = sizeof(long);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_LNRSPOC_OOM_ASSERT:
    {
        /* R-1 item (2): assert OOM is a returned Err, not a crash. leeway_rs_poc_oom_probe
         * exhausts the nonpaged pool through the Rust fallible allocator until try_alloc
         * returns Err, frees every block, and returns the count taken (>0) or a negative
         * code. Reaching this handler at all proves no bugcheck; rc=0 iff the count is >0. */
        int blocks, rc;
        ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;

        if (Irp->AssociatedIrp.SystemBuffer == NULL || outLen < sizeof(int))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        blocks = leeway_rs_poc_oom_probe();
        rc = (blocks > 0) ? 0 : -1;
        DbgPrint("lnrspoc: oom_assert blocks=%d rc=%d\n", blocks, rc);
        *(int *)Irp->AssociatedIrp.SystemBuffer = rc;
        info = sizeof(int);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_LNRSPOC_DISPATCH_SELFTEST:
    {
        /* R-1 item (1): run the no-heap validators + SpinLock at DISPATCH via a DPC.
         * Output = LONG rc (0 = pass). This handler runs at PASSIVE. The context lives on the
         * kernel stack (nonpaged) and is alive for the whole synchronous wait, so no pool is
         * needed; the DPC touches it at DISPATCH and signals Done before the wait returns. */
        LNRSPOC_DISPATCH_CTX c;
        ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;

        if (Irp->AssociatedIrp.SystemBuffer == NULL || outLen < sizeof(int))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(&c, sizeof c);
        KeInitializeEvent(&c.Done, NotificationEvent, FALSE);
        KeInitializeDpc(&c.Dpc, LnrspocDispatchDpc, &c);

        /* good mount point: tag MOUNT_POINT, ReparseDataLength 16 (8 field hdr + 8 path),
         * SubstituteNameOffset 0, SubstituteNameLength 4, PrintName 0/0, first WCHAR '\'. */
        c.good_mp[0] = 0x03; c.good_mp[1] = 0x00; c.good_mp[2] = 0x00; c.good_mp[3] = 0xA0;
        c.good_mp[4] = 0x10; c.good_mp[5] = 0x00;           /* ReparseDataLength = 16 */
        c.good_mp[10] = 0x04;                                /* SubstituteNameLength = 4 */
        c.good_mp[16] = 0x5C; c.good_mp[17] = 0x00;          /* L'\\' */
        /* bad: identical but the substitute's first WCHAR is '/', not '\' -> INVALID. */
        RtlCopyMemory(c.bad_mp, c.good_mp, sizeof c.bad_mp);
        c.bad_mp[16] = 0x2F;
        /* v1 CRC record: [len=4][A B C D]. */
        c.rec[0] = 0x04; c.rec[4] = 'A'; c.rec[5] = 'B'; c.rec[6] = 'C'; c.rec[7] = 'D';

        KeInsertQueueDpc(&c.Dpc, NULL, NULL);
        KeWaitForSingleObject(&c.Done, Executive, KernelMode, FALSE, NULL);
        DbgPrint("lnrspoc: dispatch_selftest irql=%u rc=%d\n", c.IrqlSeen, c.Result);

        *(int *)Irp->AssociatedIrp.SystemBuffer = c.Result;
        info = sizeof(int);
        status = STATUS_SUCCESS;
        break;
    }
#ifdef LNRSPOC_TEST_IOCTLS
    case IOCTL_LNRSPOC_FORCE_PANIC:
        /* Destructive test ONLY: brings down the box via the controlled bugcheck.
         * Reachable solely here, only in a test build, only in a disposable VM. */
        DbgPrint("lnrspoc: FORCE_PANIC requested - invoking controlled bugcheck\n");
        leeway_rs_poc_force_panic(); /* does not return */
        break;
#endif
    default:
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static VOID
LnrspocUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dos;
    UNREFERENCED_PARAMETER(DriverObject);
    RtlInitUnicodeString(&dos, LNRSPOC_DOS_NAME);
    IoDeleteSymbolicLink(&dos);
    if (g_DeviceObject)
        IoDeleteDevice(g_DeviceObject);
    DbgPrint("lnrspoc: unloaded\n");
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING devName, dosName;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&devName, LNRSPOC_DEVICE_NAME);
    status = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN, FALSE, &g_DeviceObject);
    if (!NT_SUCCESS(status))
        return status;

    RtlInitUnicodeString(&dosName, LNRSPOC_DOS_NAME);
    status = IoCreateSymbolicLink(&dosName, &devName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
        return status;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = LnrspocCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = LnrspocCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = LnrspocDeviceControl;
    DriverObject->DriverUnload = LnrspocUnload;

    LnrspocSelfTest();
    DbgPrint("lnrspoc: loaded (Rust PoC v2: pool allocator + SpinLock)\n");
    return STATUS_SUCCESS;
}
