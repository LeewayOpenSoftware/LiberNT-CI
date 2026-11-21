/*
 * PROJECT:     Orosi API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for pwrshplugin.dll (WSMan plugin stubs)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_pwrshplugin(void);

const struct test winetest_testlist[] =
{
    { "pwrshplugin", func_pwrshplugin },
    { 0, 0 }
};
