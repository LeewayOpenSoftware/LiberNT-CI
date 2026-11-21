/*
 * PROJECT:     LiberNT Management Infrastructure (MI) client
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal mi.dll: an MI client shim over the in-process wbemprox
 *              COM server, enough for pwsh Get-CimInstance (Win32_OperatingSystem,
 *              Win32_Process). Design: dotnet-powershell/MI-MIN-DESIGN.md; ABI
 *              ground truth: dotnet-powershell/MI-ABI.md.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "oleauto.h"
#include "wbemcli.h"

#include "mi.h"

/* WBEM client GUIDs. Defined locally (initguid) so mi.dll needs no extra GUID
 * import lib; values are the stable public WMI GUIDs. VERIFY at build against
 * the tree's wbemuuid (sdk/lib) before freezing. */
#include <initguid.h>
DEFINE_GUID(CLSID_WbemLocator, 0x4590f811, 0x1d3a, 0x11d0, 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24);
DEFINE_GUID(IID_IWbemLocator, 0xdc12a687, 0x737f, 0x11cf, 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24);
DEFINE_GUID(IID_IWbemServices, 0x9556dc99, 0x828c, 0x11cf, 0xa3, 0x7e, 0x00, 0xaa, 0x00, 0x32, 0x40, 0xc7);
DEFINE_GUID(IID_IEnumWbemClassObject, 0x027947e1, 0xd731, 0x11ce, 0xa3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01);
DEFINE_GUID(IID_IWbemClassObject, 0xdc12a681, 0x737f, 0x11cf, 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24);

/* ---- forward FT globals ---- */
static const MI_ApplicationFT       g_application_ft;
static const MI_SessionFT           g_session_ft;
static const MI_OperationFT         g_operation_ft;
static const MI_InstanceFT          g_instance_ft;
static const MI_DestinationOptionsFT g_destination_options_ft;
static const MI_OperationOptionsFT  g_operation_options_ft;
static const MI_UtilitiesFT         g_utilities_ft;

/* ---- small heap helpers ---- */
static void *mi_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void  mi_free(void *p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }

