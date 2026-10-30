#define STANDALONE
#include <apitest.h>

extern void func_ReenumerateDevNode(void);

const struct test winetest_testlist[] =
{
    { "ReenumerateDevNode", func_ReenumerateDevNode },
    { 0, 0 }
};
