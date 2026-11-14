/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     SHGetStockIconInfo(SIID_SHIELD) returns a real shield icon
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "shelltest.h"

START_TEST(StockIconShield)
{
    SHSTOCKICONINFO shield = { sizeof(shield) }, doc = { sizeof(doc) };
    HRESULT hr;

    hr = SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICONLOCATION, &shield);
    ok_hr(hr, S_OK);
    hr = SHGetStockIconInfo(SIID_DOCNOASSOC, SHGSI_ICONLOCATION, &doc);
    ok_hr(hr, S_OK);
    ok(lstrcmpiW(shield.szPath, doc.szPath) != 0 || shield.iIcon != doc.iIcon,
       "SIID_SHIELD falls back to the document icon (%S,%d)\n", shield.szPath, shield.iIcon);

    hr = SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &shield);
    ok_hr(hr, S_OK);
    ok(shield.hIcon != NULL, "no shield icon\n");
    if (shield.hIcon)
        DestroyIcon(shield.hIcon);
}
