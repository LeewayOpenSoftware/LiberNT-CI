#define STANDALONE
#include <apitest.h>

extern void func_PciIds(void);

const struct test winetest_testlist[] =
{
    { "PciIds", func_PciIds },
    { 0, 0 }
};
