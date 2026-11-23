#define STANDALONE
#include <apitest.h>

extern void func_Abi(void);

const struct test winetest_testlist[] =
{
    { "Abi", func_Abi },
    { 0, 0 }
};
