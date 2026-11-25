/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The host window: status, Welcome, spinner, the credential page, fade-out
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

#define LUH_CLASS           L"LiberNT_LogonUIHost"
#define LUH_TIMER_HEARTBEAT 1
#define LUH_TIMER_SPINNER   2
#define LUH_TIMER_FADE      3
#define LUH_TIMER_STARTUP   4   /* a page must arrive within the provider call bound */
#define LUH_SPINNER_MS      50

#define LUH_ID_FIELD_BASE   1000
#define LUH_ID_RESTART      1900
#define LUH_ID_SHUTDOWN     1901

#define LUH_PAGE_WIDTH      360
#define LUH_ROW_HEIGHT      30
#define LUH_ROW_GAP         10
#define LUH_TEXT_ABOVE_PAGE 100     /* title 40 + gap 8 + result 28 + 24 clearance over the first row */

typedef struct _LUH_UI
{
    HFONT TitleFont;
    HFONT TextFont;
    WCHAR Title[256];       /* "Welcome" user or status text */
    WCHAR Detail[256];      /* result text, when any */
    BOOL Spinner;
    int SpinnerAngle;
    BOOL FirstPaintDone;
    BOOL Fading;
    int FadeStep;
    WLUI_EXIT_REASON ExitReason;
    int ExitCode;
    BOOL Exiting;
    /* credential page */
    BOOL ProvidersStarted;
    BOOL InLegalNotice;     /* the notice box is up; its modal loop dispatches our commands */
    DWORD PendingScenario;  /* a RequestCredentials that arrived under the box */
    DWORD PendingFlags;
    PLUH_PAGE Page;         /* owned by providers.c; valid until the next WM_LUH_PAGE */
    int PageTop;            /* y of the first field row; the title and the result sit above it */
    BOOL PageBusy;          /* a submission is with winlogon; controls disabled */
    HWND PolicyButtons[2];
} LUH_UI;

static LUH_UI g_Ui;

static void
LuhBeginExit(
    _In_ HWND Window,
    _In_ WLUI_EXIT_REASON Reason,
    _In_ int ExitCode)
{
    if (g_Ui.Exiting)
        return;
    g_Ui.Exiting = TRUE;
    g_Ui.ExitReason = Reason;
    g_Ui.ExitCode = ExitCode;
    if (g_Ui.ProvidersStarted)
        LuhProviderDeselect();
    LuhIpcExiting(Reason);
    DestroyWindow(Window);
}

static void
LuhSetSpinner(
    _In_ HWND Window,
    _In_ BOOL On)
{
    if (On && !g_Ui.Spinner)
        SetTimer(Window, LUH_TIMER_SPINNER, LUH_SPINNER_MS, NULL);
    else if (!On && g_Ui.Spinner)
        KillTimer(Window, LUH_TIMER_SPINNER);
    g_Ui.Spinner = On;
}

/* ---- credential page ----------------------------------------------------------- */

static void
LuhDestroyPage(void)
{
    DWORD i;

    if (g_Ui.Page)
    {
        for (i = 0; i < g_Ui.Page->Count; i++)
        {
            if (g_Ui.Page->Fields[i].Control)
            {
                SetWindowTextW(g_Ui.Page->Fields[i].Control, L"");
                DestroyWindow(g_Ui.Page->Fields[i].Control);
                g_Ui.Page->Fields[i].Control = NULL;
            }
            if (g_Ui.Page->Fields[i].LabelControl)
            {
                DestroyWindow(g_Ui.Page->Fields[i].LabelControl);
                g_Ui.Page->Fields[i].LabelControl = NULL;
            }
        }
        /* The page came from the provider thread; the UI owns it from WM_LUH_PAGE on. */
        SecureZeroMemory(g_Ui.Page, sizeof(*g_Ui.Page));
        HeapFree(GetProcessHeap(), 0, g_Ui.Page);
        g_Ui.Page = NULL;
    }
    for (i = 0; i < 2; i++)
    {
        if (g_Ui.PolicyButtons[i])
        {
            DestroyWindow(g_Ui.PolicyButtons[i]);
            g_Ui.PolicyButtons[i] = NULL;
        }
    }
    g_Ui.PageBusy = FALSE;
}

