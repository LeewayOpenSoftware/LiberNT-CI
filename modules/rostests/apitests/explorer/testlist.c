#define STANDALONE
#include <apitest.h>

extern void func_VolumeOsd(void);

const struct test winetest_testlist[] =
{
    { "VolumeOsd", func_VolumeOsd },
    { 0, 0 }
};
