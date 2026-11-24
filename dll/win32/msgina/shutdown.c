/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS msgina.dll
 * FILE:            lib/msgina/shutdown.c
 * PURPOSE:         Shutdown options and the shell shutdown entry points
 * PROGRAMMERS:     Lee Schroeder (spaceseel at gmail dot com)
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 *                  Arnav Bhatt (arnavbhatt288 at gmail dot com)
 *                  Ahmed ARIF <arif.ing@outlook.com>
 */

#include "msgina.h"
#include <ndk/pofuncs.h>
#include <wingdi.h>
#include <objbase.h>
#include "logonui.h"

/**
 * @brief
 * Looks up a policy value on both HKEY_LOCAL_MACHINE
 * and HKEY_CURRENT_USER, under the same policy sub-key.
 *
 * @note
 * HKEY_LOCAL_MACHINE policies trump over HKEY_CURRENT_USER ones.
 **/
LONG
GetPolicyDWORDValue(
    _In_opt_ HKEY hKeyCurrentUser,
    _In_ PCWSTR pPolicyKey,
    _In_ PCWSTR ValueName,
    _Out_ PDWORD pValue)
{
    HKEY hKey;
    DWORD dwValue;
    LONG rc;

    *pValue = 0;

    /* Query the policy value in HKEY_LOCAL_MACHINE */
    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, pPolicyKey, 0,
                       KEY_QUERY_VALUE, &hKey);
    if (rc == ERROR_SUCCESS)
    {
        rc = ReadRegDwordValue(hKey, ValueName, &dwValue);
        RegCloseKey(hKey);
        if (rc == ERROR_SUCCESS)
        {
            *pValue = dwValue;
            return ERROR_SUCCESS;
        }
    }

    /* If not found, query the policy value in HKEY_CURRENT_USER */
    rc = RegOpenKeyExW(hKeyCurrentUser ? hKeyCurrentUser : HKEY_CURRENT_USER,
                       pPolicyKey, 0,
                       KEY_QUERY_VALUE, &hKey);
    if (rc == ERROR_SUCCESS)
    {
        rc = ReadRegDwordValue(hKey, ValueName, &dwValue);
        RegCloseKey(hKey);
        if (rc == ERROR_SUCCESS)
        {
            *pValue = dwValue;
            return ERROR_SUCCESS;
        }
    }

    return ERROR_NOT_FOUND;
}

static
BOOL
IsFriendlyUIActive(VOID)
{
    DWORD dwType, dwValue, dwSize;
    HKEY hKey;
    LONG lRet;

    lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Control\\Windows",
                         0,
                         KEY_QUERY_VALUE,
                         &hKey);
    if (lRet != ERROR_SUCCESS)
        return FALSE;

    /* CORE-17282 First check an optional ReactOS specific override, that Windows does not check.
       We use this to allow users pairing 'Server'-configuration with FriendlyShutdown.
       Otherwise users would have to change CSDVersion or LogonType (side-effects AppCompat) */
    dwValue = 0;
    dwSize = sizeof(dwValue);
    lRet = RegQueryValueExW(hKey,
                            L"EnforceFriendlyShutdown",
                            NULL,
                            &dwType,
                            (LPBYTE)&dwValue,
                            &dwSize);

    if (lRet == ERROR_SUCCESS && dwType == REG_DWORD && dwValue == 0x1)
    {
        RegCloseKey(hKey);
        return TRUE;
    }

    /* Check product version number */
    dwValue = 0;
    dwSize = sizeof(dwValue);
    lRet = RegQueryValueExW(hKey,
                            L"CSDVersion",
                            NULL,
                            &dwType,
                            (LPBYTE)&dwValue,
                            &dwSize);
    RegCloseKey(hKey);

    if (lRet != ERROR_SUCCESS || dwType != REG_DWORD || dwValue != 0x300)
    {
        /* Allow Friendly UI only on Workstation */
        return FALSE;
    }

    /* Check LogonType value */
    lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                         L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                         0,
                         KEY_QUERY_VALUE,
                         &hKey);
    if (lRet != ERROR_SUCCESS)
        return FALSE;

    dwValue = 0;
    dwSize = sizeof(dwValue);
    lRet = RegQueryValueExW(hKey,
                            L"LogonType",
                            NULL,
                            &dwType,
                            (LPBYTE)&dwValue,
                            &dwSize);
    RegCloseKey(hKey);

    if (lRet != ERROR_SUCCESS || dwType != REG_DWORD)
        return FALSE;

    return (dwValue != 0);
}

