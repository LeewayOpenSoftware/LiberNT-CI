// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

// This class is used to copy memory from user-mode pointer into kernel-allocated buffer.
// It handles the allocation and freeing of the buffer automatically, as well as probing the user-mode buffer

template<typename T>
class UserModeMemoryCopy {

public:

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    CheckAndCopy(
        _In_ T* UserModeBuffer,
        _In_ SIZE_T Size
    )
    {
        _SEH2_TRY
        {
            ProbeForRead(
                UserModeBuffer,
                Size,
                sizeof(UCHAR));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
        }
        _SEH2_END;

        if (Size == 0)
        {
            return STATUS_INVALID_PARAMETER;
        }

        auto bufferCopy = MakeSizedPoolPtrNP<T>('pcmu', Size);
        if (!bufferCopy)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        _SEH2_TRY
        {
            RtlCopyMemory(
                bufferCopy.get(),
                UserModeBuffer,
                Size);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
        }
        _SEH2_END;

        m_allocatedBuffer = wistd::move(bufferCopy);
        return STATUS_SUCCESS;
    }

    _IRQL_requires_(PASSIVE_LEVEL)
    T *Get(
        void
    ) const noexcept
    {
        return m_allocatedBuffer.get();
    }

private:

    KPoolPtrNP<T>
        m_allocatedBuffer;
};
