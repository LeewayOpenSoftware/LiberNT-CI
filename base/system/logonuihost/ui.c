/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The host window: status, Welcome, spinner, the credential page, fade-out
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include "resource.h"
#include "gfx.h"
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

#define LUH_CLASS           L"LiberNT_LogonUIHost"
#define LUH_TIMER_HEARTBEAT 1
#define LUH_TIMER_SPINNER   2
#define LUH_TIMER_FADE      3
#define LUH_TIMER_STARTUP   4   /* a page must arrive within the provider call bound */
#define LUH_TIMER_CLOCK     5   /* the curtain's clock, only while the curtain is up */
#define LUH_SPINNER_MS      50

#define LUH_ID_FIELD_BASE   1000
#define LUH_ID_RESTART      1900
#define LUH_ID_SHUTDOWN     1901
#define LUH_ID_POWER        1902    /* the power button: a menu of LUH_ID_RESTART and LUH_ID_SHUTDOWN */
#define LUH_ID_OPTION_BASE  1920    /* + row of the Ctrl+Alt+Del options (LuhOptionRows) */
#define LUH_ID_OPTION_CANCEL 1930
#define LUH_POWER_WIDTH     120
#define LUH_ID_TILE_BASE    2000    /* + tile index, below LUH_ID_TILE_BASE + LUH_MAX_TILES */

#define LUH_TILE_WIDTH      260
#define LUH_TILE_MARGIN     24

#define LUH_PAGE_WIDTH      360
#define LUH_ROW_HEIGHT      30
#define LUH_ROW_GAP         10
#define LUH_TEXT_ABOVE_PAGE 100     /* title 40 + gap 8 + result 28 + 24 clearance over the first row */
#define LUH_DETAIL_WIDTH    560     /* a wrapped result line, e.g. the account restriction text */
#define LUH_DETAIL_MARGIN   16
#define LUH_CAPS_GAP        28      /* under the last password box: the Caps Lock line */

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
    HWND TileButtons[LUH_MAX_TILES];    /* the other tiles, bottom left, as on the Windows sign-in screen */
    HWND PowerButton;                   /* bottom right; absent when ShutdownWithoutLogon is 0 */
    BOOL OptionsUp;                     /* the Ctrl+Alt+Del options of a signed-in user are shown */
    HWND OptionButtons[5];              /* the options shown, then Cancel */
    BOOL ChangeCancel;                  /* a change the user asked for (not forced): it can be cancelled */
    HWND CancelButton;                  /* that Cancel, under the change page */
    /* the curtain: clock and date before the tiles, CAD-less logon and unlock only */
    BOOL Curtain;
    BOOL CadCurtain;        /* the Ctrl+Alt+Del notice: Title is its text and only winlogon replaces it */
    PLUH_PAGE CurtainPage;  /* a page that arrived under the curtain; shown when it lifts */
    HFONT ClockFont;
    HFONT DateFont;
    WCHAR ClockText[64];
    WCHAR DateText[128];
    HWND CapsControl;       /* the focused password box while Caps Lock is on */
    int CapsTop;            /* y of the gap reserved under the page's last password box */
    /* drawing: built once per screen size, then only blitted */
    LUH_SURFACE Back;       /* the frame being composed */
    LUH_SURFACE Sharp;      /* the wallpaper, filled to the screen: the curtain */
    LUH_SURFACE Soft;       /* the same, blurred and darkened once: behind every other page */
    LUH_SURFACE Face;       /* Sharp with the clock and date: rebuilt once a minute */
    LUH_SURFACE Still;      /* Soft with the page's text: rebuilt only when that text changes */
    WCHAR StillKey[640];    /* what Still shows */
    LONG SpinnerY;          /* the spinner's centre under the text in Still */
    HBRUSH SoftBrush;       /* Soft as a pattern, behind static controls */
    LONG Lift;              /* how far the curtain is pulled up, in pixels */
    BOOL Dragging;
    LONG DragStartY;
    LONG DragLastY;
    DWORD DragLastTime;
    double DragVelocity;    /* pixels per millisecond, positive upwards */
    LUH_ANIM DragAnim;
} LUH_UI;

static LUH_UI g_Ui;

/* The UI text, from the string table in the user's language (en-US, es-ES in the tree);
 * loaded once per string on the UI thread. */
static PCWSTR
LuhText(
    _In_ UINT Id)
{
    static WCHAR Cache[IDS_LUH_LAST - IDS_LUH_POWER + 1][256];
    PWSTR Slot;

    if (Id < IDS_LUH_POWER || Id > IDS_LUH_LAST)
        return L"";
    Slot = Cache[Id - IDS_LUH_POWER];
    if (Slot[0] == UNICODE_NULL && LoadStringW(GetModuleHandleW(NULL), Id, Slot, ARRAYSIZE(Cache[0])) <= 0)
        Slot[0] = UNICODE_NULL;
    return Slot;
}

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

/* Account restrictions the provider leaves unworded: LogonUI says them itself, as on Windows. */
#define LUH_STATUS_ACCOUNT_RESTRICTION  ((LONG)0xC000006EL)
#define LUH_STATUS_INVALID_LOGON_HOURS  ((LONG)0xC000006FL)
#define LUH_STATUS_INVALID_WORKSTATION  ((LONG)0xC0000070L)
#define LUH_STATUS_ACCOUNT_DISABLED     ((LONG)0xC0000072L)
#define LUH_STATUS_ACCOUNT_EXPIRED      ((LONG)0xC0000193L)
#define LUH_STATUS_ACCOUNT_LOCKED_OUT   ((LONG)0xC0000234L)

static UINT
LuhAccountStatusCode(
    _In_ LONG Code)
{
    switch (Code)
    {
        case LUH_STATUS_ACCOUNT_DISABLED:   return IDS_LUH_ACCOUNT_DISABLED;
        case LUH_STATUS_ACCOUNT_EXPIRED:    return IDS_LUH_ACCOUNT_EXPIRED;
        case LUH_STATUS_INVALID_LOGON_HOURS: return IDS_LUH_LOGON_HOURS;
        case LUH_STATUS_INVALID_WORKSTATION: return IDS_LUH_WORKSTATION;
        case LUH_STATUS_ACCOUNT_LOCKED_OUT: return IDS_LUH_LOCKED_OUT;
        case LUH_STATUS_ACCOUNT_RESTRICTION: return IDS_LUH_ACCOUNT_RESTRICTED;
    }
    return 0;
}

