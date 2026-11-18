#define STANDALONE
#include <apitest.h>

extern void func_DevGetObjectsContainer(void);
extern void func_ReenumerateDevNode(void);

const struct test winetest_testlist[] =
{
    { "DevGetObjectsContainer", func_DevGetObjectsContainer },
    { "ReenumerateDevNode", func_ReenumerateDevNode },
    { 0, 0 }
};
