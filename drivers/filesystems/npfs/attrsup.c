/*
 * PROJECT:     LiberNT Named Pipe FileSystem
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Named attributes of pipes, connections and handles
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "npfs.h"

#define NPFS_ATTRIBUTE_TAG 'aFpN'

static
PNP_ATTRIBUTE
NpFindAttribute(IN PLIST_ENTRY List,
                IN PCSTR Name,
                IN ULONG NameLength)
{
    PLIST_ENTRY Entry;
    PNP_ATTRIBUTE Attribute;
    PAGED_CODE();

    for (Entry = List->Flink; Entry != List; Entry = Entry->Flink)
    {
        Attribute = CONTAINING_RECORD(Entry, NP_ATTRIBUTE, Links);
        if (Attribute->NameLength == NameLength && RtlEqualMemory(Attribute->Data, Name, NameLength))
            return Attribute;
    }
    return NULL;
}

NTSTATUS
NTAPI
NpParseAttributeName(IN PIRP Irp,
                     OUT PCSTR *Name,
                     OUT PULONG NameLength)
{
    PIO_STACK_LOCATION IoStack;
    PCSTR Buffer;
    ULONG Length;

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Buffer = Irp->AssociatedIrp.SystemBuffer;
    Length = IoStack->Parameters.FileSystemControl.InputBufferLength;
    if (!Buffer || !Length)
        return STATUS_INVALID_PARAMETER;

    for (*NameLength = 0; *NameLength < Length && Buffer[*NameLength]; (*NameLength)++);
    if (*NameLength == Length)
        return STATUS_INVALID_PARAMETER;

    *Name = Buffer;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NpSetAttribute(IN PLIST_ENTRY List,
               IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PNP_ATTRIBUTE Attribute, OldAttribute;
    PCSTR Name;
    ULONG NameLength, ValueLength, Length;
    NTSTATUS Status;
    PAGED_CODE();

    Status = NpParseAttributeName(Irp, &Name, &NameLength);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!NameLength)
        return STATUS_INVALID_PARAMETER;

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    Length = IoStack->Parameters.FileSystemControl.InputBufferLength;
    ValueLength = Length - NameLength - 1;
    OldAttribute = NpFindAttribute(List, Name, NameLength);

    if (!ValueLength)
    {
        if (OldAttribute)
        {
            RemoveEntryList(&OldAttribute->Links);
            ExFreePoolWithTag(OldAttribute, NPFS_ATTRIBUTE_TAG);
        }
        return STATUS_SUCCESS;
    }

    if (Length > MAXULONG - FIELD_OFFSET(NP_ATTRIBUTE, Data))
        return STATUS_INSUFFICIENT_RESOURCES;

    Attribute = ExAllocatePoolWithQuotaTag(PagedPool | POOL_QUOTA_FAIL_INSTEAD_OF_RAISE,
                                           FIELD_OFFSET(NP_ATTRIBUTE, Data) + Length,
                                           NPFS_ATTRIBUTE_TAG);
    if (!Attribute)
        return STATUS_INSUFFICIENT_RESOURCES;

    Attribute->NameLength = NameLength;
    Attribute->ValueLength = ValueLength;
    RtlCopyMemory(Attribute->Data, Name, Length);

    if (OldAttribute)
    {
        RemoveEntryList(&OldAttribute->Links);
        ExFreePoolWithTag(OldAttribute, NPFS_ATTRIBUTE_TAG);
    }
    InsertTailList(List, &Attribute->Links);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NpGetAttribute(IN PLIST_ENTRY List,
               IN PCSTR Name,
               IN ULONG NameLength,
               IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PNP_ATTRIBUTE Attribute;
    PAGED_CODE();

    Attribute = NpFindAttribute(List, Name, NameLength);
    if (!Attribute)
        return STATUS_NOT_FOUND;

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    if (IoStack->Parameters.FileSystemControl.OutputBufferLength < Attribute->ValueLength)
        return STATUS_BUFFER_TOO_SMALL;

    RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,
                  Attribute->Data + Attribute->NameLength + 1,
                  Attribute->ValueLength);
    Irp->IoStatus.Information = Attribute->ValueLength;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NpGetBuiltInAttribute(IN ULONG Value,
                      IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStack;
    PAGED_CODE();

    IoStack = IoGetCurrentIrpStackLocation(Irp);
    if (IoStack->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG))
        return STATUS_BUFFER_TOO_SMALL;

    *(PULONG)Irp->AssociatedIrp.SystemBuffer = Value;
    Irp->IoStatus.Information = sizeof(ULONG);
    return STATUS_SUCCESS;
}

VOID
NTAPI
NpFreeAttributes(IN PLIST_ENTRY List)
{
    PLIST_ENTRY Entry;
    PAGED_CODE();

    while (!IsListEmpty(List))
    {
        Entry = RemoveHeadList(List);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, NP_ATTRIBUTE, Links), NPFS_ATTRIBUTE_TAG);
    }
}
