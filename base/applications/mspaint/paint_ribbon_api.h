/*
 * PROJECT:    PAINT for LiberNT
 * LICENSE:    LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:    Paint ribbon public API
 * COPYRIGHT:  Copyright 2026 Leeway Open Software
 */

#pragma once

#include "../../../sdk/include/leewayui/ribbon.h"

#ifdef __cplusplus
extern "C" {
#endif

HRESULT PaintRibbonInitialize(HWND hwndMain, ILeewayRibbon **pRibbon);
BOOL PaintRibbonHandleCommand(UINT32 id);
void PaintRibbonCleanup(ILeewayRibbon *ribbon);
void PaintRibbonInvalidateCommand(ILeewayRibbon *ribbon, UINT32 command_id);
void PaintRibbonInvalidateAll(ILeewayRibbon *ribbon);
void PaintRibbonSetTextToolActive(ILeewayRibbon *ribbon, BOOL active);

#ifdef __cplusplus
}
#endif
