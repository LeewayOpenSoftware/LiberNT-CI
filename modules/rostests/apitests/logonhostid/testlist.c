#define STANDALONE
#include <apitest.h>

extern void func_Identity(void);

const struct test winetest_testlist[] =
{
    { "Identity", func_Identity },
    { 0, 0 }
};
