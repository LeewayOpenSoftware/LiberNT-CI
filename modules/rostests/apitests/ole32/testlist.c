
#define STANDALONE
#include <wine/test.h>

extern void func_globaloptions(void);
extern void func_initializespy(void);
extern void func_launchid(void);
extern void func_localserver(void);
extern void func_security(void);
extern void func_storagestat(void);
extern void func_surrogate(void);
extern void func_uninitquit(void);

const struct test winetest_testlist[] =
{
    { "globaloptions", func_globaloptions },
    { "initializespy", func_initializespy },
    { "launchid", func_launchid },
    { "localserver", func_localserver },
    { "security", func_security },
    { "storagestat", func_storagestat },
    { "surrogate", func_surrogate },
    { "uninitquit", func_uninitquit },

    { 0, 0 }
};
