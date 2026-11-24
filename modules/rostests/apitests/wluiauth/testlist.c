#define STANDALONE
#include <apitest.h>

extern void func_Authorize(void);

const struct test winetest_testlist[] =
{
    { "Authorize", func_Authorize },
    { 0, 0 }
};
