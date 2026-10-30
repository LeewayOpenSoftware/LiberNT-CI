#pragma once

#include <karray.h>

namespace Rtl
{
    class KRTL_CLASS KBitmap
    {
    public:

        KBitmap() = default;
        ~KBitmap() = default;

        bool Initialize(size_t numberOfBits)
        {
            size_t bitsPerMachineWord = 64;
            if (!m_storage.resize((numberOfBits + bitsPerMachineWord - 1) / bitsPerMachineWord))
                return false;

            RtlInitializeBitMapEx(&m_bitmap, &m_storage[0], numberOfBits);
            RtlClearAllBitsEx(&m_bitmap);
            return true;
        }

        NONPAGED
        bool TestBit(size_t bitNumber)
        {
            return RtlCheckBitEx(&m_bitmap, bitNumber);
        }

        NONPAGED
        void SetBit(size_t bitNumber)
        {
            RtlSetBitEx(&m_bitmap, bitNumber);
        }

        NONPAGED
        void ClearBit(size_t bitNumber)
        {
            RtlClearBitEx(&m_bitmap, bitNumber);
        }

        size_t FindSetBits(size_t numberToFind, size_t hintIndex)
        {
            return RtlFindSetBitsEx(&m_bitmap, numberToFind, hintIndex);
        }

        size_t FindSetBitsAndClear(size_t numberToFind, size_t hintIndex)
        {
            return RtlFindSetBitsAndClearEx(&m_bitmap, numberToFind, hintIndex);
        }

    private:
        RTL_BITMAP_EX m_bitmap;
        Rtl::KArray<ULONG64, NonPagedPoolNx> m_storage;
    };

}