static
BOOL
IsDomainMember(VOID)
{
    UNIMPLEMENTED;
    return FALSE;
}

static
BOOL
IsNetwareActive(VOID)
{
    UNIMPLEMENTED;
    return FALSE;
}

static
BOOL
ForceFriendlyUI(VOID)
{
    DWORD dwType, dwValue, dwSize;
    HKEY hKey;
    LONG lRet;

    lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                         L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                         0,
                         KEY_QUERY_VALUE,
                         &hKey);
    if (lRet == ERROR_SUCCESS)
    {
        dwValue = 0;
        dwSize = sizeof(dwValue);
        lRet = RegQueryValueExW(hKey,
                                L"ForceFriendlyUI",
                                NULL,
                                &dwType,
                                (LPBYTE)&dwValue,
                                &dwSize);
        RegCloseKey(hKey);

        if (lRet == ERROR_SUCCESS && dwType == REG_DWORD)
            return (dwValue != 0);
    }

    lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                         L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                         0,
                         KEY_QUERY_VALUE,
                         &hKey);
    if (lRet == ERROR_SUCCESS)
    {
        dwValue = 0;
        dwSize = sizeof(dwValue);
        lRet = RegQueryValueExW(hKey,
                                L"ForceFriendlyUI",
                                NULL,
                                &dwType,
                                (LPBYTE)&dwValue,
                                &dwSize);

        RegCloseKey(hKey);

        if (lRet == ERROR_SUCCESS && dwType == REG_DWORD)
            return (dwValue != 0);
    }

    return FALSE;
}

BOOL
WINAPI
ShellIsFriendlyUIActive(VOID)
{
    BOOL bActive;

    bActive = IsFriendlyUIActive();

    if ((IsDomainMember() || IsNetwareActive()) && !ForceFriendlyUI())
        return FALSE;

    return bActive;
}

DWORD
LoadShutdownSelState(
    _In_ HKEY hKeyCurrentUser)
{
    HKEY hKey;
    DWORD dwValue, dwTemp, dwSize;
    LONG lRet;

    /* Default to first item */
    dwValue = 0;

    /* Open the subkey */
    lRet = RegOpenKeyExW(hKeyCurrentUser,
                         L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                         0, KEY_QUERY_VALUE, &hKey);
    if (lRet != ERROR_SUCCESS)
        return dwValue;

    /* Read the value */
    dwSize = sizeof(dwTemp);
    lRet = RegQueryValueExW(hKey,
                            L"Shutdown Setting",
                            NULL, NULL,
                            (LPBYTE)&dwTemp, &dwSize);
    RegCloseKey(hKey);

    if (lRet == ERROR_SUCCESS)
    {
        switch (dwTemp)
        {
            case WLX_SHUTDOWN_STATE_LOGOFF:
                dwValue = WLX_SAS_ACTION_LOGOFF;
                break;

            case WLX_SHUTDOWN_STATE_POWER_OFF:
                dwValue = WLX_SAS_ACTION_SHUTDOWN_POWER_OFF;
                break;

            case WLX_SHUTDOWN_STATE_REBOOT:
                dwValue = WLX_SAS_ACTION_SHUTDOWN_REBOOT;
                break;

            // 0x08

            case WLX_SHUTDOWN_STATE_SLEEP:
                dwValue = WLX_SAS_ACTION_SHUTDOWN_SLEEP;
                break;

            case WLX_SHUTDOWN_STATE_SLEEP2:
                dwValue = WLX_SAS_ACTION_SHUTDOWN_SLEEP2;
                break;

            case WLX_SHUTDOWN_STATE_HIBERNATE:
                dwValue = WLX_SAS_ACTION_SHUTDOWN_HIBERNATE;
                break;

            // WLX_SHUTDOWN_STATE_DISCONNECT
        }
    }

    return dwValue;
}

