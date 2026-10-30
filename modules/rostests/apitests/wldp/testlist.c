#define STANDALONE
#include <apitest.h>

extern void func_WldpPolicy(void);

const struct test winetest_testlist[] =
{
    { "WldpPolicy", func_WldpPolicy },
    { 0, 0 }
};