static MI_Char *mi_wcsdup(const MI_Char *s)
{
    SIZE_T n;
    MI_Char *d;
    if (!s) return NULL;
    n = (lstrlenW(s) + 1) * sizeof(MI_Char);
    d = mi_alloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* =====================================================================
 * Private objects. MI handles have a fixed layout {reserved1, reserved2,
 * ft}; we stash our private pointer in reserved2 (pointer-sized).
 * ===================================================================== */

typedef struct _mi_element
{
    MI_Char  *name;
    MI_Type   type;
    MI_Value  value;   /* strings/arrays point into heap we own */
    MI_Uint32 flags;
}
mi_element;

/* built instance: a caller-visible MI_Instance whose reserved[] carry our data.
 *   reserved[0] = mi_element* array
 *   reserved[1] = element count
 *   reserved[2] = class name (MI_Char*)
 */
#define INST_ELEMS(i)  ((mi_element*)(i)->reserved[0])
#define INST_COUNT(i)  ((MI_Uint32)(i)->reserved[1])
#define INST_CLASS(i)  ((MI_Char*)(i)->reserved[2])

typedef struct _mi_session_priv
{
    IWbemServices *svc;     /* ROOT\CIMV2 */
    MI_Boolean     com_init;
}
mi_session_priv;

typedef struct _mi_operation_priv
{
    MI_Instance **instances;
    MI_Uint32     count;
    MI_Uint32     next;
    MI_Boolean    terminal_sent;
    MI_Result     status;
}
mi_operation_priv;

#define SES_PRIV(s)  ((mi_session_priv*)(s)->reserved2)
#define OP_PRIV(o)   ((mi_operation_priv*)(o)->reserved2)

/* =====================================================================
 * Instance build + free + instance FT
 * ===================================================================== */

static void mi_instance_free(MI_Instance *inst)
{
    mi_element *e;
    MI_Uint32 i;
    if (!inst) return;
    e = INST_ELEMS(inst);
    for (i = 0; e && i < INST_COUNT(inst); i++)
    {
        mi_free(e[i].name);
        if (e[i].type == MI_STRING) mi_free(e[i].value.string);
    }
    mi_free(e);
    mi_free(INST_CLASS(inst));
    mi_free(inst);
}

/* Convert one VARIANT + CIMTYPE into MI_Value/MI_Type (scalar subset v1). */
static MI_Boolean variant_to_mi(const VARIANT *v, CIMTYPE cim, MI_Value *out, MI_Type *type)
{
    memset(out, 0, sizeof(*out));
    if (V_VT(v) == VT_NULL || V_VT(v) == VT_EMPTY)
    {
        *type = (cim == CIM_STRING) ? MI_STRING : MI_UINT32;
        return MI_FALSE; /* null element */
    }
    switch (cim & ~CIM_FLAG_ARRAY)
    {
    case CIM_STRING: case CIM_REFERENCE: case CIM_DATETIME:
        *type = MI_STRING;
        out->string = mi_wcsdup(V_BSTR(v));
        return out->string != NULL;
    case CIM_BOOLEAN:  *type = MI_BOOLEAN; out->boolean = V_BOOL(v) ? MI_TRUE : MI_FALSE; return MI_TRUE;
    case CIM_UINT8:    *type = MI_UINT8;   out->uint8  = (MI_Uint8)V_UI1(v); return MI_TRUE;
    case CIM_SINT8:    *type = MI_SINT8;   out->sint8  = (MI_Sint8)V_I1(v);  return MI_TRUE;
    case CIM_UINT16:   *type = MI_UINT16;  out->uint16 = (MI_Uint16)V_UI2(v); return MI_TRUE;
    case CIM_SINT16:   *type = MI_SINT16;  out->sint16 = (MI_Sint16)V_I2(v);  return MI_TRUE;
    case CIM_UINT32:   *type = MI_UINT32;  out->uint32 = (MI_Uint32)V_UI4(v); return MI_TRUE;
    case CIM_SINT32:   *type = MI_SINT32;  out->sint32 = (MI_Sint32)V_I4(v);  return MI_TRUE;
    case CIM_UINT64:   *type = MI_UINT64;  out->uint64 = _wtoi64(V_BSTR(v));  return MI_TRUE; /* WMI returns 64-bit as BSTR */
    case CIM_SINT64:   *type = MI_SINT64;  out->sint64 = _wtoi64(V_BSTR(v));  return MI_TRUE;
    case CIM_REAL32:   *type = MI_REAL32;  out->real32 = (MI_Real32)V_R4(v);  return MI_TRUE;
    case CIM_REAL64:   *type = MI_REAL64;  out->real64 = (MI_Real64)V_R8(v);  return MI_TRUE;
    default:
        /* arrays and unsupported CIM types: expose as null for v1 */
        *type = MI_STRING;
        return MI_FALSE;
    }
}

/* Build an MI_Instance from an IWbemClassObject (non-system properties). */
static MI_Instance *build_instance(IWbemClassObject *obj)
{
    MI_Instance *inst;
    mi_element *elems = NULL;
    MI_Uint32 cap = 0, n = 0;
    VARIANT vClass;
    HRESULT hr;

    inst = mi_alloc(sizeof(*inst));
    if (!inst) return NULL;
    inst->ft = &g_instance_ft;

    VariantInit(&vClass);
    if (SUCCEEDED(IWbemClassObject_Get(obj, L"__CLASS", 0, &vClass, NULL, NULL)) && V_VT(&vClass) == VT_BSTR)
        inst->reserved[2] = (ptrdiff_t)mi_wcsdup(V_BSTR(&vClass));
    VariantClear(&vClass);

    hr = IWbemClassObject_BeginEnumeration(obj, WBEM_FLAG_NONSYSTEM_ONLY);
    if (FAILED(hr)) { inst->reserved[0] = 0; inst->reserved[1] = 0; return inst; }

    for (;;)
    {
        BSTR name = NULL;
        VARIANT val;
        CIMTYPE cim = 0;
        VariantInit(&val);
        hr = IWbemClassObject_Next(obj, 0, &name, &val, &cim, NULL);
        if (hr != WBEM_S_NO_ERROR) { VariantClear(&val); if (name) SysFreeString(name); break; }

        if (n == cap)
        {
            MI_Uint32 ncap = cap ? cap * 2 : 16;
            mi_element *ne = mi_alloc(ncap * sizeof(mi_element));
            if (!ne) { VariantClear(&val); SysFreeString(name); break; }
            if (elems) { memcpy(ne, elems, n * sizeof(mi_element)); mi_free(elems); }
            elems = ne; cap = ncap;
        }
        elems[n].name = mi_wcsdup(name);
        elems[n].flags = variant_to_mi(&val, cim, &elems[n].value, &elems[n].type) ? 0 : MI_FLAG_NULL;
        n++;
        VariantClear(&val);
        SysFreeString(name);
    }
    IWbemClassObject_EndEnumeration(obj);

    inst->reserved[0] = (ptrdiff_t)elems;
    inst->reserved[1] = (ptrdiff_t)n;
    return inst;
}

static MI_Result MI_CALL inst_Clone(const MI_Instance *self, MI_Instance **newInstance)
{
    (void)self; (void)newInstance; return MI_RESULT_NOT_SUPPORTED;
}
static MI_Result MI_CALL inst_Destruct(MI_Instance *self) { mi_instance_free(self); return MI_RESULT_OK; }
static MI_Result MI_CALL inst_Delete(MI_Instance *self)   { mi_instance_free(self); return MI_RESULT_OK; }
static MI_Result MI_CALL inst_IsA(const MI_Instance *self, const MI_ClassDecl *cd, MI_Boolean *flag)
{ (void)self; (void)cd; if (flag) *flag = MI_FALSE; return MI_RESULT_OK; }
static MI_Result MI_CALL inst_GetClassName(const MI_Instance *self, const MI_Char **className)
{ if (!className) return MI_RESULT_INVALID_PARAMETER; *className = INST_CLASS(self); return *className ? MI_RESULT_OK : MI_RESULT_NOT_FOUND; }
static MI_Result MI_CALL inst_SetNameSpace(MI_Instance *self, const MI_Char *ns) { (void)self; (void)ns; return MI_RESULT_OK; }
static MI_Result MI_CALL inst_GetNameSpace(const MI_Instance *self, const MI_Char **ns)
{ if (ns) *ns = self->nameSpace; return MI_RESULT_OK; }
static MI_Result MI_CALL inst_GetElementCount(const MI_Instance *self, MI_Uint32 *count)
{ if (!count) return MI_RESULT_INVALID_PARAMETER; *count = INST_COUNT(self); return MI_RESULT_OK; }
static MI_Result MI_CALL inst_AddElement(MI_Instance *self, const MI_Char *name, const MI_Value *value, MI_Type type, MI_Uint32 flags)
{ (void)self;(void)name;(void)value;(void)type;(void)flags; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL inst_SetElement(MI_Instance *self, const MI_Char *name, const MI_Value *value, MI_Type type, MI_Uint32 flags)
{ (void)self;(void)name;(void)value;(void)type;(void)flags; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL inst_SetElementAt(MI_Instance *self, MI_Uint32 index, const MI_Value *value, MI_Type type, MI_Uint32 flags)
{ (void)self;(void)index;(void)value;(void)type;(void)flags; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL inst_GetElement(const MI_Instance *self, const MI_Char *name, MI_Value *value,
                                         MI_Type *type, MI_Uint32 *flags, MI_Uint32 *index)
{
    mi_element *e = INST_ELEMS(self);
    MI_Uint32 i;
    if (!name) return MI_RESULT_INVALID_PARAMETER;
    for (i = 0; e && i < INST_COUNT(self); i++)
    {
        if (lstrcmpiW(e[i].name, name) == 0)
        {
            if (value) *value = e[i].value;
            if (type)  *type  = e[i].type;
            if (flags) *flags = e[i].flags;
            if (index) *index = i;
            return MI_RESULT_OK;
        }
    }
    return MI_RESULT_NO_SUCH_PROPERTY;
}
static MI_Result MI_CALL inst_GetElementAt(const MI_Instance *self, MI_Uint32 index, const MI_Char **name,
                                           MI_Value *value, MI_Type *type, MI_Uint32 *flags)
{
    mi_element *e = INST_ELEMS(self);
    if (index >= INST_COUNT(self)) return MI_RESULT_NOT_FOUND;
    if (name)  *name  = e[index].name;
    if (value) *value = e[index].value;
    if (type)  *type  = e[index].type;
    if (flags) *flags = e[index].flags;
    return MI_RESULT_OK;
}
static MI_Result MI_CALL inst_ClearElement(MI_Instance *self, const MI_Char *name) { (void)self;(void)name; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL inst_ClearElementAt(MI_Instance *self, MI_Uint32 index) { (void)self;(void)index; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL inst_GetServerName(const MI_Instance *self, const MI_Char **name) { if (name) *name = self->serverName; return MI_RESULT_OK; }
static MI_Result MI_CALL inst_SetServerName(MI_Instance *self, const MI_Char *name) { (void)self;(void)name; return MI_RESULT_OK; }
static MI_Result MI_CALL inst_GetClass(const MI_Instance *self, MI_Class **instanceClass) { (void)self; if (instanceClass) *instanceClass = NULL; return MI_RESULT_NOT_SUPPORTED; }

static const MI_InstanceFT g_instance_ft =
{
    inst_Clone, inst_Destruct, inst_Delete, inst_IsA, inst_GetClassName, inst_SetNameSpace,
    inst_GetNameSpace, inst_GetElementCount, inst_AddElement, inst_SetElement, inst_SetElementAt,
    inst_GetElement, inst_GetElementAt, inst_ClearElement, inst_ClearElementAt, inst_GetServerName,
    inst_SetServerName, inst_GetClass
};

/* =====================================================================
 * Operation FT (pull model is primary; push honored when a callback is set)
 * ===================================================================== */

static MI_Result MI_CALL op_Close(MI_Operation *operation)
{
    mi_operation_priv *p = OP_PRIV(operation);
    MI_Uint32 i;
    if (p)
    {
        for (i = 0; i < p->count; i++)
            if (p->instances[i]) mi_instance_free(p->instances[i]);
        mi_free(p->instances);
        mi_free(p);
        operation->reserved2 = 0;
    }
    return MI_RESULT_OK;
}
static MI_Result MI_CALL op_Cancel(MI_Operation *operation, MI_CancellationReason reason)
{
    mi_operation_priv *p = OP_PRIV(operation);
    (void)reason;
    if (p) p->next = p->count; /* no more instances; GetInstance will send terminal */
    return MI_RESULT_OK;
}
static MI_Result MI_CALL op_GetSession(MI_Operation *operation, MI_Session *session)
{ (void)operation; if (session) memset(session, 0, sizeof(*session)); return MI_RESULT_OK; }

static MI_Result MI_CALL op_GetInstance(MI_Operation *operation, const MI_Instance **instance, MI_Boolean *moreResults,
                                        MI_Result *result, const MI_Char **errorMessage, const MI_Instance **completionDetails)
{
    mi_operation_priv *p = OP_PRIV(operation);
    if (errorMessage) *errorMessage = NULL;
    if (completionDetails) *completionDetails = NULL;
    if (!p) { if (instance) *instance = NULL; if (moreResults) *moreResults = MI_FALSE; if (result) *result = MI_RESULT_FAILED; return MI_RESULT_OK; }

    /* Instance-bearing calls report moreResults=TRUE (the caller must call once
     * more to receive the terminal); the terminal call reports moreResults=FALSE. */
    if (p->next < p->count)
    {
        if (instance) *instance = p->instances[p->next];
        p->next++;
        if (moreResults) *moreResults = MI_TRUE;
        if (result) *result = MI_RESULT_OK;
        return MI_RESULT_OK;
    }

    /* terminal: exactly one final result with moreResults = FALSE */
    if (instance) *instance = NULL;
    if (moreResults) *moreResults = MI_FALSE;
    if (result) *result = p->status;
    p->terminal_sent = MI_TRUE;
    return MI_RESULT_OK;
}

static MI_Result MI_CALL op_GetIndication(MI_Operation *operation, const MI_Instance **instance, const MI_Char **bookmark,
                                          const MI_Char **machineID, MI_Boolean *moreResults, MI_Result *result,
                                          const MI_Char **errorMessage, const MI_Instance **completionDetails)
{
    (void)operation;
    if (instance) *instance = NULL;
    if (bookmark) *bookmark = NULL;
    if (machineID) *machineID = NULL;
    if (moreResults) *moreResults = MI_FALSE;
    if (result) *result = MI_RESULT_NOT_SUPPORTED;
    if (errorMessage) *errorMessage = NULL;
    if (completionDetails) *completionDetails = NULL;
    return MI_RESULT_OK;
}
static MI_Result MI_CALL op_GetClass(MI_Operation *operation, const MI_Class **classResult, MI_Boolean *moreResults,
                                     MI_Result *result, const MI_Char **errorMessage, const MI_Instance **completionDetails)
{
    (void)operation;
    if (classResult) *classResult = NULL;
    if (moreResults) *moreResults = MI_FALSE;
    if (result) *result = MI_RESULT_NOT_SUPPORTED;
    if (errorMessage) *errorMessage = NULL;
    if (completionDetails) *completionDetails = NULL;
    return MI_RESULT_OK;
}

static const MI_OperationFT g_operation_ft =
{
    op_Close, op_Cancel, op_GetSession, op_GetInstance, op_GetIndication, op_GetClass
};

/* Deliver buffered results via a provided instanceResult callback (push). */
static MI_Result MI_CALL ack_noop(MI_Operation *operation) { (void)operation; return MI_RESULT_OK; }

static void push_results(MI_Operation *operation, MI_OperationCallbacks *cbs)
{
    mi_operation_priv *p = OP_PRIV(operation);
    MI_Uint32 i;
    if (!cbs || !cbs->instanceResult) return;
    for (i = 0; p && i < p->count; i++)
        cbs->instanceResult(operation, cbs->callbackContext, p->instances[i], MI_TRUE,
                            MI_RESULT_OK, NULL, NULL, ack_noop);
    /* exactly one terminal */
    cbs->instanceResult(operation, cbs ? cbs->callbackContext : NULL, NULL, MI_FALSE,
                        p ? p->status : MI_RESULT_FAILED, NULL, NULL, ack_noop);
    if (p) p->next = p->count, p->terminal_sent = MI_TRUE;
}

/* Fill an operation handle: run the WQL query on the session and buffer rows. */
static void start_query(MI_Session *session, const MI_Char *wql, MI_OperationCallbacks *cbs, MI_Operation *operation)
{
    mi_session_priv *sp = SES_PRIV(session);
    mi_operation_priv *p;
    IEnumWbemClassObject *iter = NULL;
    HRESULT hr;

    memset(operation, 0, sizeof(*operation));
    operation->ft = &g_operation_ft;
    p = mi_alloc(sizeof(*p));
    operation->reserved2 = (ptrdiff_t)p;
    if (!p) return;
    p->status = MI_RESULT_OK;

    if (!sp || !sp->svc) { p->status = MI_RESULT_FAILED; goto done; }

    hr = IWbemServices_ExecQuery(sp->svc, (BSTR)L"WQL", (BSTR)wql,
                                 WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &iter);
    if (FAILED(hr)) { p->status = MI_RESULT_INVALID_QUERY; goto done; }

    for (;;)
    {
        IWbemClassObject *obj = NULL;
        ULONG got = 0;
        hr = IEnumWbemClassObject_Next(iter, WBEM_INFINITE, 1, &obj, &got);
        if (hr != WBEM_S_NO_ERROR || got == 0) { if (obj) IWbemClassObject_Release(obj); break; }
        {
            MI_Instance **na;
            MI_Instance *inst = build_instance(obj);
            IWbemClassObject_Release(obj);
            if (!inst) continue;
            na = mi_alloc((p->count + 1) * sizeof(MI_Instance*));
            if (!na) { mi_instance_free(inst); break; }
            if (p->instances) { memcpy(na, p->instances, p->count * sizeof(MI_Instance*)); mi_free(p->instances); }
            p->instances = na;
            p->instances[p->count++] = inst;
        }
    }
    IEnumWbemClassObject_Release(iter);

done:
    if (cbs && cbs->instanceResult) push_results(operation, cbs);
}

/* =====================================================================
 * Session FT
 * ===================================================================== */

static MI_Result MI_CALL ses_Close(MI_Session *session, void *completionContext, void (MI_CALL *completionCallback)(void*))
{
    mi_session_priv *sp = SES_PRIV(session);
    if (sp)
    {
        if (sp->svc) IWbemServices_Release(sp->svc);
        if (sp->com_init) CoUninitialize();
        mi_free(sp);
        session->reserved2 = 0;
    }
    if (completionCallback) completionCallback(completionContext);
    return MI_RESULT_OK;
}
static MI_Result MI_CALL ses_GetApplication(MI_Session *session, MI_Application *application)
{ (void)session; if (application) { memset(application, 0, sizeof(*application)); application->ft = &g_application_ft; } return MI_RESULT_OK; }

/* Build "SELECT * FROM <class>" and run it. */
static void MI_CALL ses_EnumerateInstances(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
    const MI_Char *namespaceName, const MI_Char *className, MI_Boolean keysOnly, MI_OperationCallbacks *callbacks, MI_Operation *operation)
{
    MI_Char wql[512];
    int n;
    (void)flags; (void)options; (void)namespaceName; (void)keysOnly;
    if (!className) className = L"";
    lstrcpynW(wql, L"SELECT * FROM ", 512);
    n = lstrlenW(wql);
    lstrcpynW(wql + n, className, 512 - n);
    start_query(session, wql, callbacks, operation);
}
static void MI_CALL ses_QueryInstances(MI_Session *session, MI_Uint32 flags, MI_OperationOptions *options,
    const MI_Char *namespaceName, const MI_Char *queryDialect, const MI_Char *queryExpression, MI_OperationCallbacks *callbacks, MI_Operation *operation)
{
    (void)flags; (void)options; (void)namespaceName; (void)queryDialect;
    start_query(session, queryExpression ? queryExpression : L"", callbacks, operation);
}

/* Operations we do not implement in v1 must still deliver one terminal result. */
static void deliver_not_supported(MI_OperationCallbacks *cbs, MI_Operation *operation)
{
    mi_operation_priv *p;
    memset(operation, 0, sizeof(*operation));
    operation->ft = &g_operation_ft;
    p = mi_alloc(sizeof(*p));
    operation->reserved2 = (ptrdiff_t)p;
    if (p) p->status = MI_RESULT_NOT_SUPPORTED;
    if (cbs && cbs->instanceResult)
        cbs->instanceResult(operation, cbs->callbackContext, NULL, MI_FALSE, MI_RESULT_NOT_SUPPORTED, NULL, NULL, ack_noop);
}
#define STUB_OP(name, ...) static void MI_CALL name(__VA_ARGS__)

static void MI_CALL ses_GetInstance(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Instance *in, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)in; deliver_not_supported(cb, op); }
static void MI_CALL ses_ModifyInstance(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Instance *in, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)in; deliver_not_supported(cb, op); }
static void MI_CALL ses_CreateInstance(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Instance *in, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)in; deliver_not_supported(cb, op); }
static void MI_CALL ses_DeleteInstance(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Instance *in, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)in; deliver_not_supported(cb, op); }
static void MI_CALL ses_Invoke(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Char *cl, const MI_Char *m, const MI_Instance *in, const MI_Instance *ip, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)cl;(void)m;(void)in;(void)ip; deliver_not_supported(cb, op); }
static void MI_CALL ses_AssociatorInstances(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Instance *ik, const MI_Char *ac, const MI_Char *rc, const MI_Char *ro, const MI_Char *rr, MI_Boolean ko, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)ik;(void)ac;(void)rc;(void)ro;(void)rr;(void)ko; deliver_not_supported(cb, op); }
static void MI_CALL ses_ReferenceInstances(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Instance *ik, const MI_Char *rc, const MI_Char *ro, MI_Boolean ko, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)ik;(void)rc;(void)ro;(void)ko; deliver_not_supported(cb, op); }
static void MI_CALL ses_Subscribe(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Char *qd, const MI_Char *qe, const MI_SubscriptionDeliveryOptions *do_, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)qd;(void)qe;(void)do_; deliver_not_supported(cb, op); }
static void MI_CALL ses_GetClass(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Char *cl, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)cl; deliver_not_supported(cb, op); }
static void MI_CALL ses_EnumerateClasses(MI_Session *s, MI_Uint32 f, MI_OperationOptions *o, const MI_Char *ns, const MI_Char *cl, MI_Boolean no, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f;(void)o;(void)ns;(void)cl;(void)no; deliver_not_supported(cb, op); }
static void MI_CALL ses_TestConnection(MI_Session *s, MI_Uint32 f, MI_OperationCallbacks *cb, MI_Operation *op)
{ (void)s;(void)f; deliver_not_supported(cb, op); }

