/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Page component ABI between the Explorer view (shell32) and leewaydisp.cpl
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * Mirrors hive/shared/libernt/shell/personalization-cp-namespace/DESIGN.md.
 */

#pragma once

#define LWPAGE_MAIN       0
#define LWPAGE_BACKGROUND 1
#define LWPAGE_WINCOLOR   2
#define LWPAGE_DISPLAY    3
#define LWPAGE_RESOLUTION 4
#define LWNAV_CPHOME      0xFFFF

typedef struct LWPAGE_HOST
{
    DWORD cbSize;
    void *ctx;
    HRESULT (CALLBACK *Navigate)(void *ctx, UINT pageId);
    HRESULT (CALLBACK *SetStatus)(void *ctx, PCWSTR text);
} LWPAGE_HOST;

HWND WINAPI LeewayPageCreate(UINT pageId, HWND hwndParent, const RECT *prc, const LWPAGE_HOST *host, void **ppPage);
void WINAPI LeewayPageResize(void *page, const RECT *prc);
BOOL WINAPI LeewayPageTranslateKey(void *page, MSG *msg);
void WINAPI LeewayPageDestroy(void *page);
