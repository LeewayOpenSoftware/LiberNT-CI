/*
 * PROJECT:     LiberNT uxtheme
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Trust gate in front of every visual-style load (ThemeLoader4 I1a-T, THEME-TRUST-POLICY v5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#define UXT_PART_THEMES L"Themes"

BOOL UXTHEME_TrustIsPrivileged(void);

BOOL UXTHEME_TrustUsesMachineDefault(void);

BOOL UXTHEME_TrustV3AnyPathOrdinary(void);

BOOL UXTHEME_TrustNameFromStoredPath(LPCWSTR text, LPWSTR name, DWORD cchName);

void UXTHEME_TrustMachineDefault(LPWSTR name, DWORD cchName);

BOOL UXTHEME_TrustBuildV3Path(LPCWSTR name, LPWSTR path, DWORD cchPath);

HRESULT UXTHEME_TrustOpenThemeFile(LPCWSTR path, LPCWSTR pszColorName, LPCWSTR pszSizeName, PTHEME_FILE *tf, BOOL selfLoad);

void UXTHEME_TrustClearGuard(PTHEME_FILE tf);

BOOL UXTHEME_TrustReloadAllowed(void);
UINT UXTHEME_TrustReloadDelay(void);
