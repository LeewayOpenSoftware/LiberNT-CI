/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Enumerate Win32_OperatingSystem through the MI client function table
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <wchar.h>
#include <mi.h>

START_TEST(MiEnumerate)
{
    MI_Application app = MI_APPLICATION_NULL;
    MI_Session ses = MI_SESSION_NULL;
    MI_Operation op = MI_OPERATION_NULL;
    MI_Instance *extErr = NULL;
    MI_Result r;
    MI_Uint32 count = 0;
    MI_Boolean saw_terminal = MI_FALSE;

    r = MI_Application_InitializeV1(0, NULL, &extErr, &app);
    ok(r == MI_RESULT_OK, "MI_Application_InitializeV1 = %d\n", (int)r);
    if (r != MI_RESULT_OK) return;

    r = MI_Application_NewSession(&app, NULL, NULL, NULL, NULL, &extErr, &ses);
    ok(r == MI_RESULT_OK, "MI_Application_NewSession = %d\n", (int)r);
    if (r != MI_RESULT_OK) { MI_Application_Close(&app); return; }

    MI_Session_EnumerateInstances(&ses, 0, NULL, L"root/cimv2", L"Win32_OperatingSystem", MI_FALSE, NULL, &op);

    for (;;)
    {
        const MI_Instance *inst = NULL;
        MI_Boolean more = MI_FALSE;
        MI_Result res = MI_RESULT_OK;
        const MI_Char *errMsg = NULL;
        const MI_Instance *cd = NULL;

        r = MI_Operation_GetInstance(&op, &inst, &more, &res, &errMsg, &cd);
        ok(r == MI_RESULT_OK, "MI_Operation_GetInstance call = %d\n", (int)r);
        if (r != MI_RESULT_OK) break;

        if (inst)
        {
            const MI_Char *cls = NULL;
            MI_Value val;
            MI_Type type = 0;
            MI_Uint32 flags = 0, idx = 0, elems = 0;

            count++;
            MI_Instance_GetClassName(inst, &cls);
            ok(cls && wcscmp(cls, L"Win32_OperatingSystem") == 0,
               "class name = %ls\n", cls ? cls : L"(null)");

            MI_Instance_GetElementCount(inst, &elems);
            ok(elems > 0, "instance has %u elements\n", elems);

            if (MI_Instance_GetElement(inst, L"Caption", &val, &type, &flags, &idx) == MI_RESULT_OK
                && type == MI_STRING && !(flags & MI_FLAG_NULL))
            {
                trace("OS Caption: %ls\n", val.string ? val.string : L"");
            }
        }
        else
        {
            /* terminal: must be reported exactly once, with result OK */
            saw_terminal = MI_TRUE;
            ok(res == MI_RESULT_OK, "terminal result = %d\n", (int)res);
        }
        if (!more) break;
    }

    ok(count >= 1, "expected at least one Win32_OperatingSystem instance, got %u\n", count);
    ok(saw_terminal == MI_TRUE, "operation did not deliver a terminal result\n");

    MI_Operation_Close(&op);
    MI_Session_Close(&ses, NULL, NULL);
    MI_Application_Close(&app);
}