static PLUH_FIELD
LuhFindField(
    _In_ DWORD Id)
{
    DWORD i;
    if (g_Ui.Page == NULL)
        return NULL;
    for (i = 0; i < g_Ui.Page->Count; i++)
        if (g_Ui.Page->Fields[i].Id == Id)
            return &g_Ui.Page->Fields[i];
    return NULL;
}

static BOOL
LuhFieldVisible(
    _In_ const LUH_FIELD *Field)
{
    return Field->State == CPFS_DISPLAY_IN_SELECTED_TILE || Field->State == CPFS_DISPLAY_IN_BOTH;
}

/* An edit field shows its label in a row of its own; the field itself starts with its value. */
static BOOL
LuhFieldHasLabelRow(
    _In_ const LUH_FIELD *Field)
{
    return (Field->Type == CPFT_EDIT_TEXT || Field->Type == CPFT_PASSWORD_TEXT) && Field->Label[0] != UNICODE_NULL;
}

/* Lays the visible fields out as a column of standard controls in the middle of the screen. */
static void
LuhCreatePage(
    _In_ HWND Window,
    _In_ PLUH_PAGE Page)
{
    RECT Client;
    int X, Y, Rows = 0;
    DWORD i;
    HWND Focus = NULL;
    WCHAR Num[16];

    LuhDestroyPage();
    g_Ui.Page = Page;
    GetClientRect(Window, &Client);

    for (i = 0; i < Page->Count; i++)
        if (LuhFieldVisible(&Page->Fields[i]) && Page->Fields[i].Type != CPFT_TILE_IMAGE)
            Rows += LuhFieldHasLabelRow(&Page->Fields[i]) ? 2 : 1;

    X = (Client.right - LUH_PAGE_WIDTH) / 2;
    Y = Client.bottom / 2 - (Rows * (LUH_ROW_HEIGHT + LUH_ROW_GAP)) / 2;
    g_Ui.PageTop = Y;

    for (i = 0; i < Page->Count; i++)
    {
        PLUH_FIELD Field = &Page->Fields[i];
        DWORD Style = WS_CHILD | WS_VISIBLE;
        PCWSTR Class = NULL;
        PCWSTR Text = Field->Value[0] ? Field->Value : Field->Label;

        if (!LuhFieldVisible(Field))
            continue;

        switch (Field->Type)
        {
            case CPFT_LARGE_TEXT:
            case CPFT_SMALL_TEXT:
                Class = L"STATIC";
                Style |= SS_CENTER;
                break;
            case CPFT_EDIT_TEXT:
                Class = L"EDIT";
                Style |= WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
                Text = Field->Value;    /* the label is its own row: as text it would be typed into */
                break;
            case CPFT_PASSWORD_TEXT:
                Class = L"EDIT";
                Style |= WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD;
                Text = L"";
                break;
            case CPFT_SUBMIT_BUTTON:
            case CPFT_COMMAND_LINK:
                Class = L"BUTTON";
                Style |= WS_TABSTOP | BS_PUSHBUTTON;
                break;
            case CPFT_CHECKBOX:
                Class = L"BUTTON";
                Style |= WS_TABSTOP | BS_AUTOCHECKBOX;
                break;
            default:
                continue;   /* tile images and combo boxes are Phase 4 */
        }

        if (LuhFieldHasLabelRow(Field))
        {
            Field->LabelControl = CreateWindowExW(0, L"STATIC", Field->Label, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                  X, Y, LUH_PAGE_WIDTH, LUH_ROW_HEIGHT, Window, NULL, NULL, NULL);
            if (Field->LabelControl)
                SendMessageW(Field->LabelControl, WM_SETFONT, (WPARAM)g_Ui.TextFont, TRUE);
            Y += LUH_ROW_HEIGHT + LUH_ROW_GAP;
        }
        Field->Control = CreateWindowExW(0, Class, Text, Style, X, Y, LUH_PAGE_WIDTH, LUH_ROW_HEIGHT,
                                         Window, (HMENU)(UINT_PTR)(LUH_ID_FIELD_BASE + i), NULL, NULL);
        if (Field->Control)
        {
            SendMessageW(Field->Control, WM_SETFONT, (WPARAM)g_Ui.TextFont, TRUE);
            if (Field->Type == CPFT_EDIT_TEXT || Field->Type == CPFT_PASSWORD_TEXT)
            {
                SendMessageW(Field->Control, EM_SETLIMITTEXT, LUH_FIELD_TEXT - 1, 0);
                if (Field->Interactive == CPFIS_DISABLED)
                    EnableWindow(Field->Control, FALSE);
                else if (Focus == NULL || Field->Interactive == CPFIS_FOCUSED)
                    Focus = Field->Control;
            }
        }
        Y += LUH_ROW_HEIGHT + LUH_ROW_GAP;
    }

    if (Focus)
        SetFocus(Focus);
    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%lu", Page->Count);
    LuhTrace("page", Num);
    InvalidateRect(Window, NULL, TRUE);
}