VOID
SaveShutdownSelState(
    _In_ HKEY hKeyCurrentUser,
    _In_ DWORD ShutdownCode)
{
    HKEY hKey;
    DWORD dwValue = 0;
    LONG lRet;

    /* Create the subkey */
    lRet = RegCreateKeyExW(hKeyCurrentUser,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer",
                           0, NULL,
                           REG_OPTION_NON_VOLATILE,
                           KEY_SET_VALUE,
                           NULL, &hKey, NULL);
    if (lRet != ERROR_SUCCESS)
        return;

    switch (ShutdownCode)
    {
        case WLX_SAS_ACTION_LOGOFF:
            dwValue = WLX_SHUTDOWN_STATE_LOGOFF;
            break;

        case WLX_SAS_ACTION_SHUTDOWN_POWER_OFF:
            dwValue = WLX_SHUTDOWN_STATE_POWER_OFF;
            break;

        case WLX_SAS_ACTION_SHUTDOWN_REBOOT:
            dwValue = WLX_SHUTDOWN_STATE_REBOOT;
            break;

        case WLX_SAS_ACTION_SHUTDOWN_SLEEP:
            dwValue = WLX_SHUTDOWN_STATE_SLEEP;
            break;

        case WLX_SAS_ACTION_SHUTDOWN_HIBERNATE:
            dwValue = WLX_SHUTDOWN_STATE_HIBERNATE;
            break;
    }

    RegSetValueExW(hKey,
                   L"Shutdown Setting",
                   0, REG_DWORD,
                   (LPBYTE)&dwValue, sizeof(dwValue));
    RegCloseKey(hKey);
}

DWORD
GetAllowedShutdownOptions(
    _In_opt_ HKEY hKeyCurrentUser,
    _In_opt_ HANDLE hUserToken)
{
    DWORD Options = 0;
    DWORD dwPolicyValue;
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS Status;

    dwPolicyValue = 0;
    GetPolicyDWORDValue(hKeyCurrentUser,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                        L"NoLogoff", &dwPolicyValue);
    if (dwPolicyValue == 0)
        Options |= WLX_SHUTDOWN_STATE_LOGOFF;

#if 0 // TODO: Enable and lookup on a Terminal Services session
    dwPolicyValue = 0;
    GetPolicyDWORDValue(hKeyCurrentUser,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
                        L"NoDisconnect", &dwPolicyValue);
    if (dwPolicyValue == 0)
        Options |= WLX_SHUTDOWN_STATE_DISCONNECT;
#endif

    /* Add shutdown options only if the user has shutdown privilege */
    if (!TestTokenPrivilege(hUserToken, SE_SHUTDOWN_PRIVILEGE))
        return Options; // The user doesn't have them, bail out.

    /* We can always shutdown and restart */
    Options |= WLX_SHUTDOWN_STATE_POWER_OFF | WLX_SHUTDOWN_STATE_REBOOT;

    /* Determine whether extra power options are available */
    Status = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (!NT_SUCCESS(Status))
    {
        ERR("NtPowerInformation(SystemPowerCapabilities) failed (Status 0x%08lx)\n", Status);
    }
    else
    {
        if (IS_PWR_SUSPEND_ALLOWED(&PowerCaps))
            Options |= WLX_SHUTDOWN_STATE_SLEEP;

        // TODO: "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power" value "Shutdown"
        // for sleep options. In particular, if set it tells that wakeup events can be disabled.
        // This will enable WLX_SHUTDOWN_STATE_SLEEP2 support.

        if (IS_PWR_HIBERNATE_ALLOWED(&PowerCaps))
            Options |= WLX_SHUTDOWN_STATE_HIBERNATE;
    }

    // TODO: Consider Windows 8+ support for:
    // "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power" value "HiberbootEnabled"

    // TODO: Add WLX_SHUTDOWN_AUTOUPDATE if:
    // - Policy "Software\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU" value "NoAUShutdownOption"
    //   is absent or zero;
    // - SystemBatteryState detects the computer is on external power;
    // - wuaueng.dll!ordinal_4 tells there are pending updates.

    return Options;
}

#define DIM_SCREEN_ALPHA 150

static const WCHAR DimScreenClassName[] = L"MsginaDimScreen";