static UINT
LuhAccountStatusText(
    _In_ LONG Status,
    _In_ LONG SubStatus)
{
    UINT Text = 0;

    /* The sub-status says which restriction; one without its own sentence gets the general one. */
    if (Status == LUH_STATUS_ACCOUNT_RESTRICTION && SubStatus < 0)
        Text = LuhAccountStatusCode(SubStatus);
    if (Text == 0)
        Text = LuhAccountStatusCode(Status);
    return Text;
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

/* Caps Lock warning, as on the Windows sign-in screen: only while a password box has the focus. */
static void
LuhUpdateCapsLock(
    _In_ HWND Window)
{
    HWND Focus = GetFocus(), Show = NULL;
    DWORD i;

    if (g_Ui.Page && !g_Ui.Curtain && Focus != NULL && (GetKeyState(VK_CAPITAL) & 1))
    {
        for (i = 0; i < g_Ui.Page->Count; i++)
        {
            if (g_Ui.Page->Fields[i].Control == Focus && g_Ui.Page->Fields[i].Type == CPFT_PASSWORD_TEXT)
            {
                Show = Focus;
                break;
            }
        }
    }
    if (Show != g_Ui.CapsControl)
    {
        g_Ui.CapsControl = Show;
        InvalidateRect(Window, NULL, TRUE);
    }
}

static void
LuhDestroyPage(void)
{
    DWORD i;

    g_Ui.CapsControl = NULL;    /* its box goes away with the page */
    g_Ui.CapsTop = 0;
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
    for (i = 0; i < LUH_MAX_TILES; i++)
    {
        if (g_Ui.TileButtons[i])
        {
            DestroyWindow(g_Ui.TileButtons[i]);
            g_Ui.TileButtons[i] = NULL;
        }
    }
    if (g_Ui.PowerButton)
    {
        DestroyWindow(g_Ui.PowerButton);
        g_Ui.PowerButton = NULL;
    }
    for (i = 0; i < ARRAYSIZE(g_Ui.OptionButtons); i++)
    {
        if (g_Ui.OptionButtons[i])
        {
            DestroyWindow(g_Ui.OptionButtons[i]);
            g_Ui.OptionButtons[i] = NULL;
        }
    }
    g_Ui.OptionsUp = FALSE;
    if (g_Ui.CancelButton)
    {
        DestroyWindow(g_Ui.CancelButton);
        g_Ui.CancelButton = NULL;
    }
    g_Ui.PageBusy = FALSE;
}

/*
 * The power button of the sign-in screen. Hiding it is only the visible half: winlogon's
 * action handler refuses Restart and Shut down itself while ShutdownWithoutLogon is 0.
 */
static void
LuhCreatePowerButton(
    _In_ HWND Window)
{
    RECT Client;

    if (g_Options.NoPower)
        return;
    GetClientRect(Window, &Client);
    g_Ui.PowerButton = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_POWER), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                       Client.right - LUH_TILE_MARGIN - LUH_POWER_WIDTH,
                                       Client.bottom - LUH_TILE_MARGIN - LUH_ROW_HEIGHT,
                                       LUH_POWER_WIDTH, LUH_ROW_HEIGHT, Window, (HMENU)LUH_ID_POWER, NULL, NULL);
    if (g_Ui.PowerButton)
        SendMessageW(g_Ui.PowerButton, WM_SETFONT, (WPARAM)g_Ui.TextFont, TRUE);
}

/* Restart and Shut down, above the button; the choice goes to winlogon, which decides. */
static void
LuhPowerMenu(
    _In_ HWND Window)
{
    HMENU Menu;
    RECT Button;
    UINT Choice;

    if (g_Ui.PowerButton == NULL || g_Ui.PageBusy)
        return;
    if (g_Ui.OptionsUp && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        if (MessageBoxW(Window, LuhText(IDS_LUH_EMERGENCY_TEXT), LuhText(IDS_LUH_EMERGENCY_TITLE),
                        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDOK)
        {
            LuhTrace("power", L"emergency");
            WluiClientRequestAction(&g_Connection, WluiActionEmergencyRestart);
        }
        return;
    }
    Menu = CreatePopupMenu();
    if (Menu == NULL)
        return;
    AppendMenuW(Menu, MF_STRING, LUH_ID_RESTART, LuhText(IDS_LUH_RESTART));
    AppendMenuW(Menu, MF_STRING, LUH_ID_SHUTDOWN, LuhText(IDS_LUH_SHUTDOWN));
    GetWindowRect(g_Ui.PowerButton, &Button);
    Choice = (UINT)TrackPopupMenu(Menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY,
                                  Button.right, Button.top, 0, Window, NULL);
    DestroyMenu(Menu);
    if (Choice == LUH_ID_RESTART || Choice == LUH_ID_SHUTDOWN)
    {
        LuhTrace("power", Choice == LUH_ID_RESTART ? L"restart" : L"shutdown");
        WluiClientRequestAction(&g_Connection, Choice == LUH_ID_RESTART ? WluiActionRestart : WluiActionShutdown);
    }
}

static void LuhDropCurtain(_In_ HWND Window);

/* The Ctrl+Alt+Del options, in msgina's order; a row policy turned off is not shown. */
static const struct
{
    UINT Text;
    DWORD Hide;
    WLUI_ACTION Action;
} LuhOptionRows[] =
{
    { IDS_LUH_LOCK, WLUI_FLAG_NO_LOCK, WluiActionLock },
    { IDS_LUH_SIGN_OUT, WLUI_FLAG_NO_LOGOFF, WluiActionLogoff },
    { IDS_LUH_CHANGE_PASSWORD, WLUI_FLAG_NO_CHANGE_PASSWORD, WluiActionChangePassword },
    { IDS_LUH_TASK_MANAGER, WLUI_FLAG_NO_TASK_MANAGER, WluiActionTaskManager },
};

static void
LuhShowOptionsPage(
    _In_ HWND Window,
    _In_ DWORD Flags,
    _In_ PCWSTR User)
{
    RECT Client;
    int X, Y;
    DWORD i, Shown = 0;

    LuhDropCurtain(Window);
    LuhDestroyPage();
    LuhSetSpinner(Window, FALSE);
    GetClientRect(Window, &Client);
    StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), User);
    g_Ui.Detail[0] = UNICODE_NULL;
    X = (Client.right - LUH_PAGE_WIDTH) / 2;
    Y = Client.bottom / 2 - 2 * (LUH_ROW_HEIGHT + LUH_ROW_GAP);
    g_Ui.PageTop = Y;
    for (i = 0; i < ARRAYSIZE(LuhOptionRows); i++)
    {
        if (Flags & LuhOptionRows[i].Hide)
            continue;
        g_Ui.OptionButtons[Shown] = CreateWindowExW(0, L"BUTTON", LuhText(LuhOptionRows[i].Text),
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                    X, Y, LUH_PAGE_WIDTH, LUH_ROW_HEIGHT, Window,
                                                    (HMENU)(UINT_PTR)(LUH_ID_OPTION_BASE + i), NULL, NULL);
        if (g_Ui.OptionButtons[Shown])
            SendMessageW(g_Ui.OptionButtons[Shown++], WM_SETFONT, (WPARAM)g_Ui.TextFont, TRUE);
        Y += LUH_ROW_HEIGHT + LUH_ROW_GAP;
    }
    Y += LUH_ROW_HEIGHT;
    g_Ui.OptionButtons[Shown] = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_CANCEL),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                X + LUH_PAGE_WIDTH / 2, Y, LUH_PAGE_WIDTH / 2, LUH_ROW_HEIGHT, Window,
                                                (HMENU)LUH_ID_OPTION_CANCEL, NULL, NULL);
    if (g_Ui.OptionButtons[Shown])
        SendMessageW(g_Ui.OptionButtons[Shown], WM_SETFONT, (WPARAM)g_Ui.TextFont, TRUE);
    g_Options.NoPower = (Flags & WLUI_FLAG_NO_POWER) != 0;
    LuhCreatePowerButton(Window);
    g_Ui.OptionsUp = TRUE;
    SetFocus(g_Ui.OptionButtons[0] ? g_Ui.OptionButtons[0] : Window);
    LuhTrace("options", NULL);
    InvalidateRect(Window, NULL, TRUE);
}

