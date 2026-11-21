/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     MI client API-test registration
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_MiEnumerate(void);

const struct test winetest_testlist[] =
{
    { "MiEnumerate", func_MiEnumerate },
    { 0, 0 }
};
