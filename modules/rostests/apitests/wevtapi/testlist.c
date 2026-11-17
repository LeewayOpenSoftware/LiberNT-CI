#define STANDALONE
#include <apitest.h>

extern void func_EvtQuery(void);

const struct test winetest_testlist[] =
{
    { "EvtQuery", func_EvtQuery },
    { 0, 0 }
};
