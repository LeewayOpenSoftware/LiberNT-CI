#define STANDALONE
#include <apitest.h>

extern void func_GdiOnlyCallback(void);

const struct test winetest_testlist[] =
{
    { "GdiOnlyCallback", func_GdiOnlyCallback },
    { 0, 0 }
};