/* An option picked (or Cancel): winlogon decides and takes the host down; no second pick. */
static void
LuhPickOption(
    _In_ WLUI_ACTION Action)
{
    DWORD i;
    WCHAR Num[16];

    if (!g_Ui.OptionsUp && !(g_Ui.ChangeCancel && Action == WluiActionCancel))
        return;
    for (i = 0; i < ARRAYSIZE(g_Ui.OptionButtons); i++)
        if (g_Ui.OptionButtons[i])
            EnableWindow(g_Ui.OptionButtons[i], FALSE);
    if (g_Ui.CancelButton)
        EnableWindow(g_Ui.CancelButton, FALSE);
    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%u", (UINT)Action);
    LuhTrace("option", Num);
    WluiClientRequestAction(&g_Connection, Action);
}

/* With more than one tile, every tile is listed bottom left; the selected one is checked. */
static void
LuhCreateTileList(
    _In_ HWND Window,
    _In_ PLUH_PAGE Page)
{
    RECT Client;
    DWORD i, Count = min(Page->TileCount, (DWORD)LUH_MAX_TILES);
    int Y;

    if (Count < 2)
        return;
    GetClientRect(Window, &Client);
    Y = Client.bottom - LUH_TILE_MARGIN - (int)Count * (LUH_ROW_HEIGHT + LUH_ROW_GAP);
    for (i = 0; i < Count; i++)
    {
        g_Ui.TileButtons[i] = CreateWindowExW(0, L"BUTTON", Page->TileNames[i],
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE | BS_LEFT,
                                              LUH_TILE_MARGIN, Y, LUH_TILE_WIDTH, LUH_ROW_HEIGHT, Window,
                                              (HMENU)(UINT_PTR)(LUH_ID_TILE_BASE + i), NULL, NULL);
        if (g_Ui.TileButtons[i])
        {
            SendMessageW(g_Ui.TileButtons[i], WM_SETFONT, (WPARAM)g_Ui.TextFont, TRUE);
            if (i == Page->TileSelected)
                SendMessageW(g_Ui.TileButtons[i], BM_SETCHECK, BST_CHECKED, 0);
        }
        Y += LUH_ROW_HEIGHT + LUH_ROW_GAP;
    }
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
    DWORD i, LastPassword = (DWORD)-1;
    HWND Focus = NULL;
    WCHAR Num[16];

    LuhDestroyPage();
    g_Ui.Page = Page;
    GetClientRect(Window, &Client);

    for (i = 0; i < Page->Count; i++)
        if (LuhFieldVisible(&Page->Fields[i]) && Page->Fields[i].Type != CPFT_TILE_IMAGE)
            Rows += LuhFieldHasLabelRow(&Page->Fields[i]) ? 2 : 1;
    /* Only a page with a password box makes room for the Caps Lock line, under its last one. */
    for (i = 0; i < Page->Count; i++)
        if (LuhFieldVisible(&Page->Fields[i]) && Page->Fields[i].Type == CPFT_PASSWORD_TEXT)
            LastPassword = i;

    X = (Client.right - LUH_PAGE_WIDTH) / 2;
    Y = Client.bottom / 2 - (Rows * (LUH_ROW_HEIGHT + LUH_ROW_GAP) + (LastPassword != (DWORD)-1 ? LUH_CAPS_GAP : 0)) / 2;
    g_Ui.PageTop = Y;
    g_Ui.CapsTop = 0;

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
        if (i == LastPassword)
        {
            g_Ui.CapsTop = Y - LUH_ROW_GAP + 2;
            Y += LUH_CAPS_GAP;
        }
    }

    if (g_Ui.ChangeCancel && Page->Scenario == CPUS_CHANGE_PASSWORD)
    {
        g_Ui.CancelButton = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_CANCEL),
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                            X + LUH_PAGE_WIDTH / 2, Y, LUH_PAGE_WIDTH / 2, LUH_ROW_HEIGHT, Window,
                                            (HMENU)LUH_ID_OPTION_CANCEL, NULL, NULL);
        if (g_Ui.CancelButton)
            SendMessageW(g_Ui.CancelButton, WM_SETFONT, (WPARAM)g_Ui.TextFont, TRUE);
    }
    LuhCreateTileList(Window, Page);
    LuhCreatePowerButton(Window);
    if (Focus)
        SetFocus(Focus);
    LuhUpdateCapsLock(Window);
    StringCchPrintfW(Num, ARRAYSIZE(Num), L"%lu", Page->Count);
    LuhTrace("page", Num);
    if (Page->TileCount > 1)
    {
        StringCchPrintfW(Num, ARRAYSIZE(Num), L"%lu/%lu", Page->TileSelected, Page->TileCount);
        LuhTrace("tiles", Num);
    }
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
    /* No switching tiles and no power menu while a submission is with winlogon. */
    for (i = 0; i < LUH_MAX_TILES; i++)
        if (g_Ui.TileButtons[i])
            EnableWindow(g_Ui.TileButtons[i], Enable);
    if (g_Ui.PowerButton)
        EnableWindow(g_Ui.PowerButton, Enable);
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
    MessageBoxW(Window, Text, Caption[0] ? Caption : LuhText(IDS_LUH_NOTICE), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
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
    StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), LuhText(IDS_LUH_NO_SIGNIN));
    StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), LuhText(IDS_LUH_CONTACT_ADMIN));
    LuhSetSpinner(Window, FALSE);
    X = (Client.right - LUH_PAGE_WIDTH) / 2;
    /* ShutdownWithoutLogon=0: no power buttons; winlogon refuses the action anyway. */
    if (!g_Options.NoPower)
    {
        g_Ui.PolicyButtons[0] = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_RESTART), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                                X, Client.bottom / 2 + 60, LUH_PAGE_WIDTH / 2 - 5, LUH_ROW_HEIGHT,
                                                Window, (HMENU)LUH_ID_RESTART, NULL, NULL);
        g_Ui.PolicyButtons[1] = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_SHUTDOWN), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
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
                           Status[0] ? Status : LuhText(IDS_LUH_SIGNIN_FAILED));
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
    StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), LuhText(IDS_LUH_PLEASE_WAIT));
    g_Ui.Detail[0] = UNICODE_NULL;
    LuhSetSpinner(Window, TRUE);
    SetTimer(Window, LUH_TIMER_STARTUP, LUH_PROVIDER_CALL_MS, NULL);
    InvalidateRect(Window, NULL, TRUE);
}

