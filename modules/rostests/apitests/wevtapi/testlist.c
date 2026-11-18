#define STANDALONE
#include <apitest.h>

extern void func_EvtFormatMessage(void);
extern void func_EvtQuery(void);

const struct test winetest_testlist[] =
{
    { "EvtFormatMessage", func_EvtFormatMessage },
    { "EvtQuery", func_EvtQuery },
    { 0, 0 }
};
