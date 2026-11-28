#define STANDALONE
#include <apitest.h>

extern void func_ale(void);
extern void func_mpssvc(void);
extern void func_netfwpolicy2(void);

const struct test winetest_testlist[] =
{
    { "ale", func_ale },
    { "mpssvc", func_mpssvc },
    { "netfwpolicy2", func_netfwpolicy2 },
    { 0, 0 }
};
