/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Window colour (active title bar) over SetSysColors and HKCU\\Control Panel\\Colors
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <leewayui/display_backend.h>
#include <strsafe.h>

COLORREF
LWinColorGet(void)
{
    return GetSysColor(COLOR_ACTIVECAPTION);
}

BOOL
LWinColorSet(COLORREF color)
{
    INT element = COLOR_ACTIVECAPTION;
    COLORREF old = GetSysColor(COLOR_ACTIVECAPTION);
    WCHAR text[32];
    HKEY key;
    LONG result;

    if (!SetSysColors(1, &element, &color))
        return FALSE;
    StringCchPrintfW(text, ARRAYSIZE(text), L"%u %u %u", GetRValue(color), GetGValue(color), GetBValue(color));
    result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Colors", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (result == ERROR_SUCCESS)
    {
        result = RegSetValueExW(key, L"ActiveTitle", 0, REG_SZ, (const BYTE *)text, (lstrlenW(text) + 1) * sizeof(WCHAR));
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS)
    {
        /* never leave the live colour different from the stored one */
        SetSysColors(1, &element, &old);
        return FALSE;
    }
    return TRUE;
}
