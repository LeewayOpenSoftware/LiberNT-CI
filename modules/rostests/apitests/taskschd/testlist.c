#define STANDALONE
#include <apitest.h>

extern void func_TaskEngine(void);
extern void func_ExecXmlEmpty(void);

const struct test winetest_testlist[] =
{
    { "TaskEngine", func_TaskEngine },
    { "ExecXmlEmpty", func_ExecXmlEmpty },
    { 0, 0 }
};
