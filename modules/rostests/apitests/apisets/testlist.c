
#define STANDALONE
#include <apitest.h>

extern void func_apisets(void);
extern void func_win11_fallback(void);
extern void func_apisetmap(void);

const struct test winetest_testlist[] =
{
    { "apisets", func_apisets },
    { "win11_fallback", func_win11_fallback },
    { "apisetmap", func_apisetmap },
    { 0, 0 }
};
