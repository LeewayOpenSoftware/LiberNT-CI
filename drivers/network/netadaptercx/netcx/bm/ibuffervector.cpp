// Copyright (C) Microsoft Corporation. All rights reserved.
#include "bmprecomp.hpp"

#include "ibuffervector.hpp"
#include "IBufferVector.tmh"

_Use_decl_annotations_
IBufferVector::IBufferVector(
    size_t BufferCount,
    size_t BufferOffset,
    size_t BufferCapacity
)
    : m_BufferCount(BufferCount)
    , m_BufferOffset(BufferOffset)
    , m_BufferCapacity(BufferCapacity)
{
}

_Use_decl_annotations_
NTSTATUS
IBufferVector::Initialize(
    void
)
{
    CX_RETURN_IF_NOT_NT_SUCCESS(
        CreateMdls());

    CX_RETURN_IF_NOT_NT_SUCCESS(
        MapToUserModeAddressSpace());

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
IBufferVector::CreateMdls(
    void
)
{
    CX_RETURN_NTSTATUS_IF(
        STATUS_INSUFFICIENT_RESOURCES,
        !m_mdl.resize(m_BufferCount));

    for (ULONG i = 0; i < m_BufferCount; i++)
    {
        void *kernelVirtualAddress = GetKernelModeVirtualAddress(i);

        auto const mdlSize = MmSizeOfMdl(
            kernelVirtualAddress,
            m_BufferCapacity);

        auto mdl = MakeSizedPoolPtrNP<MDL>(BUFFER_MANAGER_POOL_TAG, mdlSize);

        if (!mdl)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        MmInitializeMdl(mdl.get(), kernelVirtualAddress, m_BufferCapacity);
        MmBuildMdlForNonPagedPool(mdl.get());

        m_mdl[i] = wistd::move(mdl);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
IBufferVector::MapToUserModeAddressSpace(
    void
)
{
#if NETCX_REDIRECTOR
    CX_RETURN_NTSTATUS_IF(
        STATUS_INSUFFICIENT_RESOURCES,
        !m_userModeAddress.resize(m_BufferCount));

    for (size_t i = 0; i < m_BufferCount; i++)
    {
        MDL *mdl = m_mdl[i].get();

        if (mdl == nullptr)
        {
            return STATUS_INVALID_PARAMETER;
        }

        void *umVirtualAddress = nullptr;

        _SEH2_TRY
        {
            umVirtualAddress = MmMapLockedPagesSpecifyCache(
                mdl,
                UserMode,
                MmCached,
                nullptr,
                FALSE,
                NormalPagePriority | MdlMappingNoExecute);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            break;
        }
        _SEH2_END;

        if (umVirtualAddress == nullptr)
        {
            break;
        }

        m_userModeAddress[i] = umVirtualAddress;
    }
#endif

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void
IBufferVector::UnmapFromUserModeAddressSpace(
    void
)
{
#if NETCX_REDIRECTOR
    for (size_t i = 0; i < m_BufferCount; i++)
    {
        if (m_userModeAddress[i] != nullptr)
        {
            MmUnmapLockedPages(
                m_userModeAddress[i],
                m_mdl[i].get());

            m_userModeAddress[i] = nullptr;
        }
    }
#endif
}

_Use_decl_annotations_
void
IBufferVector::GetEntryByIndex(
    _In_ size_t Index,
    _Out_ NET_DATA_HEADER * NetDataHeader
) const
{
    NetDataHeader->KmVirtualAddress = GetKernelModeVirtualAddress(Index);
#if NETCX_REDIRECTOR
    NetDataHeader->UmVirtualAddress = m_userModeAddress[Index];
#endif
    NetDataHeader->LogicalAddress = GetLogicalAddress(Index);
    NetDataHeader->Index = static_cast<UINT32>(Index);
    NetDataHeader->Mdl = m_mdl[Index].get();
}
