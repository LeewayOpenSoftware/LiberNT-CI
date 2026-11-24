
#define STANDALONE
#include <apitest.h>

extern void func_SChannelChain(void);

const struct test winetest_testlist[] =
{
    { "SChannelChain", func_SChannelChain },

    { 0, 0 }
};
