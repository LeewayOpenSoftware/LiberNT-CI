/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     MI client API-test registration
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_MiEnumerate(void);
extern void func_MiPush(void);
extern void func_MiPushClose(void);

const struct test winetest_testlist[] =
{
    { "MiEnumerate", func_MiEnumerate },
    { "MiPush", func_MiPush },
    { "MiPushClose", func_MiPushClose },
    { 0, 0 }
};