/* Another tile picked: the provider thread switches and posts the new page (WM_LUH_PAGE). */
static void
LuhSelectTile(
    _In_ HWND Window,
    _In_ DWORD Index)
{
    if (g_Ui.Page == NULL || g_Ui.PageBusy || Index >= g_Ui.Page->TileCount || Index == g_Ui.Page->TileSelected)
        return;
    g_Ui.Detail[0] = UNICODE_NULL;
    if (!LuhProviderSelectTile(Index))
    {
        /* Refused: keep the page, put the check back on the selected tile. */
        if (g_Ui.TileButtons[Index])
            SendMessageW(g_Ui.TileButtons[Index], BM_SETCHECK, BST_UNCHECKED, 0);
        if (g_Ui.Page && g_Ui.Page->TileSelected < LUH_MAX_TILES && g_Ui.TileButtons[g_Ui.Page->TileSelected])
            SendMessageW(g_Ui.TileButtons[g_Ui.Page->TileSelected], BM_SETCHECK, BST_CHECKED, 0);
    }
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

/* ---- the curtain ------------------------------------------------------------------ */

/* ---- backdrop and lock screen ----------------------------------------------------- */

#define LUH_SOFT_SCALE      8       /* the blur runs on an eighth of the screen, then scales up */
#define LUH_SOFT_RADIUS     3
#define LUH_SOFT_DARKEN     96      /* about 38 % black over the blurred wallpaper (Win11 shots) */
#define LUH_LIFT_MS         350     /* a whole-screen lift; a partial one takes its share */
#define LUH_LIFT_MIN_MS     120
#define LUH_LIFT_VELOCITY   0.6     /* pixels per millisecond upwards at release: a flick lifts */

/*
 * Built once per launch: the wallpaper of the logon session filled to the screen (the
 * curtain), and the same picture blurred and darkened (behind every other page).
 */
static BOOL
LuhBuildBackdrop(
    _In_ LONG Width,
    _In_ LONG Height)
{
    WCHAR Path[MAX_PATH];
    LUH_SURFACE Image, Small;
    RECT All = { 0, 0, Width, Height };

    if (!LuhSurfaceCreate(&g_Ui.Back, Width, Height) || !LuhSurfaceCreate(&g_Ui.Sharp, Width, Height) ||
        !LuhSurfaceCreate(&g_Ui.Still, Width, Height) ||
        !LuhSurfaceCreate(&g_Ui.Soft, Width, Height) || !LuhSurfaceCreate(&g_Ui.Face, Width, Height) ||
        !LuhSurfaceCreate(&Small, max(Width / LUH_SOFT_SCALE, 1), max(Height / LUH_SOFT_SCALE, 1)))
    {
        return FALSE;
    }

    Path[0] = UNICODE_NULL;
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(Path), Path, 0) && Path[0] != UNICODE_NULL &&
        LuhSurfaceLoad(&Image, Path))
    {
        LuhSurfaceScaleFrom(&g_Ui.Sharp, &Image);
        LuhSurfaceDestroy(&Image);
    }
    else
    {
        /* The picture was removed or cannot be read: a plain deep blue is still a lock screen. */
        LuhFillRect(&g_Ui.Sharp, &All, RGB(0, 40, 120), 255);
    }

    LuhSurfaceScaleFrom(&Small, &g_Ui.Sharp);
    LuhBlur(&Small, LUH_SOFT_RADIUS);
    LuhSurfaceScaleFrom(&g_Ui.Soft, &Small);
    LuhSurfaceDestroy(&Small);
    LuhFillRect(&g_Ui.Soft, &All, RGB(0, 0, 0), LUH_SOFT_DARKEN);

    g_Ui.SoftBrush = CreatePatternBrush(g_Ui.Soft.Bitmap);
    return g_Ui.SoftBrush != NULL;
}