static void
LuhEnablePage(
    _In_ BOOL Enable)
{
    DWORD i;
    if (g_Ui.Page == NULL)
        return;
    for (i = 0; i < g_Ui.Page->Count; i++)
        if (g_Ui.Page->Fields[i].Control && g_Ui.Page->Fields[i].Interactive != CPFIS_DISABLED)
            EnableWindow(g_Ui.Page->Fields[i].Control, Enable);
    g_Ui.PageBusy = !Enable;
}

static void
LuhClearSecrets(void)
{
    DWORD i;
    if (g_Ui.Page == NULL)
        return;
    for (i = 0; i < g_Ui.Page->Count; i++)
        if (g_Ui.Page->Fields[i].Type == CPFT_PASSWORD_TEXT && g_Ui.Page->Fields[i].Control)
            SetWindowTextW(g_Ui.Page->Fields[i].Control, L"");
}

/* After a refused submit the keyboard goes to the first empty editable field (the cleared
 * password, usually), or to the first editable one; a page with no focus leaves the user typing
 * into nothing (leg finding 3b). */
static void
LuhFocusForRetry(void)
{
    HWND First = NULL;
    DWORD i;
    if (g_Ui.Page == NULL)
        return;
    for (i = 0; i < g_Ui.Page->Count; i++)
    {
        PLUH_FIELD Field = &g_Ui.Page->Fields[i];
        if (Field->Control == NULL || Field->Interactive == CPFIS_DISABLED ||
            (Field->Type != CPFT_EDIT_TEXT && Field->Type != CPFT_PASSWORD_TEXT))
            continue;
        if (First == NULL)
            First = Field->Control;
        if (GetWindowTextLengthW(Field->Control) == 0)
        {
            SetFocus(Field->Control);
            return;
        }
    }
    if (First)
        SetFocus(First);
}

/* The legal notice msgina showed before the logon dialog: Policies\System first, then the
 * Winlogon key; acknowledged before any credential page exists (winlogon sets the flag). */
