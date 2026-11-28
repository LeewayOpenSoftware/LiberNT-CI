/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     IoGetIoPriorityHint / IoSetIoPriorityHint read and write the
 *              IRP's own priority hint instead of a constant
 */

#include <kmt_test.h>

/* The IRP must carry the hint itself: a constant return value would pass
 * neither the per-value round trip nor the invalid-value rejection below. */
START_TEST(IoPriorityHint)
{
    PIRP Irp;
    IO_PRIORITY_HINT Hint;
    ULONG PsIoPriority = (ULONG)-1;
    ULONG PublicFlags;
    NTSTATUS Status;
    ULONG Value;

    Irp = IoAllocateIrp(1, FALSE);
    if (skip(Irp != NULL, "IoAllocateIrp failed\n"))
        return;

    /* Without an explicit hint the IRP inherits the issuing thread's I/O
     * priority. Compare against what Ps reports for this very thread, and
     * against the documented default (Normal) for a system thread. */
    Status = ZwQueryInformationThread(ZwCurrentThread(), ThreadIoPriority,
                                      &PsIoPriority, sizeof(PsIoPriority),
                                      NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(PsIoPriority, (ULONG)IoPriorityNormal);

    Hint = IoGetIoPriorityHint(Irp);
    ok_eq_int(Hint, IoPriorityNormal);
    ok_eq_ulong((ULONG)Hint, PsIoPriority);

    /* Same answer once the IRP is tied to the current thread */
    Irp->Tail.Overlay.Thread = PsGetCurrentThread();
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityNormal);

    /* An explicit hint must not disturb the public IRP flags */
    Irp->Flags = IRP_NOCACHE | IRP_PAGING_IO;
    Status = IoSetIoPriorityHint(Irp, IoPriorityLow);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityLow);
    PublicFlags = Irp->Flags & 0xFFFF;
    ok_eq_hex(PublicFlags, IRP_NOCACHE | IRP_PAGING_IO);

    /* Every valid hint round-trips, including VeryLow (0) which a "no hint
     * stored" implementation could not distinguish from unset */
    for (Value = IoPriorityVeryLow; Value < MaxIoPriorityTypes; Value++)
    {
        Status = IoSetIoPriorityHint(Irp, (IO_PRIORITY_HINT)Value);
        ok(Status == STATUS_SUCCESS, "IoSetIoPriorityHint(%lu) = 0x%lx\n", Value, Status);
        Hint = IoGetIoPriorityHint(Irp);
        ok(Hint == (IO_PRIORITY_HINT)Value, "IoGetIoPriorityHint = %d, expected %lu\n", Hint, Value);
    }

    /* Out-of-range hints are rejected and leave the stored hint alone */
    Status = IoSetIoPriorityHint(Irp, MaxIoPriorityTypes);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityCritical);
    Status = IoSetIoPriorityHint(Irp, (IO_PRIORITY_HINT)0xFFFFFFFF);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityCritical);
    PublicFlags = Irp->Flags & 0xFFFF;
    ok_eq_hex(PublicFlags, IRP_NOCACHE | IRP_PAGING_IO);

    /* Reusing the IRP drops the explicit hint: back to the thread default */
    IoReuseIrp(Irp, STATUS_SUCCESS);
    ok_eq_int(IoGetIoPriorityHint(Irp), IoPriorityNormal);
    ok_eq_hex(Irp->Flags, 0);

    IoFreeIrp(Irp);
}
