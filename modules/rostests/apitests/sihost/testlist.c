/* testlist.c - Apitest list for sihost.exe */

#define STANDALONE
#include <wine/test.h>

extern void func_sihost_phase0(void);

const struct test winetest_testlist[] =
{
    { "sihost_phase0", func_sihost_phase0 },
    { 0, 0 }
};