/* The curtain picture: sharp wallpaper, clock and date centred near the top, as on Windows 11. */
static void
LuhBuildFace(void)
{
    LONG Width = g_Ui.Face.Width, Height = g_Ui.Face.Height, Y;
    RECT Rect;

    BitBlt(g_Ui.Face.Dc, 0, 0, Width, Height, g_Ui.Sharp.Dc, 0, 0, SRCCOPY);

    /* Measured at 1024x768: clock centre y 180, date centre y 256; kept in proportion. */
    Y = MulDiv(Height, 180, 768);
    SetRect(&Rect, 0, Y - LuhPx(72), Width, Y + LuhPx(72));
    LuhDrawText(&g_Ui.Face, g_Ui.ClockFont, g_Ui.ClockText, &Rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 255, 255), 255);
    Y = MulDiv(Height, 256, 768);
    SetRect(&Rect, 0, Y - LuhPx(20), Width, Y + LuhPx(20));
    LuhDrawText(&g_Ui.Face, g_Ui.DateFont, g_Ui.DateText, &Rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, RGB(255, 255, 255), 255);

    /* The Ctrl+Alt+Del notice is the one curtain with words: winlogon's prompt, near the bottom. */
    if (g_Ui.CadCurtain)
    {
        SetRect(&Rect, 0, Height - LuhPx(104), Width, Height - LuhPx(56));
        LuhDrawText(&g_Ui.Face, g_Ui.TextFont, g_Ui.Title, &Rect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, RGB(255, 255, 255), 255);
    }
}

/* Once a minute, just after it turns: nothing else wakes the host while the curtain is idle. */
static void
LuhArmClock(
    _In_ HWND Window)
{
    SYSTEMTIME Now;

    GetLocalTime(&Now);
    SetTimer(Window, LUH_TIMER_CLOCK, (60 - Now.wSecond) * 1000 - Now.wMilliseconds + 50, NULL);
}

/* The curtain pulled up by Lift: its lower part at the top, the soft backdrop showing below it. */
static void
LuhBlitLift(
    _In_ HDC Dc,
    _In_ LONG Lift)
{
    LONG Width = g_Ui.Face.Width, Height = g_Ui.Face.Height;

    if (Lift < Height)
        BitBlt(Dc, 0, 0, Width, Height - Lift, g_Ui.Face.Dc, 0, Lift, SRCCOPY);
    if (Lift > 0)
        BitBlt(Dc, 0, Height - Lift, Width, Lift, g_Ui.Soft.Dc, 0, Height - Lift, SRCCOPY);
}

static void
LuhPresentLift(
    _In_ HWND Window,
    _In_ LONG Lift)
{
    HDC Dc;

    g_Ui.Lift = min(max(Lift, 0), g_Ui.Face.Height);
    Dc = GetDC(Window);
    if (Dc == NULL)
        return;
    LuhBlitLift(Dc, g_Ui.Lift);
    GdiFlush();
    ReleaseDC(Window, Dc);
}

/*
 * Move the curtain to To (0 down, the screen height gone) on the clock with an ease-out.
 * A release velocity shortens the run so the motion carries on at the finger's speed.
 */
static void
LuhSlideCurtain(
    _In_ HWND Window,
    _In_ LONG To,
    _In_ double Velocity,
    _In_ PCSTR Name)
{
    LONG From = g_Ui.Lift, Distance = To > From ? To - From : From - To;
    DWORD Ms;
    LUH_ANIM Anim;
    double T;

    if (Distance == 0)
        return;
    Ms = (DWORD)(((LONGLONG)LUH_LIFT_MS * Distance) / max(g_Ui.Face.Height, 1));
    /* An ease-out cubic starts at 3 * distance / duration: match the speed of the release. */
    if (Velocity > 0.0 && (DWORD)(3.0 * Distance / Velocity) < Ms)
        Ms = (DWORD)(3.0 * Distance / Velocity);
    LuhAnimStart(&Anim, Name, max(Ms, LUH_LIFT_MIN_MS));
    for (;;)
    {
        T = LuhAnimFrame(&Anim);
        LuhPresentLift(Window, LuhLerp(From, To, LuhEaseOut(T)));
        if (T >= 1.0)
            break;
        LuhAnimWait(&Anim);
    }
    LuhAnimEnd(&Anim);
}

static BOOL
LuhUpdateClock(void)
{
    WCHAR Time[64], Date[128];

    if (!GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, Time, ARRAYSIZE(Time)))
        Time[0] = UNICODE_NULL;
    if (!GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, Date, ARRAYSIZE(Date)))
        Date[0] = UNICODE_NULL;
    if (!wcscmp(Time, g_Ui.ClockText) && !wcscmp(Date, g_Ui.DateText))
        return FALSE;
    StringCchCopyW(g_Ui.ClockText, ARRAYSIZE(g_Ui.ClockText), Time);
    StringCchCopyW(g_Ui.DateText, ARRAYSIZE(g_Ui.DateText), Date);
    return TRUE;
}

static void
LuhFreeCurtainPage(void)
{
    if (g_Ui.CurtainPage)
    {
        SecureZeroMemory(g_Ui.CurtainPage, sizeof(*g_Ui.CurtainPage));
        HeapFree(GetProcessHeap(), 0, g_Ui.CurtainPage);
        g_Ui.CurtainPage = NULL;
    }
}

/*
 * The clock page Windows shows before the tiles. Only when CAD is not required: with CAD,
 * winlogon asks for credentials only after the user's SAS, which is the curtain lifting.
 */
static void
LuhShowCurtain(
    _In_ HWND Window)
{
    g_Ui.Curtain = TRUE;
    g_Ui.Lift = 0;
    LuhUpdateClock();
    LuhBuildFace();
    LuhArmClock(Window);
    SetFocus(Window);
    LuhTrace("curtain", NULL);
    InvalidateRect(Window, NULL, TRUE);
}

