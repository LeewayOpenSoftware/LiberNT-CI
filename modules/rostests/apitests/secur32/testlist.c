#define STANDALONE
#include <apitest.h>

extern void func_GetUserNameEx(void);

const struct test winetest_testlist[] =
{
    { "GetUserNameEx", func_GetUserNameEx },
    { 0, 0 }
};
