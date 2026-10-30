#define STANDALONE
#include <apitest.h>

extern void func_DrvStoreStaging(void);
extern void func_PackageSignature(void);

const struct test winetest_testlist[] =
{
    { "DrvStoreStaging", func_DrvStoreStaging },
    { "PackageSignature", func_PackageSignature },
    { 0, 0 }
};
