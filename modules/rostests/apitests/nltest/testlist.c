/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test list for nltest.exe
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#define STANDALONE
#include <apitest.h>

extern void func_nltest(void);

const struct test winetest_testlist[] =
{
    { "nltest", func_nltest },
    { 0, 0 }
};
