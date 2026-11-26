#define STANDALONE
#include <apitest.h>

extern void func_ImageCheck(void);

const struct test winetest_testlist[] =
{
    { "ImageCheck", func_ImageCheck },
    { 0, 0 }
};
