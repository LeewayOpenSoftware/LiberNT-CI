/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     NTFS filesystem driver
 * COPYRIGHT:   Copyright 2024 Carl Bialorucki <carl.bialorucki@reactos.org>
 *              Copyright 2024 Justin Miller <justin.miller@reactos.org>
 */

#include "ntfslib_new.h"
#include "ntfslib_new_internal.h"

NTSTATUS
Volume::GetVolumeLabel(_Inout_ PWSTR   VolumeLabel,
                       _Inout_ PUSHORT Length)
{
    NTSTATUS Status;
    PFileRecord VolumeFile;
    PAttribute VolumeNameAttr;
    ULONG BytesRemaining, LabelLength;

    if (!VolumeLabel || !Length)
        return STATUS_INVALID_PARAMETER;

    if (!VolumeLabelCached)
    {
        Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeName,
                                                           NULL,
                                                           _Volume,
                                                           &VolumeFile,
                                                           &VolumeNameAttr);
        if (!NT_SUCCESS(Status))
            return Status;

        LabelLength = GetAttributeDataSize(VolumeNameAttr);
        if (LabelLength > sizeof(VolumeLabelCache))
        {
            delete VolumeFile;
            return STATUS_FILE_CORRUPT_ERROR;
        }

        BytesRemaining = LabelLength;
        Status = LabelLength == 0
                 ? STATUS_SUCCESS
                 : VolumeFile->CopyData(VolumeNameAttr,
                                        (PUCHAR)VolumeLabelCache,
                                        &BytesRemaining);
        delete VolumeFile;
        if (!NT_SUCCESS(Status) || BytesRemaining != 0)
            return NT_SUCCESS(Status) ? STATUS_END_OF_FILE : Status;

        VolumeLabelCacheLength = (USHORT)LabelLength;
        VolumeLabelCached = TRUE;
    }

    *Length = VolumeLabelCacheLength;
    RtlCopyMemory(VolumeLabel, VolumeLabelCache, VolumeLabelCacheLength);
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::SetVolumeLabel(_In_ PWSTR VolumeLabel,
                       _In_ ULONG Length)
{
    NTSTATUS Status;
    FileRecord* VolumeFile;
    PAttribute VolumeNameAttr;

    if ((!VolumeLabel && Length != 0) ||
        Length > sizeof(VolumeLabelCache))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsReadOnly)
        return STATUS_ACCESS_DENIED;

    /* Allocate memory for $Volume file record, retrieve the file record, and
     * get a pointer to the $VOLUME_NAME attribute.
     */
    Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeName,
                                                       NULL,
                                                       _Volume,
                                                       &VolumeFile,
                                                       &VolumeNameAttr);

    if (!NT_SUCCESS(Status))
        return Status;

    // Replace and repack the complete resident $VOLUME_NAME value.
    Status = VolumeFile->ReplaceResidentData(
        VolumeNameAttr,
        reinterpret_cast<const UCHAR*>(VolumeLabel),
        Length);

    if (NT_SUCCESS(Status))
    {
        // Write the volume file to disk.
        Status = MFT->WriteFileRecordToMFT(VolumeFile);
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(VolumeLabelCache, VolumeLabel, Length);
            VolumeLabelCacheLength = (USHORT)Length;
            VolumeLabelCached = TRUE;
        }
    }

    delete VolumeFile;
    return Status;
}

