#define STANDALONE
#include <apitest.h>

extern void func_LevelMachine(void);

const struct test winetest_testlist[] =
{
    { "LevelMachine", func_LevelMachine },
    { 0, 0 }
};