static void
LuhLegalNotice(
    _In_ HWND Window)
{
    static const PCWSTR Keys[2] = { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon" };
    static const PCWSTR TextNames[2] = { L"legalnoticetext", L"LegalNoticeText" };
    static const PCWSTR CaptionNames[2] = { L"legalnoticecaption", L"LegalNoticeCaption" };
    WCHAR Caption[256];
    PWSTR Text = NULL;
    DWORD Size = 0, i;

    for (i = 0; i < 2 && Text == NULL; i++)
    {
        if (RegGetValueW(HKEY_LOCAL_MACHINE, Keys[i], TextNames[i], RRF_RT_REG_SZ, NULL, NULL, &Size) != ERROR_SUCCESS ||
            Size <= sizeof(WCHAR))
            continue;
        Text = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size + sizeof(WCHAR));
        if (Text == NULL)
            return;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, Keys[i], TextNames[i], RRF_RT_REG_SZ, NULL, Text, &Size) != ERROR_SUCCESS ||
            Text[0] == UNICODE_NULL)
        {
            HeapFree(GetProcessHeap(), 0, Text);
            Text = NULL;
            continue;
        }
        Size = sizeof(Caption);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, Keys[i], CaptionNames[i], RRF_RT_REG_SZ, NULL, Caption, &Size) != ERROR_SUCCESS)
            Caption[0] = UNICODE_NULL;
    }
    if (Text == NULL)
        return;
    LuhTrace("legal-notice", NULL);
    /* Modal on the UI thread: the heartbeat timer is still dispatched by the message box loop. */
    MessageBoxW(Window, Text, Caption[0] ? Caption : L"Notice", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    HeapFree(GetProcessHeap(), 0, Text);
}

static void
LuhShowPolicyPage(
    _In_ HWND Window)
{
    RECT Client;
    int X;

    LuhDestroyPage();
    GetClientRect(Window, &Client);
    StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), L"Sign-in is not available on this computer.");
    StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), L"Contact your administrator.");
    LuhSetSpinner(Window, FALSE);
    X = (Client.right - LUH_PAGE_WIDTH) / 2;
    /* ShutdownWithoutLogon=0: no power buttons; winlogon refuses the action anyway. */
    if (!g_Options.NoPower)
    {
        g_Ui.PolicyButtons[0] = CreateWindowExW(0, L"BUTTON", L"Restart", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                X, Client.bottom / 2 + 60, LUH_PAGE_WIDTH / 2 - 5, LUH_ROW_HEIGHT,
                                                Window, (HMENU)LUH_ID_RESTART, NULL, NULL);
        g_Ui.PolicyButtons[1] = CreateWindowExW(0, L"BUTTON", L"Shut down", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                X + LUH_PAGE_WIDTH / 2 + 5, Client.bottom / 2 + 60, LUH_PAGE_WIDTH / 2 - 5, LUH_ROW_HEIGHT,
                                                Window, (HMENU)LUH_ID_SHUTDOWN, NULL, NULL);
    }
    LuhTrace("policy-page", NULL);
    InvalidateRect(Window, NULL, TRUE);
}

/* Pushes every edit into the credential, then asks for the serialization. */
static void
LuhSubmit(
    _In_ HWND Window)
{
    DWORD i;
    WCHAR Text[LUH_FIELD_TEXT];
    WCHAR Status[LUH_FIELD_TEXT];
    LUH_SUBMIT_RESULT Result;

    if (g_Ui.Page == NULL || g_Ui.PageBusy)
        return;

    for (i = 0; i < g_Ui.Page->Count; i++)
    {
        PLUH_FIELD Field = &g_Ui.Page->Fields[i];
        if (Field->Control == NULL || (Field->Type != CPFT_EDIT_TEXT && Field->Type != CPFT_PASSWORD_TEXT))
            continue;
        GetWindowTextW(Field->Control, Text, ARRAYSIZE(Text));
        if (!LuhProviderSetString(Field->Id, Text))
        {
            SecureZeroMemory(Text, sizeof(Text));
            goto ProviderGone;
        }
        SecureZeroMemory(Text, sizeof(Text));
    }

    LuhEnablePage(FALSE);
    Result = LuhProviderSubmit(Status, ARRAYSIZE(Status));
    switch (Result)
    {
        case LuhSubmitSent:
            StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Status);
            LuhSetSpinner(Window, TRUE);
            break;                          /* ReportResult will re-enable the page */
        case LuhSubmitNotFinished:
        case LuhSubmitFailed:
        case LuhSubmitRefused:
            StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail),
                           Status[0] ? Status : L"Sign-in could not be completed. Try again.");
            LuhClearSecrets();
            LuhEnablePage(TRUE);
            break;
        case LuhSubmitProviderGone:
            goto ProviderGone;
    }
    InvalidateRect(Window, NULL, TRUE);
    return;

