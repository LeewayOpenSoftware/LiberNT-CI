/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Shared declarations between the applet entry and the Control Panel window
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

enum
{
    PG_PERSONALIZE = 0,
    PG_BACKGROUND,
    PG_COLOR,
    PG_DISPLAY,
    PG_RESOLUTION,
    PG_COUNT
};

extern HINSTANCE hInst;

DWORD Caps(void);
void CapsReset(void);
void LoadStr(UINT id, WCHAR *buf, int cch);
void Msg(HWND hwnd, UINT textId, UINT flags);

INT_PTR CALLBACK DisplayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK SaverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
