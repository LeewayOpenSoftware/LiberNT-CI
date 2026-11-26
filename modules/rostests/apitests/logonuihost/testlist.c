#define STANDALONE
#include <apitest.h>

extern void func_Host(void);

const struct test winetest_testlist[] =
{
    { "Host", func_Host },
    { 0, 0 }
};
