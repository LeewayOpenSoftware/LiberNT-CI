#define STANDALONE
#include <apitest.h>

extern void func_Smb2Rename(void);

const struct test winetest_testlist[] =
{
    { "Smb2Rename", func_Smb2Rename },

    { 0, 0 }
};