ProviderGone:
    /* providers.c already reported the fault and is re-enumerating; a new page will arrive. */
    LuhDestroyPage();
    StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), L"Please wait");
    g_Ui.Detail[0] = UNICODE_NULL;
    LuhSetSpinner(Window, TRUE);
    SetTimer(Window, LUH_TIMER_STARTUP, LUH_PROVIDER_CALL_MS, NULL);
    InvalidateRect(Window, NULL, TRUE);
}

static void
LuhApplyFieldUpdate(
    _In_ const LUH_FIELD_UPDATE *Update)
{
    PLUH_FIELD Field = LuhFindField(Update->Field);

    if (Field == NULL)
        return;
    switch (Update->Kind)
    {
        case LuhFieldText:
            StringCchCopyW(Field->Value, ARRAYSIZE(Field->Value), Update->Text);
            if (Field->Control)
                SetWindowTextW(Field->Control, Update->Text);
            break;
        case LuhFieldState:
            Field->State = Update->Value;
            if (Field->Control)
                ShowWindow(Field->Control, LuhFieldVisible(Field) ? SW_SHOW : SW_HIDE);
            break;
        case LuhFieldInteractive:
            Field->Interactive = Update->Value;
            if (Field->Control && !g_Ui.PageBusy)
                EnableWindow(Field->Control, Field->Interactive != CPFIS_DISABLED);
            break;
        case LuhFieldCheckbox:
            if (Field->Control)
            {
                SendMessageW(Field->Control, BM_SETCHECK, Update->Value ? BST_CHECKED : BST_UNCHECKED, 0);
                if (Update->Text[0])
                    SetWindowTextW(Field->Control, Update->Text);
            }
            break;
    }
}

/* ---- commands from winlogon ---------------------------------------------------- */

static void
LuhHandleCommand(
    _In_ HWND Window,
    _In_ const WLUI_COMMAND *Command)
{
    WCHAR Num[16];
    WCHAR Status[LUH_FIELD_TEXT];

    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%u", Command->Type);
    LuhTrace("command", Num);

    switch (Command->Type)
    {
        case WluiCmdDisplayStatus:
            StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), Command->Text);
            g_Ui.Detail[0] = UNICODE_NULL;
            LuhSetSpinner(Window, (Command->Flags & WLUI_FLAG_SPINNER) != 0);
            LuhTrace("status", g_Ui.Title);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdDisplayWelcome:
            LuhDestroyPage();
            StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), L"Welcome");
            StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Command->User);
            LuhSetSpinner(Window, TRUE);
            LuhTrace("welcome", g_Ui.Detail);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdReportResult:
            /* The provider owns the wording for its own result; winlogon's text is the fallback. */
            Status[0] = UNICODE_NULL;
            if (g_Ui.ProvidersStarted && LuhProviderReportResult(Command->Status, Command->SubStatus, Status, ARRAYSIZE(Status)) && Status[0])
                StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Status);
            else
                StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Command->Text);
            LuhSetSpinner(Window, FALSE);
            LuhClearSecrets();
            LuhEnablePage(TRUE);
            LuhFocusForRetry();
            LuhTrace("result", g_Ui.Detail);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdRequestCredentials:
            if (g_Ui.InLegalNotice)
            {
                /* Re-entered from the notice box's loop: keep the request for after the box. */
                g_Ui.PendingScenario = Command->Scenario;
                g_Ui.PendingFlags = Command->Flags;
                break;
            }
            g_Options.Scenario = Command->Scenario ? Command->Scenario : g_Options.Scenario;
            if (Command->Flags & WLUI_FLAG_FALLBACK)
                g_Options.Fallback = TRUE;
            g_Options.NoPower = (Command->Flags & WLUI_FLAG_NO_POWER) != 0;
            if ((Command->Flags & WLUI_FLAG_LEGAL_NOTICE) && !g_Ui.ProvidersStarted)
            {
                g_Ui.InLegalNotice = TRUE;
                LuhLegalNotice(Window);
                g_Ui.InLegalNotice = FALSE;
                if (g_Ui.Exiting)
                    break;              /* a Dismiss or the desktop switch arrived under the box */
                if (g_Ui.PendingScenario)
                {
                    g_Options.Scenario = g_Ui.PendingScenario;
                    if (g_Ui.PendingFlags & WLUI_FLAG_FALLBACK)
                        g_Options.Fallback = TRUE;
                    g_Options.NoPower = (g_Ui.PendingFlags & WLUI_FLAG_NO_POWER) != 0;
                    g_Ui.PendingScenario = 0;
                }
            }
            StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), L"");
            g_Ui.Detail[0] = UNICODE_NULL;
            LuhSetSpinner(Window, TRUE);
            if (!g_Ui.ProvidersStarted)
            {
                g_Ui.ProvidersStarted = LuhProvidersStart(Window);
                if (!g_Ui.ProvidersStarted)
                    LuhBeginExit(Window, WluiExitProviderInitFailed, LUH_EXIT_NOT_SUPPORTED);
                else
                    SetTimer(Window, LUH_TIMER_STARTUP, LUH_PROVIDER_CALL_MS, NULL);
            }
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdReadyForDesktopSwitch:
            /* The desktop behind is painted: fade out, then leave (§6.3). */
            if (!g_Ui.Fading)
            {
                g_Ui.Fading = TRUE;
                g_Ui.FadeStep = 0;
                LuhDestroyPage();
                LuhSetSpinner(Window, FALSE);
                SetTimer(Window, LUH_TIMER_FADE, LUH_FADE_MS / LUH_FADE_STEPS, NULL);
            }
            break;

        case WluiCmdDismiss:
            LuhBeginExit(Window, WluiExitDismissed, LUH_EXIT_OK);
            break;

        default:
            WARN("unknown command %u ignored\n", Command->Type);
            break;
    }
}

