/*
 * PROJECT:     LiberNT Kernel-Mode Driver Framework
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     USB host controller extension (UCX) class-extension client binding record
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#include <ntddk.h>
#include "wdf.h"
#include <fxldr.h>
#include <ucxclass.h>

#define WDFCX_STRING2(_Text) #_Text
#define WDFCX_STRING(_Text) WDFCX_STRING2(_Text)
#ifdef __USER_LABEL_PREFIX__
#define WDFCX_SYMBOL_PREFIX WDFCX_STRING(__USER_LABEL_PREFIX__)
#elif defined(_M_IX86)
#define WDFCX_SYMBOL_PREFIX "_"
#else
#define WDFCX_SYMBOL_PREFIX ""
#endif

#pragma section(".kmdfclassbind$b", read, write)
#pragma comment(linker, "/include:" WDFCX_SYMBOL_PREFIX "_Ucx_BIND_INFO")

PVOID UcxGlobals;
PFN_UCX_CLASS UcxClassFunctions[UcxFunctionsMax];

DATA_SEG(".kmdfclassbind$b")
WDF_CLASS_BIND_INFO _Ucx_BIND_INFO =
{
    sizeof(WDF_CLASS_BIND_INFO),
    L"Ucx",
    {1, 0, 0},
    (VOID (NTAPI **)(VOID))UcxClassFunctions,
    RTL_NUMBER_OF(UcxClassFunctions),
    &UcxGlobals,
    NULL,
    NULL,
    NULL
};
