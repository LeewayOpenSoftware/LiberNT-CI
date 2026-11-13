/*
 * ribbon-f2 diagnostic self-test (G5/mtrace): trigger a SetLayout failure whose
 * layout references a command id not in the table, and show that the framework now
 * names the exact failing id + HRESULT via OutputDebugString (captured in-process
 * through the DBWIN buffer), instead of failing opaquely. Build-time test only.
 */
#include <windows.h>
#include <leewayui/ribbon.h>
#include <stdio.h>
#include <string.h>

/* Minimal in-process capture of OutputDebugStringA/W (no debugger needed). */
static HANDLE g_evReady, g_evData, g_map; static void *g_buf;
static void capture_begin(void)
{
    g_evReady = CreateEventW(NULL, FALSE, FALSE, L"DBWIN_BUFFER_READY");
    g_evData  = CreateEventW(NULL, FALSE, FALSE, L"DBWIN_DATA_READY");
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, L"DBWIN_BUFFER");
    g_buf = MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, 4096);
    SetEvent(g_evReady);
}
/* Drain pending debug strings for up to timeout_ms; print each. */
static void capture_drain(int timeout_ms)
{
    for (;;) {
        DWORD w = WaitForSingleObject(g_evData, timeout_ms);
        if (w != WAIT_OBJECT_0) break;
        const char *s = (const char *)g_buf + sizeof(DWORD); /* [pid][ansi string] */
        printf("  [ODS] %s", s);
        if (s[0] && s[strlen(s)-1] != '\n') printf("\n");
        SetEvent(g_evReady);
    }
}

static HRESULT CALLBACK ex(UINT32 id, void *c){ (void)id;(void)c; return S_OK; }
static HRESULT CALLBACK st(UINT32 id, LeewayUICommandState *s, void *c){ (void)id;(void)c; s->enabled=TRUE; return S_OK; }

int main(void)
{
    capture_begin();

    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"DiagParent";
    RegisterClassW(&wc);
    HWND parent = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0,0,400,300, NULL,NULL, wc.hInstance, NULL);

    /* A command table WITHOUT id 9999. */
    LeewayUICommand table[] = {
        { 100, L"New", 0,0, 0,1,0,1 },
        { 101, L"Open",0,0, 0,1,0,1 },
    };
    ILeewayRibbon *r = NULL;
    HRESULT hc = LeewayRibbonCreate(parent, table, 2, ex, st, NULL, &r);
    printf("LeewayRibbonCreate hr=0x%08X ribbon=%p\n", (unsigned)hc, (void*)r);

    /* Layout whose app menu references id 9999 (NOT in the table) -> must fail,
     * and the framework must name 0x270F (9999) in the debug log. */
    UINT32 appmenu[] = { 100, 9999 };
    LeewayUITab tabs[] = { { 1, L"Home", L"H", 0, 0, NULL } };
    LeewayUILayout bad = {}; bad.app_name = L"DiagTest"; bad.tab_count = 1; bad.tabs = tabs;
    bad.appmenu_count = 2; bad.appmenu_ids = appmenu;

    printf("SetLayout with app-menu id 9999 (0x270F) missing from the table:\n");
    HRESULT hr = LeewayRibbonSetLayout(r, &bad);
    capture_drain(400);
    printf("SetLayout hr=0x%08X (LWUI_E_UNKNOWN_COMMAND = 0x%08X)\n",
           (unsigned)hr, (unsigned)MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0202));

    /* A good layout succeeds. */
    UINT32 appmenu_ok[] = { 100, 101 };
    LeewayUILayout ok = {}; ok.app_name = L"DiagTest"; ok.tab_count = 1; ok.tabs = tabs;
    ok.appmenu_count = 2; ok.appmenu_ids = appmenu_ok;
    HRESULT hr2 = LeewayRibbonSetLayout(r, &ok);
    capture_drain(200);
    printf("SetLayout (valid) hr=0x%08X\n", (unsigned)hr2);

    if (r) LeewayRibbonDestroy(r);
    return (hr != S_OK && hr2 == S_OK) ? 0 : 1;
}