/* ---- painting --------------------------------------------------------------------- */

static void
LuhPaint(
    _In_ HWND Window)
{
    PAINTSTRUCT Ps;
    HDC Dc;
    RECT Client, Rect;
    HGDIOBJ OldFont;
    int TextTop;

    Dc = BeginPaint(Window, &Ps);
    GetClientRect(Window, &Client);

    FillRect(Dc, &Client, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, RGB(255, 255, 255));

    /* With a credential page up, the text sits above its first row (the page grows with its
     * label rows, a fixed offset put the result under the title); otherwise it is centred. */
    TextTop = g_Ui.Page ? g_Ui.PageTop - LUH_TEXT_ABOVE_PAGE : Client.bottom / 2 - 40;
    Rect = Client;
    Rect.top = TextTop;
    Rect.bottom = Rect.top + 40;
    OldFont = SelectObject(Dc, g_Ui.TitleFont);
    DrawTextW(Dc, g_Ui.Title, -1, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g_Ui.Detail[0] != UNICODE_NULL)
    {
        Rect.top = Rect.bottom + 8;
        Rect.bottom = Rect.top + 28;
        SelectObject(Dc, g_Ui.TextFont);
        DrawTextW(Dc, g_Ui.Detail, -1, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (g_Ui.Spinner)
    {
        int Cx = Client.right / 2, Cy = Rect.bottom + 48, i;
        for (i = 0; i < 8; i++)
        {
            double A = (g_Ui.SpinnerAngle + i * 45) * 3.14159265 / 180.0;
            int X = Cx + (int)(18.0 * cos(A)), Y = Cy + (int)(18.0 * sin(A));
            int Gray = 60 + i * 24;
            HBRUSH B = CreateSolidBrush(RGB(Gray, Gray, Gray));
            RECT Dot = { X - 3, Y - 3, X + 3, Y + 3 };
            FillRect(Dc, &Dot, B);
            DeleteObject(B);
        }
    }

    SelectObject(Dc, OldFont);
    EndPaint(Window, &Ps);

    if (!g_Ui.FirstPaintDone)
    {
        /* Ready goes out only after the first frame is on screen (§6.3). */
        g_Ui.FirstPaintDone = TRUE;
        GdiFlush();
        LuhIpcReady();
    }
}

static LRESULT CALLBACK
LuhWindowProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_CREATE:
            SetTimer(Window, LUH_TIMER_HEARTBEAT, LUH_HEARTBEAT_MS, NULL);
            return 0;

        case WM_PAINT:
            LuhPaint(Window);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wParam, RGB(255, 255, 255));
            SetBkColor((HDC)wParam, RGB(0, 0, 0));
            return (LRESULT)GetStockObject(BLACK_BRUSH);

        case WM_TIMER:
            if (wParam == LUH_TIMER_HEARTBEAT)
            {
                LuhIpcHeartbeat();
            }
            else if (wParam == LUH_TIMER_SPINNER)
            {
                g_Ui.SpinnerAngle = (g_Ui.SpinnerAngle + 45) % 360;
                InvalidateRect(Window, NULL, FALSE);
            }
            else if (wParam == LUH_TIMER_STARTUP)
            {
                /* Start-up wedged somewhere a CLSID cannot be named: one fallback try, then give up. */
                KillTimer(Window, LUH_TIMER_STARTUP);
                if (LuhProvidersStartupTimeout())
                    SetTimer(Window, LUH_TIMER_STARTUP, LUH_PROVIDER_CALL_MS, NULL);
                else
                    LuhBeginExit(Window, WluiExitProviderInitFailed, LUH_EXIT_NOT_SUPPORTED);
            }
            else if (wParam == LUH_TIMER_FADE)
            {
                g_Ui.FadeStep++;
                if (g_Ui.FadeStep >= LUH_FADE_STEPS)
                {
                    KillTimer(Window, LUH_TIMER_FADE);
                    LuhBeginExit(Window, WluiExitAfterDesktopSwitch, LUH_EXIT_OK);
                }
                else
                {
                    SetLayeredWindowAttributes(Window, 0,
                        (BYTE)(255 - (255 * g_Ui.FadeStep) / LUH_FADE_STEPS), LWA_ALPHA);
                }
            }
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                UINT Id = LOWORD(wParam);
                if (Id == LUH_ID_RESTART || Id == LUH_ID_SHUTDOWN)
                {
                    WluiClientRequestAction(&g_Connection,
                                            Id == LUH_ID_RESTART ? WluiActionRestart : WluiActionShutdown);
                }
                else if (Id >= LUH_ID_FIELD_BASE && g_Ui.Page && Id - LUH_ID_FIELD_BASE < g_Ui.Page->Count)
                {
                    PLUH_FIELD Field = &g_Ui.Page->Fields[Id - LUH_ID_FIELD_BASE];
                    if (Field->Type == CPFT_SUBMIT_BUTTON || Field->Type == CPFT_COMMAND_LINK)
                        LuhSubmit(Window);
                }
            }
            return 0;

        case WM_LUH_COMMAND:
        {
            WLUI_COMMAND *Command = (WLUI_COMMAND *)lParam;
            if (Command)
            {
                LuhHandleCommand(Window, Command);
                SecureZeroMemory(Command, sizeof(*Command));
                HeapFree(GetProcessHeap(), 0, Command);
            }
            return 0;
        }

        case WM_LUH_PAGE:
        {
            /* A slow start-up thread may still deliver after the UI moved on: its page is
             * dropped and the current thread's deadline is left running. */
            PVOID Owner = (wParam == LUH_PAGE_READY && lParam) ? ((PLUH_PAGE)lParam)->Owner : (PVOID)lParam;
            if (!LuhProvidersIsCurrent(Owner))
            {
                if (wParam == LUH_PAGE_READY && lParam)
                {
                    SecureZeroMemory((PVOID)lParam, sizeof(LUH_PAGE));
                    HeapFree(GetProcessHeap(), 0, (PVOID)lParam);
                }
                LuhTrace("page-dropped", NULL);
                return 0;
            }
            KillTimer(Window, LUH_TIMER_STARTUP);
            if (wParam == LUH_PAGE_READY && lParam)
            {
                LuhSetSpinner(Window, FALSE);
                LuhCreatePage(Window, (PLUH_PAGE)lParam);
            }
            else if (wParam == LUH_PAGE_POLICY)
            {
                LuhShowPolicyPage(Window);
            }
            else
            {
                LuhBeginExit(Window, WluiExitProviderInitFailed, LUH_EXIT_NOT_SUPPORTED);
            }
            return 0;
        }

        case WM_LUH_FIELD:
        {
            PLUH_FIELD_UPDATE Update = (PLUH_FIELD_UPDATE)lParam;
            if (Update)
            {
                /* An abandoned thread's credential may still post; only the current one may touch the page. */
                if (LuhProvidersIsCurrent(Update->Owner))
                    LuhApplyFieldUpdate(Update);
                SecureZeroMemory(Update, sizeof(*Update));
                HeapFree(GetProcessHeap(), 0, Update);
            }
            return 0;
        }

        case WM_LUH_IPC_LOST:
            /* 995 means winlogon ended this launch; anything else is a dead channel. */
            if (!g_Ui.Exiting)
            {
                g_Ui.Exiting = TRUE;
                g_Ui.ExitCode = (wParam == ERROR_OPERATION_ABORTED) ? LUH_EXIT_OK : LUH_EXIT_IPC_FAILED;
                DestroyWindow(Window);
            }
            return 0;

        case WM_CLOSE:
            /* Only winlogon closes this window, through Dismiss. */
            return 0;

        case WM_DESTROY:
            LuhDestroyPage();
            KillTimer(Window, LUH_TIMER_HEARTBEAT);
            KillTimer(Window, LUH_TIMER_SPINNER);
            KillTimer(Window, LUH_TIMER_FADE);
            KillTimer(Window, LUH_TIMER_STARTUP);
            PostQuitMessage(g_Ui.ExitCode);
            return 0;
    }
    return DefWindowProcW(Window, Message, wParam, lParam);
}

