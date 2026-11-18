#define STANDALONE
#include <apitest.h>

extern void func_TaskEngine(void);

const struct test winetest_testlist[] =
{
    { "TaskEngine", func_TaskEngine },
    { 0, 0 }
};
