/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Push (callback) delivery of Win32_OperatingSystem via the MI FT,
 *              plus Clone of each delivered instance (the path MMI drives).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <apitest.h>
#include <wchar.h>
#include <mi.h>

typedef struct _push_state
{
    LONG      count;
    MI_Result term;
    MI_Boolean got_terminal;
    HANDLE    done;
}
push_state;

static void MI_CALL test_instance_cb(MI_Operation *op, void *ctx, const MI_Instance *inst, MI_Boolean more,
    MI_Result rc, const MI_Char *err, const MI_Instance *errd, MI_Result (MI_CALL *ack)(MI_Operation*))
{
    push_state *st = (push_state *)ctx;
    (void)more; (void)err; (void)errd;
    if (inst)
    {
        MI_Instance *clone = NULL;
        InterlockedIncrement(&st->count);
        if (inst->ft->Clone(inst, &clone) == MI_RESULT_OK && clone)
        {
            const MI_Char *cls = NULL;
            MI_Instance_GetClassName(clone, &cls);
            clone->ft->Delete(clone);
        }
    }
    else
    {
        st->term = rc;
        st->got_terminal = MI_TRUE;
        if (st->done) SetEvent(st->done);
    }
    if (ack) ack(op);
}

START_TEST(MiPush)
{
    MI_Application app = MI_APPLICATION_NULL;
    MI_Session ses = MI_SESSION_NULL;
    MI_Operation op = MI_OPERATION_NULL;
    MI_OperationCallbacks cbs = MI_OPERATIONCALLBACKS_NULL;
    MI_Instance *extErr = NULL;
    push_state st;
    MI_Result r;
    DWORD w;

    r = MI_Application_InitializeV1(0, NULL, &extErr, &app);
    ok(r == MI_RESULT_OK, "MI_Application_InitializeV1 = %d\n", (int)r);
    if (r != MI_RESULT_OK) return;
    r = MI_Application_NewSession(&app, NULL, NULL, NULL, NULL, &extErr, &ses);
    ok(r == MI_RESULT_OK, "MI_Application_NewSession = %d\n", (int)r);
    if (r != MI_RESULT_OK) { MI_Application_Close(&app); return; }

    st.count = 0;
    st.term = MI_RESULT_FAILED;
    st.got_terminal = MI_FALSE;
    st.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    cbs.callbackContext = &st;
    cbs.instanceResult = test_instance_cb;

    MI_Session_EnumerateInstances(&ses, 0, NULL, L"root/cimv2", L"Win32_OperatingSystem", MI_FALSE, &cbs, &op);

    w = WaitForSingleObject(st.done, 30000);
    ok(w == WAIT_OBJECT_0, "push delivery did not complete (wait=%lu)\n", w);
    ok(st.got_terminal == MI_TRUE, "no terminal callback\n");
    ok(st.term == MI_RESULT_OK, "terminal result = %d\n", (int)st.term);
    ok(st.count >= 1, "push delivered %d instances\n", (int)st.count);

    MI_Operation_Close(&op);
    MI_Session_Close(&ses, NULL, NULL);
    MI_Application_Close(&app);
    if (st.done) CloseHandle(st.done);
}
