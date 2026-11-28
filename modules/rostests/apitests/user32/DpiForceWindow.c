/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Tests the forced per-window DPI test hook and notification order
 */

#include "precomp.h"

typedef BOOL (WINAPI *PFORCEWINDOWTODPIFORTEST)(HWND, UINT);

static UINT MessageLog[32];
static UINT MessageCount;
static HWND TrackedChild;
static HWND TrackedParent;
static UINT ChildDpiChangedCount;
static RECT SuggestedRect;
static BOOL SuggestedSeen;

static LRESULT CALLBACK
DpiWindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(hWnd);
    UNREFERENCED_PARAMETER(wParam);

    if (Msg == WM_GETDPISCALEDSIZE ||
        Msg == WM_DPICHANGED ||
        Msg == WM_DPICHANGED_BEFOREPARENT ||
        Msg == WM_DPICHANGED_AFTERPARENT)
    {
        if (MessageCount < ARRAYSIZE(MessageLog)) MessageLog[MessageCount++] = Msg;
    }
    if (hWnd == TrackedChild && Msg == WM_DPICHANGED) ChildDpiChangedCount++;
    if (hWnd == TrackedParent && Msg == WM_DPICHANGED)
    {
        SuggestedRect = *(RECT*)lParam;
        SuggestedSeen = TRUE;
    }

    return DefWindowProcW(hWnd, Msg, wParam, lParam);
}

static void
ExpectError(PFORCEWINDOWTODPIFORTEST ForceWindowToDpiForTest,
            HWND Window,
            UINT Dpi,
            DWORD ExpectedError)
{
    DWORD Error;
    BOOL Result;

    SetLastError(ERROR_SUCCESS);
    Result = ForceWindowToDpiForTest(Window, Dpi);
    Error = GetLastError();
    ok(!Result, "ForceWindowToDpiForTest(%p, %u) unexpectedly succeeded\n", Window, Dpi);
    ok(Error == ExpectedError,
       "ForceWindowToDpiForTest(%p, %u) returned error %lu instead of %lu\n",
       Window, Dpi, Error, ExpectedError);
}

static HWND
CreateDpiWindow(HINSTANCE Instance, LPCWSTR ClassName, HWND Parent, DWORD Style)
{
    return CreateWindowExW(0, ClassName, L"DPI test", Style,
                           CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
                           Parent, NULL, Instance, NULL);
}

