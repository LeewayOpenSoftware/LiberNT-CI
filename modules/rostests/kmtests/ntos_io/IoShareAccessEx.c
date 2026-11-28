/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite share access Ex and link share access
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <kmt_test.h>

#define NDEBUG
#include <debug.h>

#define RW_SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

static
VOID
TestShareAccessEx(VOID)
{
    SHARE_ACCESS Share;
    FILE_OBJECT First, Second, Third;
    BOOLEAN NoWrite = FALSE;
    BOOLEAN CanWrite = TRUE;
    NTSTATUS Status;

    RtlZeroMemory(&Share, sizeof(Share));
    RtlZeroMemory(&First, sizeof(First));
    RtlZeroMemory(&Second, sizeof(Second));
    RtlZeroMemory(&Third, sizeof(Third));

    IoSetShareAccessEx(FILE_READ_DATA, FILE_SHARE_READ, &First, &Share, &CanWrite);
    ok_eq_ulong(Share.OpenCount, 1UL);
    ok_eq_ulong(Share.Readers, 1UL);
    ok_eq_ulong(Share.SharedWrite, 0UL);
    ok_eq_uint(First.SharedWrite, FALSE);

    Status = IoCheckShareAccessEx(FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &Second, &Share, TRUE, NULL);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);
    ok_eq_ulong(Share.OpenCount, 1UL);

    IoRemoveShareAccess(&First, &Share);
    ok_eq_ulong(Share.OpenCount, 0UL);
    ok_eq_ulong(Share.Readers, 0UL);

    /* Without write permission, denying write sharing is not honoured */
    IoSetShareAccessEx(FILE_READ_DATA, FILE_SHARE_READ, &First, &Share, &NoWrite);
    ok_eq_ulong(Share.SharedWrite, 1UL);
    ok_eq_uint(First.SharedWrite, TRUE);

    Status = IoCheckShareAccessEx(FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &Second, &Share, TRUE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Share.OpenCount, 2UL);
    ok_eq_ulong(Share.Writers, 1UL);

    Status = IoCheckShareAccessEx(FILE_READ_DATA, FILE_SHARE_READ, &Third, &Share, FALSE, &NoWrite);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(Third.SharedWrite, TRUE);
    ok_eq_ulong(Share.OpenCount, 2UL);

    Status = IoCheckShareAccessEx(FILE_READ_DATA, FILE_SHARE_READ, &Third, &Share, FALSE, &CanWrite);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);

    IoRemoveShareAccess(&Second, &Share);
    IoRemoveShareAccess(&First, &Share);
    ok_eq_ulong(Share.OpenCount, 0UL);
    ok_eq_ulong(Share.Writers, 0UL);
    ok_eq_ulong(Share.SharedWrite, 0UL);
}

