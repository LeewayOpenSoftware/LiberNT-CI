/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Usermode Font Driver Host test list
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#define STANDALONE
#include <apitest.h>

extern void func_fontdrvhost(void);
extern void func_umfd_channel(void);
extern void func_umfd_exit(void);
extern void func_umfd_freeze(void);
extern void func_umfd_recycle(void);

const struct test winetest_testlist[] =
{
    { "fontdrvhost", func_fontdrvhost },
    { "umfd_channel", func_umfd_channel },
    { "umfd_exit", func_umfd_exit },
    { "umfd_freeze", func_umfd_freeze },
    { "umfd_recycle", func_umfd_recycle },
    { 0, 0 }
};
