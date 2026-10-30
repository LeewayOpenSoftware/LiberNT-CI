/*
 * PROJECT:     LiberNT OpenGL thunk
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ICD entry points that the guest ICD reaches in the native OpenGL host
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#pragma once

#define GLTHUNK_HOST_LIBRARY L"mesa_host.dll"

typedef struct _GLTHUNK_TABLE
{
    ULONG Size;
    ULONG Reserved;
    ULONG64 DrvValidateVersion;
    ULONG64 DrvSetCallbackProcs;
    ULONG64 DrvCopyContext;
    ULONG64 DrvCreateContext;
    ULONG64 DrvCreateLayerContext;
    ULONG64 DrvDeleteContext;
    ULONG64 DrvDescribeLayerPlane;
    ULONG64 DrvDescribePixelFormat;
    ULONG64 DrvGetLayerPaletteEntries;
    ULONG64 DrvGetProcAddress;
    ULONG64 DrvReleaseContext;
    ULONG64 DrvRealizeLayerPalette;
    ULONG64 DrvSetContext;
    ULONG64 DrvSetLayerPaletteEntries;
    ULONG64 DrvSetPixelFormat;
    ULONG64 DrvShareLists;
    ULONG64 DrvSwapBuffers;
    ULONG64 DrvSwapLayerBuffers;
} GLTHUNK_TABLE, *PGLTHUNK_TABLE;