NTSTATUS
Volume::QueryVolumeFlags(_Out_ PUSHORT Flags)
{
    NTSTATUS Status;
    PFileRecord VolumeFile;
    PAttribute InfoAttr;
    VolumeInformationEx Info;
    ULONG BytesRemaining;

    if (!Flags)
        return STATUS_INVALID_PARAMETER;

    Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeInformation,
                                                       NULL,
                                                       _Volume,
                                                       &VolumeFile,
                                                       &InfoAttr);
    if (!NT_SUCCESS(Status))
        return Status;

    if (InfoAttr->IsNonResident ||
        GetAttributeDataSize(InfoAttr) < FIELD_OFFSET(VolumeInformationEx, Reserved2))
    {
        delete VolumeFile;
        return STATUS_FILE_CORRUPT_ERROR;
    }

    BytesRemaining = FIELD_OFFSET(VolumeInformationEx, Reserved2);
    Status = VolumeFile->CopyData(InfoAttr, reinterpret_cast<PUCHAR>(&Info), &BytesRemaining);
    delete VolumeFile;
    if (!NT_SUCCESS(Status))
        return Status;
    if (BytesRemaining != 0)
        return STATUS_END_OF_FILE;

    *Flags = Info.Flags;
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::SetVolumeFlags(_In_ USHORT Set,
                       _In_ USHORT Clear)
{
    NTSTATUS Status;
    FileRecord* VolumeFile;
    PAttribute InfoAttr;
    UCHAR Data[sizeof(VolumeInformationEx) + 64];
    PVolumeInformationEx Info = reinterpret_cast<PVolumeInformationEx>(Data);
    ULONG Length, BytesRemaining;

    if (IsReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;

    /* The record write below would mark the volume dirty under this stale copy */
    if (DirtyTracking && !DirtyMarked)
    {
        Status = MarkDirtyBeforeWrite();
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = MFT->GetFileAttributeFromFileRecordNumber(TypeVolumeInformation,
                                                       NULL,
                                                       _Volume,
                                                       &VolumeFile,
                                                       &InfoAttr);
    if (!NT_SUCCESS(Status))
        return Status;

    Length = (ULONG)GetAttributeDataSize(InfoAttr);
    if (InfoAttr->IsNonResident || Length < FIELD_OFFSET(VolumeInformationEx, Reserved2) ||
        Length > sizeof(Data))
    {
        delete VolumeFile;
        return STATUS_FILE_CORRUPT_ERROR;
    }

    BytesRemaining = Length;
    Status = VolumeFile->CopyData(InfoAttr, Data, &BytesRemaining);
    if (NT_SUCCESS(Status) && BytesRemaining != 0)
        Status = STATUS_END_OF_FILE;
    if (NT_SUCCESS(Status) && (Info->Flags & (Set | Clear)) != Set)
    {
        Info->Flags = (USHORT)((Info->Flags & ~Clear) | Set);
        Status = VolumeFile->ReplaceResidentData(InfoAttr, Data, Length);
        if (NT_SUCCESS(Status))
            Status = MFT->WriteFileRecordToMFT(VolumeFile);
        if (NT_SUCCESS(Status))
            Status = FlushVolume();
    }

    delete VolumeFile;
    return Status;
}


/*
 * Volume-dirty lifecycle (crash consistency). A read-write session marks the
 * volume dirty before its first metadata write lands, so a crash leaves a
 * volume the next boot checks; a clean dismount clears it. A flag that was
 * already set at mount, or that a user requested via FSCTL, is retained for
 * chkdsk and never auto-cleared. Writes are serialized by the caller's
 * metadata lock, so the flag cannot be overtaken by a second writer.
 */
NTSTATUS
Volume::EnableDirtyTracking()
{
    NTSTATUS Status;
    USHORT Flags;

    if (IsReadOnly)
        return STATUS_SUCCESS;

    Status = QueryVolumeFlags(&Flags);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Dirty already: an earlier crash or a chkdsk request. Keep it for chkdsk. */
    DirtyRetained = (Flags & VOL_DIRTY) ? TRUE : FALSE;
    DirtyMarked = DirtyRetained;
    DirtyTracking = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::MarkDirtyBeforeWrite()
{
    NTSTATUS Status;

    /* A noted corruption keeps the flag this write is about to set. */
    if (DirtyTracking && CorruptionNoted)
        DirtyRetained = TRUE;
    if (!DirtyTracking || DirtyMarked || IsReadOnly)
        return STATUS_SUCCESS;

    if (ShutdownCleaned)
        DPRINT1("NTFS: volume re-dirtied after shutdown clean\n");

    /* Set first: the flag's own record write comes back through WriteVolume. */
    DirtyMarked = TRUE;
    Status = SetVolumeFlags(VOL_DIRTY, 0);
    if (NT_SUCCESS(Status))
        Status = FlushVolume();
    if (!NT_SUCCESS(Status))
    {
        DirtyMarked = FALSE;
        NoteLostWrite(Status);
    }
    return Status;
}

NTSTATUS
Volume::MarkClean()
{
    NTSTATUS Status;

    /*
     * A read met corrupt structure during this session: leave the volume
     * dirty, even if nothing was written since mount (Windows marks the
     * volume dirty when it raises a corruption status), so the next boot
     * runs the check.
     */
    if (DirtyTracking && CorruptionNoted && !IsReadOnly)
        return CommitCorruptionDirty();

    if (!DirtyTracking || !DirtyMarked || DirtyRetained || IsReadOnly)
        return STATUS_SUCCESS;

    /* Everything else reaches the medium before the flag says it has. */
    Status = FlushVolume();
    if (NT_SUCCESS(Status))
        Status = SetVolumeFlags(0, VOL_DIRTY);
    if (NT_SUCCESS(Status))
        Status = FlushVolume();
    if (NT_SUCCESS(Status))
        DirtyMarked = FALSE;
    return Status;
}

/*
 * Make a noted corruption durable now: the volume flag says dirty on the
 * medium and is retained, so a crash before the next write or the dismount
 * still leaves a volume the next boot checks. Caller holds the metadata lock.
 */
NTSTATUS
Volume::CommitCorruptionDirty()
{
    NTSTATUS Status;

    if (!DirtyTracking || !CorruptionNoted || IsReadOnly)
        return STATUS_SUCCESS;

    /* Under the lock DirtyMarked means the dirty flag reached the medium:
     * MarkDirtyBeforeWrite clears it again when its write or flush fails. */
    if (DirtyMarked)
    {
        DirtyRetained = TRUE;
        return STATUS_SUCCESS;
    }

    DirtyMarked = TRUE;
    Status = SetVolumeFlags(VOL_DIRTY, 0);
    if (NT_SUCCESS(Status))
        Status = FlushVolume();
    if (!NT_SUCCESS(Status))
    {
        DirtyMarked = FALSE;
        return Status;
    }
    DirtyRetained = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
Volume::CommitCorruptionDirtyOnce()
{
    /* Cheap when nothing is pending: this runs on every outermost release of
     * the metadata lock. One attempt per mount; on failure MarkClean tries
     * again at dismount. */
    if (!CorruptionNoted || CorruptionCommitTried)
        return STATUS_SUCCESS;
    CorruptionCommitTried = TRUE;
    return CommitCorruptionDirty();
}

void
Volume::NoteCorruption()
{
    if (!CorruptionNoted)
    {
        DPRINT1("NTFS: corrupt on-disk structure found; the volume stays dirty\n");
        CorruptionNoted = TRUE;
    }
}

void
Volume::NoteLostWrite(_In_ NTSTATUS Status)
{
    LostWriteNoted = TRUE;
    if (!CorruptionNoted)
    {
        DPRINT1("NTFS: a metadata write failed (status %lx); the volume stays dirty\n", Status);
        CorruptionNoted = TRUE;
    }
}

BOOLEAN
Volume::ReportsDirty(_In_ USHORT Flags)
{
    if (CorruptionNoted)
        return TRUE;
    if (!(Flags & VOL_DIRTY))
        return FALSE;
    /* The flag a read-write mount sets for itself is not a check request */
    return !DirtyTracking || DirtyRetained;
}

void
Volume::RetainDirty(_In_ BOOLEAN Retain)
{
    if (!DirtyTracking)
        return;
    DirtyRetained = Retain;
    DirtyMarked = Retain;
}

NTSTATUS
Volume::MarkCleanForShutdown()
{
    NTSTATUS Status = MarkClean();

    /* After a clean shutdown, any further write is a true re-dirty worth noting;
     * no write-through (that is the owner-restricted J30 mechanism). */
    if (NT_SUCCESS(Status) && DirtyTracking && !IsReadOnly)
        ShutdownCleaned = TRUE;
    return Status;
}

/*
 * The checker locked the volume, read it whole and wrote nothing: the medium
 * is consistent and this instance saw the same medium, so the flag the volume
 * was mounted with (retained, so MarkClean leaves it) and a note from a read
 * that met a structure the check accepts can go. The flag goes through this
 * instance, so its own lifecycle agrees with the medium afterwards, which a
 * raw clear under it would not give.
 *
 * Not after a lost write: then this instance holds an update the medium
 * never got, a clean medium says nothing about it, and later writes would
 * build on the lost one with no dirty flag to show for it. The checker is
 * refused and dismounts, which drops this instance.
 */
NTSTATUS
Volume::MarkCleanAfterCheck()
{
    NTSTATUS Status;
    USHORT Flags = 0;

    if (IsReadOnly)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if (!DirtyTracking)
        return STATUS_NOT_SUPPORTED;
    if (LostWriteNoted)
        return STATUS_LOST_WRITEBEHIND_DATA;

    Status = FlushVolume();
    if (NT_SUCCESS(Status))
        Status = QueryVolumeFlags(&Flags);
    if (NT_SUCCESS(Status) && (Flags & (VOL_DIRTY | VOL_FULL_CHKDSK | VOL_ONLINE_SCAN)))
    {
        Status = SetVolumeFlags(0, VOL_DIRTY | VOL_FULL_CHKDSK | VOL_ONLINE_SCAN);
        if (NT_SUCCESS(Status))
            Status = FlushVolume();
    }
    if (NT_SUCCESS(Status))
    {
        DirtyMarked = FALSE;
        DirtyRetained = FALSE;
        CorruptionNoted = FALSE;
        CorruptionCommitTried = FALSE;
    }
    return Status;
}
