#define STANDALONE
#include <apitest.h>

extern void func_Abi(void);
extern void func_CredProtect(void);
extern void func_CredUIPack(void);
extern void func_Framework(void);
extern void func_PasswordProvider(void);
extern void func_Serialization(void);

const struct test winetest_testlist[] =
{
    { "Abi", func_Abi },
    { "CredProtect", func_CredProtect },
    { "CredUIPack", func_CredUIPack },
    { "Framework", func_Framework },
    { "PasswordProvider", func_PasswordProvider },
    { "Serialization", func_Serialization },
    { 0, 0 }
};
