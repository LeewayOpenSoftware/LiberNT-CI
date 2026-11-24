/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Usermode Font Driver Host test list
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_fontdrvhost(void);

const struct test winetest_testlist[] =
{
    { "fontdrvhost", func_fontdrvhost },
    { 0, 0 }
};
