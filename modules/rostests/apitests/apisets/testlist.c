
#define STANDALONE
#include <apitest.h>

extern void func_apisets(void);
extern void func_win11_fallback(void);

const struct test winetest_testlist[] =
{
    { "apisets", func_apisets },
    { "win11_fallback", func_win11_fallback },
    { 0, 0 }
};
