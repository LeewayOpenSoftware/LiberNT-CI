/*
 * lnrspoc.sys - Leeway experimental Rust kernel-driver PoC host (v1).
 *
 * A minimal WDM driver whose only job is to exercise the Rust staticlib
 * (leeway_rs_poc) across a C ABI. EXPERIMENTAL, off by default, never placed on
 * installation media. ntoskrnl is not touched.
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

#define LNRSPOC_DEVICE_NAME L"\Device\Lnrspoc"
#define LNRSPOC_DOS_NAME    L"\DosDevices\Lnrspoc"

/* METHOD_BUFFERED IOCTLs. 0x800+ is the customer/driver-defined function range. */
#define IOCTL_LNRSPOC_VALIDATE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#ifdef LNRSPOC_TEST_IOCTLS
#define IOCTL_LNRSPOC_FORCE_PANIC \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static PDEVICE_OBJECT g_DeviceObject = NULL;

static VOID
LnrspocSelfTest(VOID)
{
    /* Known-good record: length prefix 4 (LE), payload "test". Must return 0 and
     * never fault - this runs on every load and must stay on the non-bugcheck path. */
    unsigned char rec[8] = { 0x04, 0x00, 0x00, 0x00, 't', 'e', 's', 't' };
    unsigned long crc = 0;
    int rc = leeway_rs_poc_validate(rec, sizeof(rec), &crc);
    DbgPrint("lnrspoc: self-test validate rc=%d crc=0x%08lx\n", rc, crc);
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
    DbgPrint("lnrspoc: loaded (Rust PoC v1)\n");
    return STATUS_SUCCESS;
}
