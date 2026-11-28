#define STANDALONE
#include <apitest.h>

extern void func_ustring(void);
extern void func_version(void);

const struct test winetest_testlist[] =
{
    { "ustring", func_ustring },
    { "version", func_version },
    { 0, 0 }
};
