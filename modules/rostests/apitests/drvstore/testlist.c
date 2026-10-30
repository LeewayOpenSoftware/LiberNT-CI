#define STANDALONE
#include <apitest.h>

extern void func_DrvStoreStaging(void);

const struct test winetest_testlist[] =
{
    { "DrvStoreStaging", func_DrvStoreStaging },
    { 0, 0 }
};
