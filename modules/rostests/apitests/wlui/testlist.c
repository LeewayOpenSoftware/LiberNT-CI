#define STANDALONE
#include <apitest.h>

extern void func_Ipc(void);

const struct test winetest_testlist[] =
{
    { "Ipc", func_Ipc },
    { 0, 0 }
};
