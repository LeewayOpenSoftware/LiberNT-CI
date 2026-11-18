#define STANDALONE
#include <apitest.h>

extern void func_Smb2Lock(void);
extern void func_Smb2Notify(void);
extern void func_Smb2Rename(void);

const struct test winetest_testlist[] =
{
    { "Smb2Lock", func_Smb2Lock },
    { "Smb2Notify", func_Smb2Notify },
    { "Smb2Rename", func_Smb2Rename },

    { 0, 0 }
};
