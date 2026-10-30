#define STANDALONE
#include <apitest.h>

extern void func_DrvStoreStaging(void);
extern void func_PackageSignature(void);
extern void func_VerifyInfFile(void);

const struct test winetest_testlist[] =
{
    { "DrvStoreStaging", func_DrvStoreStaging },
    { "PackageSignature", func_PackageSignature },
    { "VerifyInfFile", func_VerifyInfFile },
    { 0, 0 }
};
