/*
 * PROJECT:     LiberNT AMD64 emulation host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Argument widths of the native system services
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define EMU_ARGUMENT_BITS   3
#define EMU_ARGUMENT_MASK   7
#define EMU_ARGUMENT_NATIVE 0
#define EMU_ARGUMENT_INT32  1
#define EMU_ARGUMENT_INT16  2
#define EMU_ARGUMENT_INT8   3
#define EMU_ARGUMENT_UINT16 4
#define EMU_ARGUMENT_UINT8  5

#ifdef __cplusplus
#include <type_traits>

struct EMU_UNTYPED_SERVICE
{
};

template <typename Type>
constexpr ULONG64 EmuArgumentKind()
{
    static_assert(sizeof(Type) <= sizeof(ULONG64));
    if (sizeof(Type) == sizeof(ULONG64)) return EMU_ARGUMENT_NATIVE;
    if (sizeof(Type) == sizeof(ULONG)) return EMU_ARGUMENT_INT32;
    if (sizeof(Type) == sizeof(USHORT)) return std::is_signed<Type>::value ? EMU_ARGUMENT_INT16 : EMU_ARGUMENT_UINT16;
    return std::is_signed<Type>::value ? EMU_ARGUMENT_INT8 : EMU_ARGUMENT_UINT8;
}

template <ULONG Count, typename Function, bool Exact = true>
struct EMU_SIGNATURE;

template <ULONG Count, bool Exact, typename Result, typename... Types>
struct EMU_SIGNATURE<Count, Result (NTAPI *)(Types...), Exact>
{
    static_assert(sizeof...(Types) * EMU_ARGUMENT_BITS <= 64);

    static constexpr ULONG64 Get()
    {
        ULONG64 Signature = 0;
        ULONG Shift = 0;

        if (Exact && sizeof...(Types) != Count) return 0;
        ((Signature |= EmuArgumentKind<Types>() << Shift, Shift += EMU_ARGUMENT_BITS), ...);
        return Signature;
    }
};

template <ULONG Count, bool Exact>
struct EMU_SIGNATURE<Count, const EMU_UNTYPED_SERVICE *, Exact>
{
    static constexpr ULONG64 Get()
    {
        return 0;
    }
};

extern "C" {
#endif

extern const ULONG64 EmuNtSignatures[];
extern const ULONG64 EmuWin32kSignatures[];

#ifdef __cplusplus
}
#endif