static const MI_SessionFT g_session_ft =
{
    ses_Close, ses_GetApplication, ses_GetInstance, ses_ModifyInstance, ses_CreateInstance,
    ses_DeleteInstance, ses_Invoke, ses_EnumerateInstances, ses_QueryInstances, ses_AssociatorInstances,
    ses_ReferenceInstances, ses_Subscribe, ses_GetClass, ses_EnumerateClasses, ses_TestConnection
};

/* =====================================================================
 * Options FTs (accept-and-ignore)
 * ===================================================================== */
static void MI_CALL do_Delete(MI_DestinationOptions *o) { if (o) o->reserved2 = 0; }
static MI_Result MI_CALL do_ok_str(MI_DestinationOptions *o, const MI_Char *n, const MI_Char *v, MI_Uint32 f) { (void)o;(void)n;(void)v;(void)f; return MI_RESULT_OK; }
static MI_Result MI_CALL do_ok_num(MI_DestinationOptions *o, const MI_Char *n, MI_Uint32 v, MI_Uint32 f) { (void)o;(void)n;(void)v;(void)f; return MI_RESULT_OK; }
static MI_Result MI_CALL do_Clone(const MI_DestinationOptions *s, MI_DestinationOptions *n)
{ if (n) { memset(n, 0, sizeof(*n)); n->ft = s->ft; } return MI_RESULT_OK; }
static MI_Result MI_CALL do_AddCredentials(MI_DestinationOptions *o, const MI_Char *n, const MI_UserCredentials *c, MI_Uint32 f)
{ (void)o;(void)n;(void)c;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetString(MI_DestinationOptions *o, const MI_Char *n, const MI_Char **v, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetNumber(MI_DestinationOptions *o, const MI_Char *n, MI_Uint32 *v, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetOptionCount(MI_DestinationOptions *o, MI_Uint32 *c)
{ (void)o;(void)c; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetOptionAt(MI_DestinationOptions *o, MI_Uint32 i, const MI_Char **n, MI_Value *v, MI_Type *t, MI_Uint32 *f)
{ (void)o;(void)i;(void)n;(void)v;(void)t;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetOption(MI_DestinationOptions *o, const MI_Char *n, MI_Value *v, MI_Type *t, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)t;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetCredentialsCount(MI_DestinationOptions *o, MI_Uint32 *c)
{ (void)o;(void)c; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetCredentialsAt(MI_DestinationOptions *o, MI_Uint32 i, const MI_Char **n, MI_UserCredentials *c, MI_Uint32 *f)
{ (void)o;(void)i;(void)n;(void)c;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_GetCredentialsPasswordAt(MI_DestinationOptions *o, MI_Uint32 i, const MI_Char **n, MI_Char *pw, MI_Uint32 bl, MI_Uint32 *pl, MI_Uint32 *f)
{ (void)o;(void)i;(void)n;(void)pw;(void)bl;(void)pl;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL do_SetInterval(MI_DestinationOptions *o, const MI_Char *n, const MI_Interval *v, MI_Uint32 f)
{ (void)o;(void)n;(void)v;(void)f; return MI_RESULT_OK; }
static MI_Result MI_CALL do_GetInterval(MI_DestinationOptions *o, const MI_Char *n, MI_Interval *v, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }

static const MI_DestinationOptionsFT g_destination_options_ft =
{
    do_Delete, do_ok_str, do_ok_num, do_AddCredentials, do_GetString, do_GetNumber,
    do_GetOptionCount, do_GetOptionAt, do_GetOption, do_GetCredentialsCount,
    do_GetCredentialsAt, do_GetCredentialsPasswordAt, do_Clone, do_SetInterval, do_GetInterval
};

static void MI_CALL oo_Delete(MI_OperationOptions *o) { if (o) o->reserved2 = 0; }
static MI_Result MI_CALL oo_ok_str(MI_OperationOptions *o, const MI_Char *n, const MI_Char *v, MI_Uint32 f) { (void)o;(void)n;(void)v;(void)f; return MI_RESULT_OK; }
static MI_Result MI_CALL oo_ok_num(MI_OperationOptions *o, const MI_Char *n, MI_Uint32 v, MI_Uint32 f) { (void)o;(void)n;(void)v;(void)f; return MI_RESULT_OK; }
static MI_Result MI_CALL oo_Clone(const MI_OperationOptions *s, MI_OperationOptions *n)
{ if (n) { memset(n, 0, sizeof(*n)); n->ft = s->ft; } return MI_RESULT_OK; }
static MI_Result MI_CALL oo_SetCustomOption(MI_OperationOptions *o, const MI_Char *n, MI_Type t, const MI_Value *v, MI_Boolean mc, MI_Uint32 f)
{ (void)o;(void)n;(void)t;(void)v;(void)mc;(void)f; return MI_RESULT_OK; }
static MI_Result MI_CALL oo_GetString(MI_OperationOptions *o, const MI_Char *n, const MI_Char **v, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL oo_GetNumber(MI_OperationOptions *o, const MI_Char *n, MI_Uint32 *v, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL oo_GetOptionCount(MI_OperationOptions *o, MI_Uint32 *c)
{ (void)o;(void)c; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL oo_GetOptionAt(MI_OperationOptions *o, MI_Uint32 i, const MI_Char **n, MI_Value *v, MI_Type *t, MI_Uint32 *f)
{ (void)o;(void)i;(void)n;(void)v;(void)t;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL oo_GetOption(MI_OperationOptions *o, const MI_Char *n, MI_Value *v, MI_Type *t, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)t;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL oo_GetEnabledChannels(MI_OperationOptions *o, const MI_Char *n, MI_Uint32 *ch, MI_Uint32 bl, MI_Uint32 *cc, MI_Uint32 *f)
{ (void)o;(void)n;(void)ch;(void)bl;(void)cc;(void)f; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL oo_SetInterval(MI_OperationOptions *o, const MI_Char *n, const MI_Interval *v, MI_Uint32 f)
{ (void)o;(void)n;(void)v;(void)f; return MI_RESULT_OK; }
static MI_Result MI_CALL oo_GetInterval(MI_OperationOptions *o, const MI_Char *n, MI_Interval *v, MI_Uint32 *i, MI_Uint32 *f)
{ (void)o;(void)n;(void)v;(void)i;(void)f; return MI_RESULT_NOT_SUPPORTED; }

static const MI_OperationOptionsFT g_operation_options_ft =
{
    oo_Delete, oo_ok_str, oo_ok_num, oo_SetCustomOption, oo_GetString, oo_GetNumber,
    oo_GetOptionCount, oo_GetOptionAt, oo_GetOption, oo_GetEnabledChannels,
    oo_Clone, oo_SetInterval, oo_GetInterval
};

/* =====================================================================
 * Application FT
 * ===================================================================== */
static MI_Result MI_CALL app_Close(MI_Application *application) { if (application) memset(application, 0, sizeof(*application)); return MI_RESULT_OK; }

static MI_Result MI_CALL app_NewSession(MI_Application *application, const MI_Char *protocol, const MI_Char *destination,
    MI_DestinationOptions *options, MI_SessionCallbacks *callbacks, MI_Instance **extendedError, MI_Session *session)
{
    mi_session_priv *sp;
    IWbemLocator *loc = NULL;
    HRESULT hr;
    (void)application; (void)protocol; (void)destination; (void)options; (void)callbacks;
    if (extendedError) *extendedError = NULL;
    if (!session) return MI_RESULT_INVALID_PARAMETER;

    memset(session, 0, sizeof(*session));
    session->ft = &g_session_ft;
    sp = mi_alloc(sizeof(*sp));
    if (!sp) return MI_RESULT_FAILED;
    session->reserved2 = (ptrdiff_t)sp;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) sp->com_init = MI_TRUE;
    else if (hr != RPC_E_CHANGED_MODE) { /* keep going; apartment may already exist */ }

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (void**)&loc);
    if (FAILED(hr)) return MI_RESULT_FAILED;
    hr = IWbemLocator_ConnectServer(loc, (BSTR)L"ROOT\\CIMV2", NULL, NULL, NULL, 0, NULL, NULL, &sp->svc);
    IWbemLocator_Release(loc);
    if (FAILED(hr)) { sp->svc = NULL; return MI_RESULT_FAILED; }
    return MI_RESULT_OK;
}

static MI_Result MI_CALL app_NewDestinationOptions(MI_Application *application, MI_DestinationOptions *options)
{ (void)application; if (!options) return MI_RESULT_INVALID_PARAMETER; memset(options, 0, sizeof(*options)); options->ft = &g_destination_options_ft; return MI_RESULT_OK; }
static MI_Result MI_CALL app_NewOperationOptions(MI_Application *application, MI_Boolean must, MI_OperationOptions *options)
{ (void)application; (void)must; if (!options) return MI_RESULT_INVALID_PARAMETER; memset(options, 0, sizeof(*options)); options->ft = &g_operation_options_ft; return MI_RESULT_OK; }
static MI_Result MI_CALL app_NewHostedProvider(MI_Application *a, const MI_Char *ns, const MI_Char *pn, void *mn, MI_Instance **e, MI_HostedProvider *p)
{ (void)a;(void)ns;(void)pn;(void)mn;(void)e;(void)p; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL app_NewInstance(MI_Application *a, const MI_Char *cn, const MI_ClassDecl *rtti, MI_Instance **i)
{ (void)a;(void)cn;(void)rtti;(void)i; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL app_NewSubscriptionDeliveryOptions(MI_Application *a, MI_Uint32 dt, MI_SubscriptionDeliveryOptions *o)
{ (void)a;(void)dt;(void)o; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL app_NewSerializer(MI_Application *a, MI_Uint32 fl, MI_Char *fmt, MI_Serializer *s)
{ (void)a;(void)fl;(void)fmt;(void)s; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL app_NewDeserializer(MI_Application *a, MI_Uint32 fl, MI_Char *fmt, MI_Deserializer *d)
{ (void)a;(void)fl;(void)fmt;(void)d; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL app_NewInstanceFromClass(MI_Application *a, const MI_Char *cn, const MI_Class *co, MI_Instance **i)
{ (void)a;(void)cn;(void)co;(void)i; return MI_RESULT_NOT_SUPPORTED; }
static MI_Result MI_CALL app_NewClass(MI_Application *a, const MI_ClassDecl *cd, const MI_Char *ns, const MI_Char *sn, MI_Class **co)
{ (void)a;(void)cd;(void)ns;(void)sn;(void)co; return MI_RESULT_NOT_SUPPORTED; }

static const MI_ApplicationFT g_application_ft =
{
    app_Close, app_NewSession, app_NewHostedProvider, app_NewInstance,
    app_NewDestinationOptions, app_NewOperationOptions, app_NewSubscriptionDeliveryOptions,
    app_NewSerializer, app_NewDeserializer, app_NewInstanceFromClass, app_NewClass
};

static MI_Uint32 MI_CALL util_MapError(const MI_Char *errorType, MI_Uint32 error) { (void)errorType; (void)error; return 0; }
static MI_Result MI_CALL util_CimError(MI_Uint32 error, const MI_Char *errorType, const MI_Char *errorMessage, MI_Instance **cimError)
{ (void)error; (void)errorType; (void)errorMessage; if (cimError) *cimError = NULL; return MI_RESULT_NOT_SUPPORTED; }
static const MI_UtilitiesFT g_utilities_ft = { util_MapError, util_CimError };

/* =====================================================================
 * The client FT + exports
 * ===================================================================== */
static const MI_ClientFT_V1 g_client_ft =
{
    &g_application_ft,
    &g_session_ft,
    &g_operation_ft,
    NULL,  /* hostedProviderFT */
    NULL,  /* serializerFT */
    NULL,  /* deserializerFT */
    NULL,  /* subscribeDeliveryOptionsFT */
    &g_destination_options_ft,
    &g_operation_options_ft,
    &g_utilities_ft
};

const MI_ClientFT_V1 *mi_clientFT_V1 = &g_client_ft;

MI_Result MI_MAIN_CALL MI_Application_InitializeV1(MI_Uint32 flags, const MI_Char *applicationID,
                                                   MI_Instance **extendedError, MI_Application *application)
{
    (void)flags; (void)applicationID;
    if (extendedError) *extendedError = NULL;
    if (!application) return MI_RESULT_INVALID_PARAMETER;
    memset(application, 0, sizeof(*application));
    application->ft = &g_application_ft;
    return MI_RESULT_OK;
}

/* compile-time ABI guards (Dwight): natural alignment, documented sizes */
C_ASSERT(sizeof(MI_Datetime) == 36);
C_ASSERT(sizeof(MI_OperationCallbacks) == 9 * sizeof(void*));
C_ASSERT(sizeof(MI_ClientFT_V1) == 10 * sizeof(void*));
C_ASSERT(FIELD_OFFSET(MI_OperationCallbacks, instanceResult) == 5 * sizeof(void*));
C_ASSERT(FIELD_OFFSET(MI_Instance, nameSpace) == 3 * sizeof(void*));

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinst);
        break;
    }
    return TRUE;
}