static
VOID
TestLinkShareAccess(VOID)
{
    SHARE_ACCESS Share;
    LINK_SHARE_ACCESS Link1, Link2;
    FILE_OBJECT First, Second, Third;
    NTSTATUS Status;

    RtlZeroMemory(&First, sizeof(First));
    RtlZeroMemory(&Second, sizeof(Second));
    RtlZeroMemory(&Third, sizeof(Third));

    /* A delete open through link 1 that does not share delete */
    IoSetLinkShareAccess(DELETE | FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &First, &Share, &Link1, 0);
    ok_eq_ulong(Share.OpenCount, 1UL);
    ok_eq_ulong(Share.Readers, 1UL);
    ok_eq_ulong(Share.Deleters, 0UL);
    ok_eq_ulong(Link1.OpenCount, 1UL);
    ok_eq_ulong(Link1.Deleters, 1UL);
    ok_eq_ulong(Link1.SharedDelete, 0UL);
    RtlZeroMemory(&Link2, sizeof(Link2));

    /* Another open through link 1 conflicts on delete */
    Status = IoCheckLinkShareAccess(FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &Second, &Share, &Link1,
                                    IO_CHECK_SHARE_ACCESS_UPDATE_SHARE_ACCESS);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);
    ok_eq_ulong(Share.OpenCount, 1UL);

    /* ...unless delete is not checked */
    Status = IoCheckLinkShareAccess(FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &Second, &Share, &Link1,
                                    IO_CHECK_SHARE_ACCESS_DONT_CHECK_DELETE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Share.OpenCount, 1UL);

    /* Link 2 has its own delete sharing */
    Status = IoCheckLinkShareAccess(DELETE | FILE_READ_DATA, FILE_SHARE_READ, &Second, &Share, &Link2,
                                    IO_CHECK_SHARE_ACCESS_UPDATE_SHARE_ACCESS);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Share.OpenCount, 2UL);
    ok_eq_ulong(Share.Readers, 2UL);
    ok_eq_ulong(Share.Deleters, 0UL);
    ok_eq_ulong(Link2.OpenCount, 1UL);
    ok_eq_ulong(Link2.Deleters, 1UL);

    /* The read/write sharing stays per stream: Second did not share write */
    Status = IoCheckLinkShareAccess(FILE_WRITE_DATA, RW_SHARE_ALL, &Third, &Share, NULL, 0);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);

    /* Without the update flag, the file object is still updated */
    Status = IoCheckLinkShareAccess(FILE_READ_DATA, FILE_SHARE_READ, &Third, &Share, &Link2,
                                    IO_CHECK_SHARE_ACCESS_DONT_CHECK_DELETE);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(Third.ReadAccess, TRUE);
    ok_eq_uint(Third.SharedRead, TRUE);
    ok_eq_ulong(Share.OpenCount, 2UL);

    /* DONT_UPDATE_FILE_OBJECT leaves it alone */
    RtlZeroMemory(&Third, sizeof(Third));
    Status = IoCheckLinkShareAccess(FILE_READ_DATA, FILE_SHARE_READ, &Third, &Share, &Link2,
                                    IO_CHECK_SHARE_ACCESS_DONT_CHECK_DELETE |
                                    IO_CHECK_SHARE_ACCESS_DONT_UPDATE_FILE_OBJECT);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(Third.ReadAccess, FALSE);
    ok_eq_uint(Third.SharedRead, FALSE);

    /* FORCE_USING_SCB checks delete against the stream, which has no deleter */
    Status = IoCheckLinkShareAccess(FILE_READ_DATA, FILE_SHARE_READ, &Third, &Share, &Link1,
                                    IO_CHECK_SHARE_ACCESS_FORCE_USING_SCB);
    ok_eq_hex(Status, STATUS_SUCCESS);

    IoRemoveLinkShareAccess(&Second, &Share, &Link2);
    ok_eq_ulong(Share.OpenCount, 1UL);
    ok_eq_ulong(Link2.OpenCount, 0UL);
    ok_eq_ulong(Link2.Deleters, 0UL);

    IoUpdateLinkShareAccess(&Second, &Share, &Link2);
    ok_eq_ulong(Share.OpenCount, 2UL);
    ok_eq_ulong(Link2.OpenCount, 1UL);
    ok_eq_ulong(Link2.Deleters, 1UL);

    IoRemoveLinkShareAccessEx(&Second, &Share, &Link2, 0);
    IoRemoveLinkShareAccessEx(&First, &Share, &Link1, 0);
    ok_eq_ulong(Share.OpenCount, 0UL);
    ok_eq_ulong(Share.Readers, 0UL);
    ok_eq_ulong(Share.SharedRead, 0UL);
    ok_eq_ulong(Link1.OpenCount, 0UL);
    ok_eq_ulong(Link1.Deleters, 0UL);
    ok_eq_ulong(Link2.OpenCount, 0UL);

    /* FORCE_USING_SCB keeps the delete counts in the stream */
    IoSetLinkShareAccess(DELETE, 0, &First, &Share, &Link1, IO_CHECK_SHARE_ACCESS_FORCE_USING_SCB);
    ok_eq_ulong(Share.Deleters, 1UL);
    ok_eq_ulong(Link1.OpenCount, 0UL);
    IoUpdateLinkShareAccessEx(&First, &Share, &Link1, IO_CHECK_SHARE_ACCESS_FORCE_USING_SCB);
    ok_eq_ulong(Share.Deleters, 2UL);
    IoRemoveLinkShareAccessEx(&First, &Share, &Link1, IO_CHECK_SHARE_ACCESS_FORCE_USING_SCB);
    IoRemoveLinkShareAccessEx(&First, &Share, &Link1, IO_CHECK_SHARE_ACCESS_FORCE_USING_SCB);
    ok_eq_ulong(Share.OpenCount, 0UL);
    ok_eq_ulong(Share.Deleters, 0UL);
    ok_eq_ulong(Link1.OpenCount, 0UL);
}

static
VOID
TestIgnoreSharing(VOID)
{
    SHARE_ACCESS Share;
    FILE_OBJECT Owner, Ignoring;
    NTSTATUS Status;

    RtlZeroMemory(&Owner, sizeof(Owner));
    RtlZeroMemory(&Ignoring, sizeof(Ignoring));

    IoSetShareAccess(FILE_READ_DATA | FILE_WRITE_DATA, 0, &Owner, &Share);
    ok_eq_uint(IoIsFileObjectIgnoringSharing(&Ignoring), FALSE);

    Status = IoCheckShareAccessEx(FILE_READ_DATA, FILE_SHARE_READ, &Ignoring, &Share, TRUE, NULL);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);

    Status = IoSetFileObjectIgnoreSharing(&Ignoring);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_uint(IoIsFileObjectIgnoringSharing(&Ignoring), TRUE);
    if (!NT_SUCCESS(Status))
        return;

    Status = IoCheckShareAccessEx(FILE_READ_DATA, FILE_SHARE_READ, &Ignoring, &Share, TRUE, NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Share.OpenCount, 1UL);

    Status = IoCheckLinkShareAccess(FILE_READ_DATA, FILE_SHARE_READ, &Ignoring, &Share, NULL,
                                    IO_CHECK_SHARE_ACCESS_FORCE_CHECK);
    ok_eq_hex(Status, STATUS_SHARING_VIOLATION);

    IoUpdateLinkShareAccess(&Ignoring, &Share, NULL);
    ok_eq_ulong(Share.OpenCount, 1UL);
    IoRemoveLinkShareAccess(&Ignoring, &Share, NULL);
    ok_eq_ulong(Share.OpenCount, 1UL);

    IoRemoveShareAccess(&Owner, &Share);
    ok_eq_ulong(Share.OpenCount, 0UL);

    /* The extension of a file object that is not from the object manager is pool */
    ExFreePool(Ignoring.FileObjectExtension);
}

static
VOID
TestAllocateIrpEx(VOID)
{
    PIRP Irp;

    Irp = IoAllocateIrpEx(NULL, 3, FALSE);
    ok(Irp != NULL, "IoAllocateIrpEx failed\n");
    if (Irp == NULL)
        return;

    ok_eq_int(Irp->StackCount, 3);
    ok_eq_int(Irp->CurrentLocation, 4);
    IoFreeIrp(Irp);
}

START_TEST(IoShareAccessEx)
{
    TestShareAccessEx();
    TestLinkShareAccess();
    TestIgnoreSharing();
    TestAllocateIrpEx();
}
