#define STANDALONE
#include <apitest.h>

extern void func_DiRollbackDriver(void);
extern void func_protocol(void);
extern void func_UpdateDriver(void);

const struct test winetest_testlist[] =
{
    { "DiRollbackDriver", func_DiRollbackDriver },
    { "protocol", func_protocol },
    { "UpdateDriver", func_UpdateDriver },
    { 0, 0 }
};
