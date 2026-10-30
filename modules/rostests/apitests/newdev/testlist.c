#define STANDALONE
#include <apitest.h>

extern void func_DiRollbackDriver(void);
extern void func_protocol(void);

const struct test winetest_testlist[] =
{
    { "DiRollbackDriver", func_DiRollbackDriver },
    { "protocol", func_protocol },
    { 0, 0 }
};
