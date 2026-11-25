#define STANDALONE
#include <apitest.h>

extern void func_cmdline(void);
extern void func_process(void);

const struct test winetest_testlist[] =
{
    { "cmdline", func_cmdline },
    { "process", func_process },
    { 0, 0 }
};
