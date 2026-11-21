#define STANDALONE
#include <apitest.h>

extern void func_InstalledCollection(void);

const struct test winetest_testlist[] =
{
    { "InstalledCollection", func_InstalledCollection },

    { 0, 0 }
};
