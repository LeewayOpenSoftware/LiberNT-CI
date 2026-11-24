#define STANDALONE
#include <apitest.h>

extern void func_Authorize(void);
extern void func_LaunchState(void);

const struct test winetest_testlist[] =
{
    { "Authorize", func_Authorize },
    { "LaunchState", func_LaunchState },
    { 0, 0 }
};