static void
LuhDropCurtain(
    _In_ HWND Window)
{
    if (!g_Ui.Curtain)
        return;
    if (g_Ui.Dragging)
    {
        g_Ui.Dragging = FALSE;
        ReleaseCapture();
    }
    g_Ui.Curtain = FALSE;
    g_Ui.CadCurtain = FALSE;
    g_Ui.Lift = 0;
    KillTimer(Window, LUH_TIMER_CLOCK);
    LuhFreeCurtainPage();
}

/* Once the curtain is gone: the page that waited under it comes up. */
static void
LuhLiftCurtain(
    _In_ HWND Window)
{
    PLUH_PAGE Page = g_Ui.CurtainPage;

    if (!g_Ui.Curtain || g_Ui.CadCurtain)
        return;
    g_Ui.CurtainPage = NULL;
    LuhDropCurtain(Window);
    LuhTrace("curtain-lifted", NULL);
    if (Page)
    {
        LuhSetSpinner(Window, FALSE);
        LuhCreatePage(Window, Page);
        if (g_Ui.Page && g_Ui.Page->AutoSubmit)
        {
            LuhTrace("auto-submit", NULL);
            LuhSubmit(Window);
        }
    }
    InvalidateRect(Window, NULL, TRUE);
}

/* The curtain follows the pointer while the button is down; each move is one frame. */
static void
LuhDragBegin(
    _In_ HWND Window,
    _In_ LONG Y)
{
    g_Ui.Dragging = TRUE;
    g_Ui.DragStartY = Y;
    g_Ui.DragLastY = Y;
    g_Ui.DragLastTime = GetMessageTime();
    g_Ui.DragVelocity = 0.0;
    SetCapture(Window);
    LuhAnimStart(&g_Ui.DragAnim, "curtain-drag", 0);
}

static void
LuhDragMove(
    _In_ HWND Window,
    _In_ LONG Y)
{
    DWORD Now = GetMessageTime(), Elapsed = Now - g_Ui.DragLastTime;

    if (Elapsed > 0)
    {
        /* Smoothed, so one jittery event does not decide a flick. */
        g_Ui.DragVelocity = 0.6 * ((double)(g_Ui.DragLastY - Y) / Elapsed) + 0.4 * g_Ui.DragVelocity;
        g_Ui.DragLastY = Y;
        g_Ui.DragLastTime = Now;
    }
    LuhAnimFrame(&g_Ui.DragAnim);
    LuhPresentLift(Window, g_Ui.DragStartY - Y);
}

/* Released: a third of the way, a flick upwards or a plain click lifts it; otherwise it drops back. */
static void
LuhDragEnd(
    _In_ HWND Window)
{
    double Velocity = g_Ui.DragVelocity;

    g_Ui.Dragging = FALSE;
    ReleaseCapture();
    LuhAnimEnd(&g_Ui.DragAnim);
    if (GetMessageTime() - g_Ui.DragLastTime > 100)
        Velocity = 0.0;

    if (g_Ui.Lift > g_Ui.Face.Height / 3 || Velocity > LUH_LIFT_VELOCITY || g_Ui.Lift < LuhPx(4))
    {
        LuhSlideCurtain(Window, g_Ui.Face.Height, Velocity, "curtain-lift");
        LuhLiftCurtain(Window);
    }
    else
    {
        LuhSlideCurtain(Window, 0, 0.0, "curtain-return");
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
            if (Command->Flags & WLUI_FLAG_CURTAIN)
            {
                LuhDestroyPage();
                LuhSetSpinner(Window, FALSE);
                if (!g_Ui.Curtain)
                    LuhShowCurtain(Window);
                g_Ui.CadCurtain = TRUE;
                LuhBuildFace();
                LuhTrace("cad-curtain", g_Ui.Title);
                InvalidateRect(Window, NULL, TRUE);
                break;
            }
            if (g_Ui.CadCurtain)
                LuhDropCurtain(Window);
            LuhSetSpinner(Window, (Command->Flags & WLUI_FLAG_SPINNER) != 0);
            LuhTrace("status", g_Ui.Title);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdDisplayWelcome:
            LuhDropCurtain(Window);
            LuhDestroyPage();
            StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), LuhText(IDS_LUH_WELCOME));
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
            else if (Command->Text[0] == UNICODE_NULL && LuhAccountStatusText(Command->Status, Command->SubStatus))
                StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail),
                               LuhText(LuhAccountStatusText(Command->Status, Command->SubStatus)));
            else
                StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Command->Text);
            LuhSetSpinner(Window, FALSE);
            LuhClearSecrets();
            LuhEnablePage(TRUE);
            LuhFocusForRetry();
            LuhUpdateCapsLock(Window);  /* the focus moved without key input */
            LuhTrace("result", g_Ui.Detail);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdRequestCredentials:
            if (g_Ui.CadCurtain)
                LuhDropCurtain(Window);     /* the user's SAS came first: the notice gives way to the page */
            StringCchCopyW(g_Options.PrefillUser, ARRAYSIZE(g_Options.PrefillUser), Command->User);
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
            /* A change the signed-in user asked for comes without WLUI_FLAG_FORCED_CHANGE. */
            g_Ui.ChangeCancel = g_Options.Scenario == CPUS_CHANGE_PASSWORD && !(Command->Flags & WLUI_FLAG_FORCED_CHANGE);
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
            if (!g_Ui.ProvidersStarted && (Command->Flags & WLUI_FLAG_DISABLE_CAD) &&
                !(Command->Flags & WLUI_FLAG_FORCED_CHANGE) &&
                (g_Options.Scenario == CPUS_LOGON || g_Options.Scenario == CPUS_UNLOCK_WORKSTATION))
            {
                LuhSetSpinner(Window, FALSE);
                LuhShowCurtain(Window);
            }
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
                LuhDropCurtain(Window);
                LuhDestroyPage();
                LuhSetSpinner(Window, FALSE);
                SetTimer(Window, LUH_TIMER_FADE, LUH_FADE_MS / LUH_FADE_STEPS, NULL);
            }
            break;

        case WluiCmdDismiss:
            LuhBeginExit(Window, WluiExitDismissed, LUH_EXIT_OK);
            break;

        case WluiCmdShowOptions:
            LuhShowOptionsPage(Window, Command->Flags, Command->User);
            break;

        default:
            WARN("unknown command %u ignored\n", Command->Type);
            break;
    }
}

