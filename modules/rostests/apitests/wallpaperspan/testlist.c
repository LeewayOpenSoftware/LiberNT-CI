#define STANDALONE
#include <apitest.h>

extern void func_SpanGeometry(void);

const struct test winetest_testlist[] =
{
    { "SpanGeometry", func_SpanGeometry },
    { 0, 0 }
};
