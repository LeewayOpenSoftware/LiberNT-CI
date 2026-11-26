#define STANDALONE
#include <apitest.h>

extern void func_TdhTl(void);

const struct test winetest_testlist[] =
{
    { "TdhTl", func_TdhTl },
    { 0, 0 }
};
