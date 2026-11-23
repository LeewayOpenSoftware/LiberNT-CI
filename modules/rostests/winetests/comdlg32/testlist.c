/* Automatically generated file; DO NOT EDIT!! */

#define STANDALONE
#include <wine/test.h>

extern void func_filedlg(void);
extern void func_finddlg(void);
extern void func_fontdlg(void);
extern void func_itemdlg(void);
extern void func_itemroute(void);
extern void func_printdlg(void);

const struct test winetest_testlist[] =
{
    { "filedlg", func_filedlg },
    { "finddlg", func_finddlg },
    { "fontdlg", func_fontdlg },
    //{ "itemdlg", func_itemdlg }, // Win 7
    { "itemroute", func_itemroute },
    { "printdlg", func_printdlg },
    { 0, 0 }
};
