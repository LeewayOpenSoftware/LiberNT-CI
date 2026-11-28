/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test winver's brand acceptance (Orosi and legacy LiberNT)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_winver_brand(void);

const struct test winetest_testlist[] =
{
    { "winver_brand", func_winver_brand },
    { 0, 0 }
};