/* ---- painting --------------------------------------------------------------------- */

/* The page's still picture: the soft backdrop and its text, drawn only when the text changes. */
static void
LuhComposeStill(
    _In_ const RECT *Client,
    _In_ int TextTop,
    _In_ const RECT *Caps)
{
    PLUH_SURFACE Back = &g_Ui.Still;
    const COLORREF White = RGB(255, 255, 255);
    RECT Rect, Wrap;
    HGDIOBJ OldFont;
    int Width, Extra;

    BitBlt(Back->Dc, 0, 0, Back->Width, Back->Height, g_Ui.Soft.Dc, 0, 0, SRCCOPY);

    /* A long account message wraps. Measure it first: it keeps its bottom edge (so the page
     * below never moves) and grows upwards, and the title moves up by the extra lines. */
    Width = min(Client->right - 2 * LUH_DETAIL_MARGIN, LUH_DETAIL_WIDTH);
    Width = max(Width, 1);
    Extra = 0;
    if (g_Ui.Detail[0] != UNICODE_NULL)
    {
        OldFont = SelectObject(Back->Dc, g_Ui.TextFont);
        SetRect(&Wrap, 0, 0, Width, 0);
        DrawTextW(Back->Dc, g_Ui.Detail, -1, &Wrap, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        SelectObject(Back->Dc, OldFont);
        if (Wrap.bottom - Wrap.top > 28)
            Extra = (Wrap.bottom - Wrap.top) - 28;
    }

    Rect = *Client;
    Rect.top = TextTop - Extra;
    Rect.bottom = Rect.top + 40;
    LuhDrawText(Back, g_Ui.TitleFont, g_Ui.Title, &Rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);

    if (g_Ui.Detail[0] != UNICODE_NULL)
    {
        Rect.top = Rect.bottom + 8;
        Rect.bottom = TextTop + 40 + 8 + 28;
        if (Extra == 0)
        {
            LuhDrawText(Back, g_Ui.TextFont, g_Ui.Detail, &Rect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);
        }
        else
        {
            Rect.left = (Client->right - Width) / 2;
            Rect.right = Rect.left + Width;
            LuhDrawText(Back, g_Ui.TextFont, g_Ui.Detail, &Rect, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS, White, 255);
            Rect.left = Client->left;
            Rect.right = Client->right;
        }
    }

    g_Ui.SpinnerY = Rect.bottom + 48;

    if (Caps->right > Caps->left)
    {
        /* Under the box, as on Windows, in the gap the page keeps under its last password box
         * (on a page with several, that is where the line shows whichever box has the focus). */
        LuhDrawText(Back, g_Ui.TextFont, LuhText(IDS_LUH_CAPS_LOCK), Caps,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);
    }
}

/*
 * A frame is the cached still picture plus what moves (the spinner), composed in Back and
 * blitted once; child controls paint themselves on top (WS_CLIPCHILDREN).
 */
static void
LuhCompose(
    _In_ const RECT *Client)
{
    WCHAR Key[ARRAYSIZE(g_Ui.StillKey)];
    RECT Caps = { 0, 0, 0, 0 };
    int TextTop, i;

    /* With a credential page up, the text sits above its first row (the page grows with its
     * label rows, a fixed offset put the result under the title); otherwise it is centred. */
    TextTop = (g_Ui.Page || g_Ui.OptionsUp) ? g_Ui.PageTop - LUH_TEXT_ABOVE_PAGE : Client->bottom / 2 - 40;
    if (g_Ui.CapsControl != NULL && g_Ui.CapsTop > 0 && GetWindowRect(g_Ui.CapsControl, &Caps))
    {
        MapWindowPoints(NULL, GetParent(g_Ui.CapsControl), (LPPOINT)&Caps, 2);
        Caps.top = g_Ui.CapsTop;
        Caps.bottom = Caps.top + LUH_CAPS_GAP;
    }

    StringCchPrintfW(Key, ARRAYSIZE(Key), L"%d|%ld|%ld|%ld|%ld|%ls|%ls", TextTop, Caps.left, Caps.top,
                     Caps.right, Caps.bottom, g_Ui.Title, g_Ui.Detail);
    if (wcscmp(Key, g_Ui.StillKey) != 0)
    {
        LuhComposeStill(Client, TextTop, &Caps);
        StringCchCopyW(g_Ui.StillKey, ARRAYSIZE(g_Ui.StillKey), Key);
    }

    BitBlt(g_Ui.Back.Dc, 0, 0, g_Ui.Back.Width, g_Ui.Back.Height, g_Ui.Still.Dc, 0, 0, SRCCOPY);
    if (g_Ui.Spinner)
    {
        int Cx = Client->right / 2, Cy = g_Ui.SpinnerY;
        for (i = 0; i < 8; i++)
        {
            double A = (g_Ui.SpinnerAngle + i * 45) * 3.14159265 / 180.0;
            LuhFillCircle(&g_Ui.Back, Cx + (LONG)(18.0 * cos(A)), Cy + (LONG)(18.0 * sin(A)), 3,
                          RGB(255, 255, 255), (BYTE)(60 + i * 24));
        }
    }
}

static void
LuhPaint(
    _In_ HWND Window)
{
    PAINTSTRUCT Ps;
    HDC Dc;
    RECT Client;

    Dc = BeginPaint(Window, &Ps);
    GetClientRect(Window, &Client);

    if (g_Ui.Curtain)
    {
        /* The curtain is one cached picture; mid-drag a repaint shows the same position. */
        LuhBlitLift(Dc, g_Ui.Lift);
    }
    else
    {
        LuhCompose(&Client);
        BitBlt(Dc, Ps.rcPaint.left, Ps.rcPaint.top, Ps.rcPaint.right - Ps.rcPaint.left,
               Ps.rcPaint.bottom - Ps.rcPaint.top, g_Ui.Back.Dc, Ps.rcPaint.left, Ps.rcPaint.top, SRCCOPY);
    }
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
        {
            /* Labels sit on the soft backdrop: the same picture as a brush, aligned to the control. */
            POINT Origin = { 0, 0 };

            MapWindowPoints((HWND)lParam, Window, &Origin, 1);
            SetTextColor((HDC)wParam, RGB(255, 255, 255));
            SetBkMode((HDC)wParam, TRANSPARENT);
            SetBrushOrgEx((HDC)wParam, -Origin.x, -Origin.y, NULL);
            return (LRESULT)g_Ui.SoftBrush;
        }

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
            else if (wParam == LUH_TIMER_CLOCK)
            {
                if (g_Ui.Curtain)
                {
                    if (LuhUpdateClock())
                    {
                        LuhBuildFace();
                        if (!g_Ui.Dragging)
                            InvalidateRect(Window, NULL, FALSE);
                    }
                    LuhArmClock(Window);
                }
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
                else if (Id == LUH_ID_POWER)
                {
                    LuhPowerMenu(Window);
                }
                else if (Id >= LUH_ID_OPTION_BASE && Id < LUH_ID_OPTION_BASE + ARRAYSIZE(LuhOptionRows))
                {
                    LuhPickOption(LuhOptionRows[Id - LUH_ID_OPTION_BASE].Action);
                }
                else if (Id == LUH_ID_OPTION_CANCEL)
                {
                    LuhPickOption(WluiActionCancel);
                }
                else if (Id >= LUH_ID_TILE_BASE && Id < LUH_ID_TILE_BASE + LUH_MAX_TILES)
                {
                    LuhSelectTile(Window, Id - LUH_ID_TILE_BASE);
                }
                else if (Id >= LUH_ID_FIELD_BASE && Id < LUH_ID_RESTART && g_Ui.Page &&
                         Id - LUH_ID_FIELD_BASE < g_Ui.Page->Count)
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
            if (wParam == LUH_PAGE_READY && lParam && g_Ui.Curtain)
            {
                LuhFreeCurtainPage();
                g_Ui.CurtainPage = (PLUH_PAGE)lParam;
            }
            else if (wParam == LUH_PAGE_READY && lParam)
            {
                LuhSetSpinner(Window, FALSE);
                LuhCreatePage(Window, (PLUH_PAGE)lParam);
                if (g_Ui.Page && g_Ui.Page->AutoSubmit)
                {
                    LuhTrace("auto-submit", NULL);
                    LuhSubmit(Window);
                }
            }
            else if (wParam == LUH_PAGE_POLICY)
            {
                LuhDropCurtain(Window);
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

        case WM_KEYDOWN:
        case WM_RBUTTONDOWN:
            if (g_Ui.Curtain)
            {
                /* The Ctrl+Alt+Del notice has no lift path; only winlogon moves it on after the SAS. */
                if (!g_Ui.CadCurtain && !g_Ui.Dragging)
                {
                    LuhSlideCurtain(Window, g_Ui.Face.Height, 0.0, "curtain-lift");
                    LuhLiftCurtain(Window);
                }
                return 0;
            }
            break;

        case WM_LBUTTONDOWN:
            if (g_Ui.Curtain)
            {
                if (!g_Ui.CadCurtain)
                    LuhDragBegin(Window, (SHORT)HIWORD(lParam));
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (g_Ui.Dragging)
            {
                LuhDragMove(Window, (SHORT)HIWORD(lParam));
                return 0;
            }
            break;

        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_Ui.Dragging)
            {
                LuhDragEnd(Window);
                return 0;
            }
            break;

        case WM_CLOSE:
            /* Only winlogon closes this window, through Dismiss. */
            return 0;

        case WM_DESTROY:
            LuhDestroyPage();
            KillTimer(Window, LUH_TIMER_HEARTBEAT);
            KillTimer(Window, LUH_TIMER_SPINNER);
            KillTimer(Window, LUH_TIMER_FADE);
            KillTimer(Window, LUH_TIMER_STARTUP);
            KillTimer(Window, LUH_TIMER_CLOCK);
            LuhFreeCurtainPage();
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
    LuhGfxInit();
    g_Ui.TitleFont = LuhFontCreate(32, FW_LIGHT);
    g_Ui.TextFont = LuhFontCreate(20, FW_NORMAL);
    /* The lock screen as Windows 11 draws it: a large semibold clock, the date under it. */
    g_Ui.ClockFont = LuhFontCreate(100, FW_SEMIBOLD);
    g_Ui.DateFont = LuhFontCreate(20, FW_SEMIBOLD);
    if (!LuhBuildBackdrop(Width, Height))
        return NULL;

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
                             WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, Width, Height, NULL, NULL, Instance, NULL);
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
        if (Msg.message == WM_KEYDOWN && Msg.wParam == VK_ESCAPE && (g_Ui.OptionsUp || (g_Ui.ChangeCancel && g_Ui.Page)))
        {
            LuhPickOption(WluiActionCancel);
            continue;
        }
        if (!((g_Ui.Page || g_Ui.OptionsUp) && IsDialogMessageW(Window, &Msg)))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        /* Caps Lock toggles and focus moves come in as key and mouse input. */
        if ((Msg.message >= WM_KEYFIRST && Msg.message <= WM_KEYLAST) ||
            Msg.message == WM_LBUTTONDOWN || Msg.message == WM_LUH_PAGE)
            LuhUpdateCapsLock(Window);
    }

    if (g_Ui.ProvidersStarted)
        LuhProvidersStop();
    if (g_Ui.TitleFont) DeleteObject(g_Ui.TitleFont);
    if (g_Ui.TextFont) DeleteObject(g_Ui.TextFont);
    if (g_Ui.ClockFont) DeleteObject(g_Ui.ClockFont);
    if (g_Ui.DateFont) DeleteObject(g_Ui.DateFont);
    if (g_Ui.SoftBrush) DeleteObject(g_Ui.SoftBrush);
    LuhSurfaceDestroy(&g_Ui.Back);
    LuhSurfaceDestroy(&g_Ui.Sharp);
    LuhSurfaceDestroy(&g_Ui.Soft);
    LuhSurfaceDestroy(&g_Ui.Face);
    LuhSurfaceDestroy(&g_Ui.Still);
    LuhGfxShutdown();
    return (int)Msg.wParam;
}
