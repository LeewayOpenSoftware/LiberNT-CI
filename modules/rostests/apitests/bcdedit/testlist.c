#define STANDALONE
#include <apitest.h>

extern void func_BcdEdit(void);

const struct test winetest_testlist[] =
{
    { "BcdEdit", func_BcdEdit },
    { 0, 0 }
};
