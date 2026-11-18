#define STANDALONE
#include <apitest.h>

extern void func_Smb2Crypto(void);
extern void func_Smb2Lock(void);
extern void func_Smb2Negotiate(void);
extern void func_Smb2Notify(void);
extern void func_Smb2Rename(void);

const struct test winetest_testlist[] =
{
    { "Smb2Crypto", func_Smb2Crypto },
    { "Smb2Lock", func_Smb2Lock },
    { "Smb2Negotiate", func_Smb2Negotiate },
    { "Smb2Notify", func_Smb2Notify },
    { "Smb2Rename", func_Smb2Rename },

    { 0, 0 }
};