HWND
LuhUiCreate(
    _In_ HINSTANCE Instance)
{
    WNDCLASSEXW Class;
    HWND Window;
    int Width = GetSystemMetrics(SM_CXSCREEN), Height = GetSystemMetrics(SM_CYSCREEN);

    ZeroMemory(&g_Ui, sizeof(g_Ui));
    g_Ui.TitleFont = CreateFontW(-32, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_Ui.TextFont = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = LuhWindowProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.lpszClassName = LUH_CLASS;
    if (!RegisterClassExW(&Class))
        return NULL;

    /* Layered so the desktop-switch fade can drop the alpha; top-most on its desktop. */
    Window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, LUH_CLASS, L"",
                             WS_POPUP | WS_VISIBLE, 0, 0, Width, Height, NULL, NULL, Instance, NULL);
    if (Window == NULL)
        return NULL;
    SetLayeredWindowAttributes(Window, 0, 255, LWA_ALPHA);
    UpdateWindow(Window);
    return Window;
}

int
LuhUiRun(
    _In_ HWND Window)
{
    MSG Msg;

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        /* Enter in an edit field submits; Tab moves between the page's controls. */
        if (Msg.message == WM_KEYDOWN && Msg.wParam == VK_RETURN && g_Ui.Page && !g_Ui.PageBusy)
        {
            LuhSubmit(Window);
            continue;
        }
        if (g_Ui.Page && IsDialogMessageW(Window, &Msg))
            continue;
        TranslateMessage(&Msg);
        DispatchMessageW(&Msg);
    }

    if (g_Ui.ProvidersStarted)
        LuhProvidersStop();
    if (g_Ui.TitleFont) DeleteObject(g_Ui.TitleFont);
    if (g_Ui.TextFont) DeleteObject(g_Ui.TextFont);
    return (int)Msg.wParam;
}
