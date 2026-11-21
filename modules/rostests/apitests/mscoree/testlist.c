/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Fallback-path test for mscoree.dll (no real .NET CLR)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_mscoree(void);

const struct test winetest_testlist[] =
{
    { "mscoree", func_mscoree },
    { 0, 0 }
};