static void
TestNotificationOrder(PFORCEWINDOWTODPIFORTEST ForceWindowToDpiForTest,
                       HINSTANCE Instance,
                       LPCWSTR ClassName,
                       DPI_AWARENESS_CONTEXT Context)
{
    DPI_AWARENESS_CONTEXT PreviousContext;
    HWND Parent, Child;
    UINT Dpi;
    UINT OldDpi;
    RECT BeforeRect, AfterRect, ChildBefore, ChildAfter;
    BOOL Result;

    PreviousContext = SetThreadDpiAwarenessContext(Context);
    if (PreviousContext == NULL)
    {
        skip("SetThreadDpiAwarenessContext does not support this context\n");
        return;
    }

    Parent = CreateDpiWindow(Instance, ClassName, NULL, WS_OVERLAPPEDWINDOW);
    Child = CreateDpiWindow(Instance, ClassName, Parent, WS_CHILD | WS_VISIBLE);
    ok(Parent != NULL && Child != NULL, "Could not create the DPI test window pair\n");
    if (Parent == NULL || Child == NULL) goto cleanup;

    Dpi = GetDpiForWindow(Parent);
    if (Dpi < 96 || Dpi >= 480) Dpi = 120;
    else Dpi += 24;

    MessageCount = 0;
    ChildDpiChangedCount = 0;
    TrackedChild = Child;
    TrackedParent = Parent;
    SuggestedSeen = FALSE;
    GetWindowRect(Parent, &BeforeRect);
    GetWindowRect(Child, &ChildBefore);
    OldDpi = GetDpiForWindow(Parent);
    if (!OldDpi) OldDpi = 96;
    ok(DefWindowProcW(Parent, WM_DPICHANGED, Dpi, (LPARAM)1) == 0,
       "DefWindowProcW dereferenced an invalid WM_DPICHANGED lParam\n");
    Result = ForceWindowToDpiForTest(Parent, Dpi);
    ok(Result, "ForceWindowToDpiForTest failed for context %p, error %lu\n", Context, GetLastError());
    if (Result && Context == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)
    {
        ok(MessageCount >= 1 && MessageLog[0] == WM_DPICHANGED,
           "PMv1 message order started with %#x, count %u\n",
           MessageCount ? MessageLog[0] : 0, MessageCount);
        GetWindowRect(Parent, &AfterRect);
        ok(SuggestedSeen, "PMv1 window did not receive a suggested rect\n");
        ok(abs((SuggestedRect.right - SuggestedRect.left) - MulDiv(BeforeRect.right - BeforeRect.left, Dpi, OldDpi)) <= 1 &&
           abs((SuggestedRect.bottom - SuggestedRect.top) - MulDiv(BeforeRect.bottom - BeforeRect.top, Dpi, OldDpi)) <= 1,
           "PMv1 suggested size was not linearly scaled (%ldx%ld -> %ldx%ld)\n",
           BeforeRect.right - BeforeRect.left, BeforeRect.bottom - BeforeRect.top,
           SuggestedRect.right - SuggestedRect.left, SuggestedRect.bottom - SuggestedRect.top);
        ok(SuggestedRect.left == BeforeRect.left && SuggestedRect.top == BeforeRect.top,
           "PMv1 suggested position moved from (%ld,%ld) to (%ld,%ld)\n",
           BeforeRect.left, BeforeRect.top, SuggestedRect.left, SuggestedRect.top);
        ok(EqualRect(&BeforeRect, &AfterRect),
           "PMv1 window rect changed (%ldx%ld -> %ldx%ld)\n",
           BeforeRect.right - BeforeRect.left, BeforeRect.bottom - BeforeRect.top,
           AfterRect.right - AfterRect.left, AfterRect.bottom - AfterRect.top);
        GetWindowRect(Child, &ChildAfter);
        ok(ChildDpiChangedCount == 0, "PMv1 child received WM_DPICHANGED\n");
        ok(EqualRect(&ChildBefore, &ChildAfter), "PMv1 child position changed\n");
    }
    else if (Result)
    {
        ok(MessageCount >= 3 && MessageLog[0] == WM_GETDPISCALEDSIZE &&
           MessageLog[1] == WM_DPICHANGED_BEFOREPARENT &&
           MessageLog[2] == WM_DPICHANGED,
           "PMv2 parent message order was %#x, %#x, %#x (count %u)\n",
           MessageCount > 0 ? MessageLog[0] : 0,
           MessageCount > 1 ? MessageLog[1] : 0,
           MessageCount > 2 ? MessageLog[2] : 0,
           MessageCount);
        ok(MessageCount >= 4 && MessageLog[MessageCount - 1] == WM_DPICHANGED_AFTERPARENT,
           "PMv2 child AFTERPARENT notification was not last (count %u)\n", MessageCount);
    }

cleanup:
    if (Child != NULL) DestroyWindow(Child);
    if (Parent != NULL) DestroyWindow(Parent);
    TrackedChild = NULL;
    TrackedParent = NULL;
    SetThreadDpiAwarenessContext(PreviousContext);
}

START_TEST(DpiForceWindow)
{
    HMODULE Win32u;
    PFORCEWINDOWTODPIFORTEST ForceWindowToDpiForTest;
    WNDCLASSW Class;
    WCHAR ClassName[] = L"DpiForceWindowClass";
    HINSTANCE Instance = GetModuleHandleW(NULL);
    ATOM Atom;

    Win32u = LoadLibraryA("win32u.dll");
    if (Win32u == NULL)
    {
        skip("win32u.dll is not available\n");
        return;
    }
    ForceWindowToDpiForTest = (PFORCEWINDOWTODPIFORTEST)GetProcAddress(Win32u, "NtUserForceWindowToDpiForTest");
    if (ForceWindowToDpiForTest == NULL)
    {
        skip("NtUserForceWindowToDpiForTest is not exported\n");
        FreeLibrary(Win32u);
        return;
    }

    ExpectError(ForceWindowToDpiForTest, NULL, 120, ERROR_INVALID_WINDOW_HANDLE);
    ExpectError(ForceWindowToDpiForTest, GetDesktopWindow(), 95, ERROR_INVALID_PARAMETER);
    ExpectError(ForceWindowToDpiForTest, GetDesktopWindow(), 120, ERROR_ACCESS_DENIED);

    ZeroMemory(&Class, sizeof(Class));
    Class.lpfnWndProc = DpiWindowProc;
    Class.hInstance = Instance;
    Class.lpszClassName = ClassName;
    Atom = RegisterClassW(&Class);
    ok(Atom != 0, "RegisterClassW failed, error %lu\n", GetLastError());
    if (Atom != 0)
    {
        TestNotificationOrder(ForceWindowToDpiForTest, Instance, ClassName,
                              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        TestNotificationOrder(ForceWindowToDpiForTest, Instance, ClassName,
                              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        UnregisterClassW(ClassName, Instance);
    }

    FreeLibrary(Win32u);
}
