/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Shell Infrastructure Host test list
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_sihost(void);

const struct test winetest_testlist[] =
{
    { "sihost", func_sihost },
    { 0, 0 }
};
