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
/* R1b (D-2): name-grafting validator. Input = [ULONG reparse_tag][reparse buffer bytes];
 * output = the LONG NTSTATUS from leeway_rs_poc_validate_reparse_graft. Never bugchecks. */
#define IOCTL_LNRSPOC_VALIDATE_GRAFT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

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
    case IOCTL_LNRSPOC_VALIDATE_GRAFT:
    {
        /* METHOD_BUFFERED: input = [ULONG reparse_tag][reparse buffer...]; output = LONG
         * NTSTATUS. The validator borrows the buffer, never retains it, never bugchecks. */
        unsigned char *buf = (unsigned char *)Irp->AssociatedIrp.SystemBuffer;
        ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
        ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
        unsigned long tag;
        long nt;

        if (buf == NULL || outLen < sizeof(long) || inLen < sizeof(unsigned long))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        tag = *(unsigned long *)buf;
        nt = leeway_rs_poc_validate_reparse_graft(buf + sizeof(unsigned long),
                                                  inLen - sizeof(unsigned long), tag);
        DbgPrint("lnrspoc: validate_graft tag=0x%lx len=%lu -> 0x%lx\n",
                 tag, (unsigned long)(inLen - sizeof(unsigned long)), (unsigned long)nt);
        *(long *)Irp->AssociatedIrp.SystemBuffer = nt;
        info = sizeof(long);
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