static LRESULT CALLBACK
DimScreenWndProc(
    _In_ HWND hWnd,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (uMsg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hDC = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hDC, &rc, GetStockObject(BLACK_BRUSH));
        EndPaint(hWnd, &ps);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static BOOL
EnsureDimScreenClass(VOID)
{
    WNDCLASSW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DimScreenWndProc;
    wc.hInstance = hDllInstance;
    wc.lpszClassName = DimScreenClassName;
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);

    if (RegisterClassW(&wc))
        return TRUE;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/* Fade handler: an IUnknown whose last release tears the dim window down. */
typedef struct _DIM_FADE_HANDLER
{
    const IUnknownVtbl *Vtbl;
    LONG RefCount;
    HWND hWnd;
} DIM_FADE_HANDLER, *PDIM_FADE_HANDLER;

static HRESULT STDMETHODCALLTYPE
DimFadeQueryInterface(
    _In_ IUnknown *This,
    _In_ REFIID riid,
    _Out_ void **ppvObject)
{
    if (IsEqualIID(riid, &IID_IUnknown))
    {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
DimFadeAddRef(
    _In_ IUnknown *This)
{
    return InterlockedIncrement(&((PDIM_FADE_HANDLER)This)->RefCount);
}

static ULONG STDMETHODCALLTYPE
DimFadeRelease(
    _In_ IUnknown *This)
{
    PDIM_FADE_HANDLER Handler = (PDIM_FADE_HANDLER)This;
    LONG RefCount = InterlockedDecrement(&Handler->RefCount);

    if (RefCount == 0)
    {
        if (Handler->hWnd)
            DestroyWindow(Handler->hWnd);
        HeapFree(GetProcessHeap(), 0, Handler);
    }
    return RefCount;
}

static const IUnknownVtbl DimFadeVtbl =
{
    DimFadeQueryInterface,
    DimFadeAddRef,
    DimFadeRelease
};

HRESULT
WINAPI
ShellDimScreen(
    _Out_ IUnknown **ppUnknown,
    _Out_ HWND *phWindow)
{
    PDIM_FADE_HANDLER Handler;
    HWND hWnd;
    INT x, y, cx, cy;

    if (!ppUnknown || !phWindow)
        return E_POINTER;

    *ppUnknown = NULL;
    *phWindow = NULL;

    if (!EnsureDimScreenClass())
        return E_FAIL;

    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           DimScreenClassName, NULL, WS_POPUP,
                           x, y, cx, cy,
                           NULL, NULL, hDllInstance, NULL);
    if (!hWnd)
        return HRESULT_FROM_WIN32(GetLastError());

    /* Semi-transparent black overlay dims the desktop behind the dialog. */
    SetLayeredWindowAttributes(hWnd, 0, DIM_SCREEN_ALPHA, LWA_ALPHA);
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    Handler = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Handler));
    if (!Handler)
    {
        DestroyWindow(hWnd);
        return E_OUTOFMEMORY;
    }

    Handler->Vtbl = &DimFadeVtbl;
    Handler->RefCount = 1;
    Handler->hWnd = hWnd;

    *ppUnknown = (IUnknown *)Handler;
    *phWindow = hWnd;
    return S_OK;
}

DWORD WINAPI
ShellShutdownDialog(
    _In_ HWND hWndParent,
    _In_opt_ LPCWSTR pUserName,
    _In_ DWORD dwExcludeOptions)
{
    DWORD ShutdownOptions, Action, Result;
    IUnknown *FadeHandler = NULL;
    HWND hDimWindow = NULL;

    UNREFERENCED_PARAMETER(pUserName);

    ShutdownOptions = GetAllowedShutdownOptions(HKEY_CURRENT_USER, NULL) & ~dwExcludeOptions;
    Action = LoadShutdownSelState(HKEY_CURRENT_USER);
    if (Action == 0)
        Action = WLX_SAS_ACTION_SHUTDOWN_POWER_OFF;

    /* Dim the desktop behind the dialog; the dim window owns the dialog so it
     * stays above the topmost overlay. Falls back to no dim on failure. */
    if (SUCCEEDED(ShellDimScreen(&FadeHandler, &hDimWindow)))
        Result = LogonUiPowerDialog(hDimWindow, ShutdownOptions, &Action);
    else
        Result = LogonUiPowerDialog(hWndParent, ShutdownOptions, &Action);

    if (FadeHandler)
        FadeHandler->lpVtbl->Release(FadeHandler);

    if (Result)
        SaveShutdownSelState(HKEY_CURRENT_USER, Action);

    return Result;
}

DWORD WINAPI
ShellTurnOffDialog(HWND hWnd)
{
    return ShellShutdownDialog(hWnd, NULL, 0);
}
