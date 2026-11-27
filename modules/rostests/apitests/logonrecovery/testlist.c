#define STANDALONE
#include <apitest.h>

extern void func_Recovery(void);

const struct test winetest_testlist[] =
{
    { "Recovery", func_Recovery },
    { 0, 0 }
};
