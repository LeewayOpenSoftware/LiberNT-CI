/*
 * PROJECT:     LiberNT LogonUI host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     The host window: status, Welcome, spinner, the credential page, fade-out
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include "logonuihost.h"
#include "resource.h"
#include "gfx.h"
#include <aclapi.h>
#include <powrprof.h>
#include <math.h>

WINE_DEFAULT_DEBUG_CHANNEL(logonuihost);

#define LUH_CLASS           L"LiberNT_LogonUIHost"
#define LUH_TIMER_HEARTBEAT 1
#define LUH_TIMER_SPINNER   2
#define LUH_TIMER_FADE      3
#define LUH_TIMER_STARTUP   4   /* a page must arrive within the provider call bound */
#define LUH_TIMER_CLOCK     5   /* the curtain's clock, only while the curtain is up */
#define LUH_SPINNER_MS      16      /* a frame at 60 Hz; the phase comes from the clock */
#define LUH_SPINNER_PERIOD  2000    /* ms per cycle of the chasing dots */
#define LUH_SPINNER_DOTS    6
#define LUH_SPINNER_RING    28      /* ring radius at 96 DPI */
#define LUH_STATUS_DOTS_DY  (-34)   /* black status page: dots and text, from the centre (Win11 shots) */
#define LUH_STATUS_TEXT_DY  53
#define LUH_RESULT_TEXT_DY  35      /* result in place of the fields: from the first box top */
#define LUH_RESULT_OK_DY    82
#define LUH_WELCOME_TEXT_DY 52      /* in-place Welcome: text under the dots */

#define LUH_ID_FIELD_BASE   1000
#define LUH_ID_RESTART      1900
#define LUH_ID_SHUTDOWN     1901
#define LUH_ID_POWER        1902    /* the power button: a flyout of LUH_ID_SLEEP, LUH_ID_SHUTDOWN, LUH_ID_RESTART */
#define LUH_ID_SLEEP        1903
#define LUH_ID_OPTION_BASE  1920    /* + row of the Ctrl+Alt+Del options (LuhOptionRows) */
#define LUH_ID_OPTION_CANCEL 1930
#define LUH_ID_RESULT_OK    1931    /* the OK under a result shown in place of the fields */
#define LUH_ID_NOTICE_OK    1932    /* a full-screen notice */
#define LUH_ID_NOTICE_CANCEL 1933
#define LUH_POWER_WIDTH     120
#define LUH_ID_TILE_BASE    2000    /* + tile index, below LUH_ID_TILE_BASE + LUH_MAX_TILES */

#define LUH_TILE_WIDTH      260
#define LUH_TILE_MARGIN     24

#define LUH_PAGE_WIDTH      360
#define LUH_ROW_HEIGHT      30
#define LUH_ROW_GAP         10
#define LUH_DETAIL_WIDTH    560     /* a wrapped result line, e.g. the account restriction text */
/* The sign-in page, measured on the Windows 11 shots (1024x768, 96 DPI); y offsets from the centre. */
#define LUH_AVATAR_RADIUS   96
#define LUH_AVATAR_DY       (-134)
#define LUH_NAME_DY         (-3)
#define LUH_BOX_DY          44
#define LUH_BOX_WIDTH       296
#define LUH_BOX_HEIGHT      34
#define LUH_BOX_PITCH       46
#define LUH_BOX_BUTTON      34      /* the arrow and the eye: square hit areas at the box's right end */
#define LUH_PILL_WIDTH      125
#define LUH_PILL_HEIGHT     36
#define LUH_TILE_ROW        56
#define LUH_TILE_AVATAR     24      /* radius of the small account picture */
#define LUH_TILE_BOX_WIDTH  220
#define LUH_POWER_SIZE      40
#define LUH_ACCENT          RGB(76, 160, 255)
/* Ctrl+Alt+Del options (Win11 shot 09): a left-aligned list on black, 44 px pitch, a dark Cancel. */
#define LUH_OPTION_DX       (-72)   /* the list's left edge from the centre */
#define LUH_OPTION_TOP_DY   (-177)  /* the first row's top from the centre */
#define LUH_OPTION_ROW      44
#define LUH_OPTION_WIDTH    138
#define LUH_OPTION_CANCEL_DY 83
#define LUH_OPTION_CANCEL_W 120
#define LUH_OPTION_CANCEL_H 32
/* Change a password (shot 10): picture higher, a title, the account in a box, 52 px pitch. */
#define LUH_CHANGE_AVATAR_DY (-219)
#define LUH_CHANGE_TITLE_DY (-68)
#define LUH_CHANGE_FIELDS_DY (-22)
#define LUH_CHANGE_PITCH    52
#define LUH_CHANGE_CANCEL_DY 280
/* Power flyout (shot 20) and the sign-in page while Shutting down / Restarting (shots 19, 21). */
#define LUH_FLYOUT_WIDTH    132
#define LUH_FLYOUT_ROW      40
#define LUH_FLYOUT_FILL     RGB(43, 43, 48)
#define LUH_POWER_DOTS_DY   68
#define LUH_POWER_TEXT_DY   130
/* Full-screen notice (legal notice shot): caption above the centre, text from it, OK under it. */
#define LUH_NOTICE_CAPTION_DY (-27)
#define LUH_NOTICE_TEXT_WIDTH 720
#define LUH_NOTICE_BUTTON_W 118
#define LUH_NOTICE_BUTTON_H 30
/* Lock from Ctrl+Alt+Del (lock-cad frames): one white dot on black, then the curtain fades in. */
#define LUH_FADE_HOLD_MS    250
#define LUH_CAPS_GAP        28      /* under the last password box: the Caps Lock line */

typedef struct _LUH_UI
{
    HFONT TitleFont;        /* the change-password title */
    HFONT TextFont;
    WCHAR Title[256];       /* "Welcome" user or status text */
    WCHAR Detail[256];      /* result text, when any */
    BOOL Spinner;
    LUH_ANIM SpinnerAnim;   /* its clock and frame line while it turns */
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
    int PageTop;            /* y of the first field row; a result replaces the rows from here */
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
    BOOL StillValid;        /* Still matches the page; a full-window paint always rebuilds it */
    HWND Window;
    HFONT NameFont;         /* the account name under the picture */
    HFONT StrongFont;       /* the Caps Lock line */
    COLORREF BoxFill;       /* the field boxes: one solid colour, so the edits' own background matches */
    HBRUSH BoxBrush;
    WNDPROC EditProc;       /* the system edit procedure, under the cue-text subclass */
    LONG AvatarY;           /* centre of the account picture */
    LONG DetailTop;         /* the result line, under the fields */
    DWORD NameField;        /* the large text drawn as the name, or -1 */
    DWORD ArrowField;       /* the box that holds the submit arrow (and the eye), or -1 */
    RECT Arrow;
    RECT Eye;
    BOOL HasText;           /* the arrow box is not empty: the eye shows */
    BOOL Revealing;         /* the eye is held down */
    BOOL CurtainFadeIn;     /* the lock curtain still has to come up from black */
    BOOL FadeInPosted;
    BOOL SignInBusy;        /* submitted: dots and Welcome stand where the fields were */
    BOOL ResultUp;          /* a refused sign-in: its message and OK stand where the fields were */
    HWND OkButton;
    HFONT StatusFont;       /* Welcome, Signing out on the black status page */
    BOOL StatusOnPage;      /* Shutting down / Restarting over the sign-in picture */
    HFONT SmallFont;        /* the flyout tooltip */
    BOOL FlyoutUp;
    HWND FlyoutButtons[3];
    RECT FlyoutCard;
    BOOL NoticeUp;          /* a full-screen notice is up (its own modal loop) */
    BOOL NoticeOk;
    PCWSTR NoticeCaption;
    PCWSTR NoticeText;
    RECT NoticeTextRect;
    WCHAR PasswordChar;     /* the revealed edit's own mask character, put back after the reveal */
    BOOL NoReveal;          /* policy: "Do not display the password reveal button" */
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

static void LuhCloseFlyout(_In_ HWND Window, _In_ BOOL FocusPower);
static BOOL LuhIsFlyoutRow(_In_opt_ HWND Control);

/* Whatever the page shows changed: the still picture is rebuilt on the next paint. */
static void
LuhPageChanged(
    _In_ HWND Window)
{
    g_Ui.StillValid = FALSE;
    InvalidateRect(Window, NULL, FALSE);
}

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
    {
        LuhAnimStart(&g_Ui.SpinnerAnim, "spinner", 0);
        SetTimer(Window, LUH_TIMER_SPINNER, LUH_SPINNER_MS, NULL);
    }
    else if (!On && g_Ui.Spinner)
    {
        KillTimer(Window, LUH_TIMER_SPINNER);
        LuhAnimEnd(&g_Ui.SpinnerAnim);
    }
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
    if (g_Ui.Revealing && GetCapture())
        ReleaseCapture();
    g_Ui.Revealing = FALSE;
    g_Ui.HasText = FALSE;
    g_Ui.SignInBusy = FALSE;
    g_Ui.ResultUp = FALSE;
    g_Ui.StatusOnPage = FALSE;
    if (g_Ui.FlyoutUp && g_Ui.Window)
        LuhCloseFlyout(g_Ui.Window, FALSE);
    if (g_Ui.OkButton)
    {
        DestroyWindow(g_Ui.OkButton);
        g_Ui.OkButton = NULL;
    }
    g_Ui.ArrowField = (DWORD)-1;
    g_Ui.NameField = (DWORD)-1;
    if (g_Ui.BoxBrush)
    {
        DeleteObject(g_Ui.BoxBrush);
        g_Ui.BoxBrush = NULL;
    }
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
 * The power button of the sign-in screen, bottom right. Hiding it is only the visible half:
 * winlogon's action handler refuses Restart and Shut down itself while ShutdownWithoutLogon is 0.
 */
static void
LuhCreatePowerButton(
    _In_ HWND Window)
{
    RECT Client;
    LONG Size = LuhPx(LUH_POWER_SIZE);

    if (g_Options.NoPower)
        return;
    GetClientRect(Window, &Client);
    g_Ui.PowerButton = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_POWER),
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                       Client.right - LuhPx(44) - Size / 2, Client.bottom - LuhPx(42) - Size / 2,
                                       Size, Size, Window, (HMENU)LUH_ID_POWER, NULL, NULL);
}

static void LuhDropCurtain(_In_ HWND Window);

/* The Ctrl+Alt+Del options, in the Windows order; a row policy turned off is not shown. Switch user
 * is not offered: winlogon has no action for it yet, and a row that does nothing is not shown. */
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
    LONG X, Y, Width = LuhPx(LUH_OPTION_WIDTH);
    DWORD i, Shown = 0;
    SIZE Text;

    UNREFERENCED_PARAMETER(User);   /* Windows 11 does not name the user on this screen */
    LuhDropCurtain(Window);
    LuhDestroyPage();
    LuhSetSpinner(Window, FALSE);
    GetClientRect(Window, &Client);
    g_Ui.Title[0] = UNICODE_NULL;
    g_Ui.Detail[0] = UNICODE_NULL;
    for (i = 0; i < ARRAYSIZE(LuhOptionRows); i++)
    {
        Text = LuhMeasureText(g_Ui.TextFont, LuhText(LuhOptionRows[i].Text));
        Width = max(Width, Text.cx + LuhPx(18));
    }
    X = Client.right / 2 + LuhPx(LUH_OPTION_DX);
    Y = Client.bottom / 2 + LuhPx(LUH_OPTION_TOP_DY);
    g_Ui.PageTop = Y;
    for (i = 0; i < ARRAYSIZE(LuhOptionRows); i++)
    {
        if (Flags & LuhOptionRows[i].Hide)
            continue;
        g_Ui.OptionButtons[Shown++] = CreateWindowExW(0, L"BUTTON", LuhText(LuhOptionRows[i].Text),
                                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                                      X, Y, Width, LuhPx(LUH_OPTION_ROW), Window,
                                                      (HMENU)(UINT_PTR)(LUH_ID_OPTION_BASE + i), NULL, NULL);
        Y += LuhPx(LUH_OPTION_ROW);
    }
    g_Ui.OptionButtons[Shown] = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_CANCEL),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                                X + LuhPx(9), Client.bottom / 2 + LuhPx(LUH_OPTION_CANCEL_DY),
                                                LuhPx(LUH_OPTION_CANCEL_W), LuhPx(LUH_OPTION_CANCEL_H), Window,
                                                (HMENU)LUH_ID_OPTION_CANCEL, NULL, NULL);
    g_Options.NoPower = (Flags & WLUI_FLAG_NO_POWER) != 0;
    LuhCreatePowerButton(Window);
    g_Ui.OptionsUp = TRUE;
    SetFocus(g_Ui.OptionButtons[0] ? g_Ui.OptionButtons[0] : Window);
    LuhTrace("options", NULL);
    LuhPageChanged(Window);
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

/* The accounts, bottom left, as on the Windows 11 sign-in screen: the last row ends near the bottom. */
static void
LuhCreateTileList(
    _In_ HWND Window,
    _In_ PLUH_PAGE Page)
{
    RECT Client;
    DWORD i, Count = min(Page->TileCount, (DWORD)LUH_MAX_TILES);
    LONG Row = LuhPx(LUH_TILE_ROW), Y;

    if (Count < 2)
        return;
    GetClientRect(Window, &Client);
    Y = Client.bottom - LuhPx(27) - (LONG)Count * Row;
    for (i = 0; i < Count; i++)
    {
        g_Ui.TileButtons[i] = CreateWindowExW(0, L"BUTTON", Page->TileNames[i],
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                              LuhPx(24), Y + LuhPx(2), LuhPx(LUH_TILE_BOX_WIDTH), Row - LuhPx(4),
                                              Window, (HMENU)(UINT_PTR)(LUH_ID_TILE_BASE + i), NULL, NULL);
        Y += Row;
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

/* ---- sign-in page drawing ------------------------------------------------------------ */

/* An elliptic arc from angle From to To (degrees, clockwise from 3 o'clock), as short strokes. */
static void
LuhArc(
    _Inout_ PLUH_SURFACE Surface,
    _In_ double Cx,
    _In_ double Cy,
    _In_ double Rx,
    _In_ double Ry,
    _In_ double From,
    _In_ double To,
    _In_ double Width,
    _In_ COLORREF Color)
{
    const double Step = 10.0, Rad = 3.14159265358979 / 180.0;
    double A, B;

    for (A = From; A < To; A = B)
    {
        B = min(A + Step, To);
        LuhStroke(Surface, Cx + Rx * cos(A * Rad), Cy + Ry * sin(A * Rad),
                  Cx + Rx * cos(B * Rad), Cy + Ry * sin(B * Rad), Width, Color, 255);
    }
}

/* The default account picture: a head and shoulders, sized to a circle of diameter Size. */
static void
LuhDrawPerson(
    _Inout_ PLUH_SURFACE Surface,
    _In_ LONG Cx,
    _In_ LONG Cy,
    _In_ LONG Size,
    _In_ COLORREF Color)
{
    RECT Body;

    LuhFillCircle(Surface, Cx, Cy - Size * 12 / 100, Size * 13 / 100, Color, 255);
    SetRect(&Body, Cx - Size * 23 / 100, Cy + Size * 4 / 100, Cx + Size * 23 / 100, Cy + Size * 27 / 100);
    LuhFillRound(Surface, &Body, Size * 10 / 100, Color, 255);
}

static void
LuhDrawArrow(
    _Inout_ PLUH_SURFACE Surface,
    _In_ const RECT *Rect)
{
    double Cx = (Rect->left + Rect->right) / 2.0, Cy = (Rect->top + Rect->bottom) / 2.0;
    double L = LuhPx(7), H = LuhPx(5), W = max(LuhPx(12), 10) / 10.0;

    LuhStroke(Surface, Cx - L, Cy, Cx + L, Cy, W, RGB(255, 255, 255), 230);
    LuhStroke(Surface, Cx + L, Cy, Cx + L - H, Cy - H, W, RGB(255, 255, 255), 230);
    LuhStroke(Surface, Cx + L, Cy, Cx + L - H, Cy + H, W, RGB(255, 255, 255), 230);
}

static void
LuhDrawEye(
    _Inout_ PLUH_SURFACE Surface,
    _In_ const RECT *Rect)
{
    double Cx = (Rect->left + Rect->right) / 2.0, Cy = (Rect->top + Rect->bottom) / 2.0;
    double W = max(LuhPx(12), 10) / 10.0;

    LuhArc(Surface, Cx, Cy, LuhPx(8), LuhPx(5), 0.0, 360.0, W, RGB(255, 255, 255));
    LuhFillCircle(Surface, (LONG)Cx, (LONG)Cy, max(LuhPx(2), 2), RGB(255, 255, 255), 230);
}

/* Sleep: a crescent, a disc with an offset disc in the flyout's colour taken out of it. */
static void
LuhDrawMoon(
    _Inout_ PLUH_SURFACE Surface,
    _In_ LONG Cx,
    _In_ LONG Cy)
{
    LONG R = LuhPx(7);

    LuhFillCircle(Surface, Cx, Cy, R, RGB(255, 255, 255), 255);
    LuhFillCircle(Surface, Cx + R / 2 + 1, Cy - R / 2, R - 1, LUH_FLYOUT_FILL, 255);
}

/* Restart: a ring open at the top right with an arrow head at its end. */
static void
LuhDrawRestart(
    _Inout_ PLUH_SURFACE Surface,
    _In_ LONG Cx,
    _In_ LONG Cy)
{
    const double Rad = 3.14159265358979 / 180.0;
    double R = LuhPx(7), W = max(LuhPx(12), 10) / 10.0, Ex = Cx + R * cos(-60.0 * Rad), Ey = Cy + R * sin(-60.0 * Rad);

    LuhArc(Surface, Cx, Cy, R, R, -30.0, 270.0, W, RGB(255, 255, 255));
    LuhStroke(Surface, Ex - LuhPx(4), Ey - LuhPx(1), Ex + LuhPx(1), Ey + LuhPx(1), W, RGB(255, 255, 255), 255);
    LuhStroke(Surface, Ex + LuhPx(1), Ey + LuhPx(1), Ex + LuhPx(1), Ey - LuhPx(4), W, RGB(255, 255, 255), 255);
}

/* The power symbol: a ring open at the top and a bar through the gap. */
static void
LuhDrawPower(
    _Inout_ PLUH_SURFACE Surface,
    _In_ LONG Cx,
    _In_ LONG Cy)
{
    double R = LuhPx(8), W = max(LuhPx(15), 12) / 10.0;

    LuhArc(Surface, Cx, Cy, R, R, -60.0, 240.0, W, RGB(255, 255, 255));
    LuhStroke(Surface, Cx, Cy - R - 1, Cx, Cy - 1, W, RGB(255, 255, 255), 255);
}

static PLUH_FIELD
LuhFieldOfControl(
    _In_ HWND Control)
{
    DWORD i;

    if (g_Ui.Page == NULL || Control == NULL)
        return NULL;
    for (i = 0; i < g_Ui.Page->Count; i++)
        if (g_Ui.Page->Fields[i].Control == Control)
            return &g_Ui.Page->Fields[i];
    return NULL;
}

/*
 * The owner-drawn controls (user tiles, power, Sign in, links): drawn over the still page in
 * the scratch frame, then copied to the control. Drawn on events only, never per frame.
 */
static void
LuhDrawItem(
    _In_ const DRAWITEMSTRUCT *Item)
{
    PLUH_SURFACE Surface = &g_Ui.Back;
    const COLORREF White = RGB(255, 255, 255);
    BOOL Down = (Item->itemState & ODS_SELECTED) != 0;
    PLUH_FIELD Field = LuhFieldOfControl(Item->hwndItem);
    WCHAR Text[LUH_FIELD_TEXT];
    POINT Origin = { 0, 0 };
    LONG Width = Item->rcItem.right - Item->rcItem.left, Height = Item->rcItem.bottom - Item->rcItem.top;
    LONG Cx, Cy;
    RECT Rect, Local;

    if (!g_Ui.StillValid)
        return;
    MapWindowPoints(Item->hwndItem, GetParent(Item->hwndItem), &Origin, 1);
    SetRect(&Rect, Origin.x, Origin.y, Origin.x + Width, Origin.y + Height);
    BitBlt(Surface->Dc, Rect.left, Rect.top, Width, Height, g_Ui.Still.Dc, Rect.left, Rect.top, SRCCOPY);
    GetWindowTextW(Item->hwndItem, Text, ARRAYSIZE(Text));
    Cx = (Rect.left + Rect.right) / 2;
    Cy = (Rect.top + Rect.bottom) / 2;

    if (Item->CtlID >= LUH_ID_TILE_BASE && Item->CtlID < LUH_ID_TILE_BASE + LUH_MAX_TILES)
    {
        /* Bottom left: a small picture and the name; the selected account sits on a light plate. */
        if (Down || (g_Ui.Page && Item->CtlID - LUH_ID_TILE_BASE == g_Ui.Page->TileSelected))
            LuhFillRound(Surface, &Rect, LuhPx(4), White, Down ? 40 : 26);
        Cx = Rect.left + LuhPx(12 + LUH_TILE_AVATAR);
        LuhFillCircle(Surface, Cx, Cy, LuhPx(LUH_TILE_AVATAR), RGB(226, 226, 226), 255);
        LuhDrawPerson(Surface, Cx, Cy, LuhPx(2 * LUH_TILE_AVATAR), RGB(150, 150, 150));
        Local = Rect;
        Local.left = Cx + LuhPx(LUH_TILE_AVATAR + 12);
        Local.right -= LuhPx(8);
        LuhDrawText(Surface, g_Ui.TextFont, Text, &Local, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    White, 255);
    }
    else if (LuhIsFlyoutRow(Item->hwndItem))
    {
        /* A flyout row: its symbol, then the label. */
        Cx = Rect.left + LuhPx(18);
        if (Item->CtlID == LUH_ID_RESTART)
            LuhDrawRestart(Surface, Cx, Cy);
        else if (Item->CtlID == LUH_ID_SLEEP)
            LuhDrawMoon(Surface, Cx, Cy);
        else
            LuhDrawPower(Surface, Cx, Cy);
        Local = Rect;
        Local.left += LuhPx(38);
        LuhDrawText(Surface, g_Ui.TextFont, Text, &Local, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    White, Down ? 170 : 255);
    }
    else if (Item->CtlID == LUH_ID_NOTICE_OK || Item->CtlID == LUH_ID_NOTICE_CANCEL ||
             Item->CtlID == LUH_ID_RESTART || Item->CtlID == LUH_ID_SHUTDOWN)
    {
        /* Buttons on black: the dark grey pill of the notice and Ctrl+Alt+Del screens. */
        LuhFillRound(Surface, &Rect, LuhPx(4), Down ? RGB(40, 40, 40) : RGB(51, 51, 51), 255);
        LuhDrawText(Surface, g_Ui.TextFont, Text, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    White, 255);
    }
    else if (Item->CtlID == LUH_ID_POWER)
    {
        if (Down)
            LuhFillRound(Surface, &Rect, LuhPx(4), White, 40);
        LuhDrawPower(Surface, Cx, Cy);
    }
    else if (g_Ui.OptionsUp && Item->CtlID >= LUH_ID_OPTION_BASE &&
             Item->CtlID < LUH_ID_OPTION_BASE + ARRAYSIZE(LuhOptionRows))
    {
        /* A plain text row; the keyboard focus is a white frame around it. */
        Local = Rect;
        Local.left += LuhPx(9);
        LuhDrawText(Surface, g_Ui.TextFont, Text, &Local, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    RGB(225, 225, 225), Down ? 170 : 255);
    }
    else if (g_Ui.OptionsUp && Item->CtlID == LUH_ID_OPTION_CANCEL)
    {
        LuhFillRound(Surface, &Rect, LuhPx(4), Down ? RGB(40, 40, 40) : RGB(51, 51, 51), 255);
        LuhDrawText(Surface, g_Ui.TextFont, Text, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    White, 255);
    }
    else if (Field && Field->Type == CPFT_COMMAND_LINK)
    {
        LuhDrawText(Surface, g_Ui.TextFont, Text, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    White, Down ? 170 : 255);
    }
    else
    {
        /* Sign in, Cancel: a quiet translucent pill. */
        LuhFillRound(Surface, &Rect, LuhPx(4), White, Down ? 64 : 40);
        LuhFrameRound(Surface, &Rect, LuhPx(4), 1, White, 60);
        LuhDrawText(Surface, g_Ui.TextFont, Text, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    White, 255);
    }

    if ((Item->itemState & ODS_FOCUS) && !(Item->itemState & ODS_NOFOCUSRECT))
        LuhFrameRound(Surface, &Rect, LuhPx(4), LuhPx(2), White, 255);
    GdiFlush();
    BitBlt(Item->hDC, 0, 0, Width, Height, Surface->Dc, Rect.left, Rect.top, SRCCOPY);
}

/* Borderless edits inside the drawn boxes: the field's label shows as cue text while empty. */
static LRESULT CALLBACK
LuhEditProc(
    _In_ HWND Edit,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    LRESULT Result = CallWindowProcW(g_Ui.EditProc, Edit, Message, wParam, lParam);
    PLUH_FIELD Field;
    HDC Dc;
    HGDIOBJ Old;
    RECT Rect;

    if (Message == WM_PAINT && GetWindowTextLengthW(Edit) == 0 && (Field = LuhFieldOfControl(Edit)) != NULL &&
        Field->Label[0] != UNICODE_NULL)
    {
        Dc = GetDC(Edit);
        if (Dc)
        {
            GetClientRect(Edit, &Rect);
            Rect.left += LOWORD(SendMessageW(Edit, EM_GETMARGINS, 0, 0)) + 1;
            Old = SelectObject(Dc, (HFONT)SendMessageW(Edit, WM_GETFONT, 0, 0));
            SetBkMode(Dc, TRANSPARENT);
            SetTextColor(Dc, RGB(200, 200, 205));
            DrawTextW(Dc, Field->Label, -1, &Rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(Dc, Old);
            ReleaseDC(Edit, Dc);
        }
    }
    return Result;
}

/* A field box and what sits in it, over the soft backdrop. */
static void
LuhDrawBox(
    _Inout_ PLUH_SURFACE Surface,
    _In_ DWORD Index)
{
    PLUH_FIELD Field = &g_Ui.Page->Fields[Index];
    BOOL Focused = Field->Control != NULL && GetFocus() == Field->Control;
    RECT Line;

    LuhFillRound(Surface, &Field->Box, LuhPx(4), g_Ui.BoxFill, 255);
    LuhFrameRound(Surface, &Field->Box, LuhPx(4), 1, RGB(255, 255, 255), Focused ? 110 : 70);
    if (Focused)
    {
        /* Windows 11 marks the focused box with an accent underline. */
        SetRect(&Line, Field->Box.left + LuhPx(3), Field->Box.bottom - LuhPx(2), Field->Box.right - LuhPx(3),
                Field->Box.bottom);
        LuhFillRect(Surface, &Line, LUH_ACCENT, 255);
    }
    if (Index == g_Ui.ArrowField)
    {
        LuhDrawArrow(Surface, &g_Ui.Arrow);
        if (!IsRectEmpty(&g_Ui.Eye) && Field->Control && GetWindowTextLengthW(Field->Control) > 0)
            LuhDrawEye(Surface, &g_Ui.Eye);
    }
}

/* The sign-in page's still picture: avatar, name, any further text lines, boxes, result, Caps Lock. */
static void
LuhComposeSignIn(
    _Inout_ PLUH_SURFACE Surface,
    _In_ const RECT *Client)
{
    const COLORREF White = RGB(255, 255, 255);
    LONG Cx = Client->right / 2;
    RECT Rect;
    DWORD i;

    PLUH_PAGE Page = g_Ui.Page;

    LuhFillCircle(Surface, Cx, g_Ui.AvatarY, LuhPx(LUH_AVATAR_RADIUS), White, 38);
    LuhDrawPerson(Surface, Cx, g_Ui.AvatarY, LuhPx(2 * LUH_AVATAR_RADIUS), RGB(232, 234, 245));
    if (Page->Scenario == CPUS_CHANGE_PASSWORD)
    {
        SetRect(&Rect, 0, Client->bottom / 2 + LuhPx(LUH_CHANGE_TITLE_DY) - LuhPx(22), Client->right,
                Client->bottom / 2 + LuhPx(LUH_CHANGE_TITLE_DY) + LuhPx(22));
        LuhDrawText(Surface, g_Ui.TitleFont, LuhText(IDS_LUH_CHANGE_PASSWORD), &Rect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);
    }

    for (i = 0; i < g_Ui.Page->Count; i++)
    {
        PLUH_FIELD Field = &g_Ui.Page->Fields[i];

        if (!LuhFieldVisible(Field))
            continue;
        if (IsRectEmpty(&Field->Box))
            continue;
        if (Page->Scenario == CPUS_CHANGE_PASSWORD && i == g_Ui.NameField)
        {
            /* The account, read only, in the first box. */
            if (!g_Ui.SignInBusy && !g_Ui.ResultUp && !g_Ui.StatusOnPage)
            {
                Rect = Field->Box;
                LuhFillRound(Surface, &Rect, LuhPx(4), g_Ui.BoxFill, 255);
                LuhFrameRound(Surface, &Rect, LuhPx(4), 1, White, 70);
                Rect.left += LuhPx(10);
                LuhDrawText(Surface, g_Ui.TextFont, Field->Value[0] ? Field->Value : Field->Label, &Rect,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);
            }
        }
        else if (Field->Type == CPFT_LARGE_TEXT || Field->Type == CPFT_SMALL_TEXT)
        {
            LuhDrawText(Surface, i == g_Ui.NameField ? g_Ui.NameFont : g_Ui.TextFont,
                        Field->Value[0] ? Field->Value : Field->Label, &Field->Box,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);
        }
        else if ((Field->Type == CPFT_EDIT_TEXT || Field->Type == CPFT_PASSWORD_TEXT) && Field->Control &&
                 !g_Ui.SignInBusy && !g_Ui.ResultUp && !g_Ui.StatusOnPage)
        {
            LuhDrawBox(Surface, i);
        }
    }

    if (g_Ui.StatusOnPage)
    {
        /* Shutting down / Restarting: the picture and name stay, the dots and the text replace the rest. */
        g_Ui.SpinnerY = Client->bottom / 2 + LuhPx(LUH_POWER_DOTS_DY);
        SetRect(&Rect, 0, Client->bottom / 2 + LuhPx(LUH_POWER_TEXT_DY) - LuhPx(18), Client->right,
                Client->bottom / 2 + LuhPx(LUH_POWER_TEXT_DY) + LuhPx(18));
        LuhDrawText(Surface, g_Ui.StatusFont, g_Ui.Title, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    White, 255);
        return;
    }
    if (g_Ui.SignInBusy || g_Ui.ResultUp)
    {
        /* In place of the fields: the dots and Welcome, or the result above its OK. */
        if (g_Ui.SignInBusy)
        {
            g_Ui.SpinnerY = g_Ui.PageTop + LuhPx(LUH_BOX_HEIGHT) / 2;
            SetRect(&Rect, 0, g_Ui.SpinnerY + LuhPx(LUH_WELCOME_TEXT_DY) - LuhPx(16), Client->right,
                    g_Ui.SpinnerY + LuhPx(LUH_WELCOME_TEXT_DY) + LuhPx(16));
            LuhDrawText(Surface, g_Ui.TextFont, LuhText(IDS_LUH_WELCOME), &Rect,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);
        }
        else
        {
            SetRect(&Rect, Cx - LuhPx(LUH_DETAIL_WIDTH) / 2, g_Ui.PageTop + LuhPx(LUH_RESULT_TEXT_DY) - LuhPx(24),
                    Cx + LuhPx(LUH_DETAIL_WIDTH) / 2, g_Ui.PageTop + LuhPx(LUH_RESULT_OK_DY) - LuhPx(8));
            LuhDrawText(Surface, g_Ui.TextFont, g_Ui.Detail, &Rect, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS,
                        White, 255);
        }
        return;
    }

    if (g_Ui.Detail[0] != UNICODE_NULL)
    {
        SetRect(&Rect, Cx - LuhPx(LUH_DETAIL_WIDTH) / 2, g_Ui.DetailTop, Cx + LuhPx(LUH_DETAIL_WIDTH) / 2,
                g_Ui.DetailTop + LuhPx(48));
        LuhDrawText(Surface, g_Ui.TextFont, g_Ui.Detail, &Rect, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS,
                    White, 255);
    }
    g_Ui.SpinnerY = g_Ui.DetailTop + LuhPx(72);

    if (g_Ui.CapsControl != NULL && g_Ui.CapsTop > 0)
    {
        PLUH_FIELD Field = LuhFieldOfControl(g_Ui.CapsControl);

        if (Field)
        {
            SetRect(&Rect, Field->Box.left, g_Ui.CapsTop, Field->Box.right, g_Ui.CapsTop + LuhPx(LUH_CAPS_GAP));
            LuhDrawText(Surface, g_Ui.StrongFont, LuhText(IDS_LUH_CAPS_LOCK), &Rect,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, White, 255);
        }
    }
}

/* Press and hold the eye: the password shows while the button is down, as on Windows 11. */
static void
LuhReveal(
    _In_ HWND Window,
    _In_ BOOL On)
{
    PLUH_FIELD Field;

    if (g_Ui.ArrowField == (DWORD)-1 || g_Ui.Page == NULL)
        return;
    Field = &g_Ui.Page->Fields[g_Ui.ArrowField];
    if (Field->Control == NULL || Field->Type != CPFT_PASSWORD_TEXT || On == g_Ui.Revealing || (On && g_Ui.NoReveal))
        return;
    if (On)
        g_Ui.PasswordChar = (WCHAR)SendMessageW(Field->Control, EM_GETPASSWORDCHAR, 0, 0);
    g_Ui.Revealing = On;
    SendMessageW(Field->Control, EM_SETPASSWORDCHAR, On ? 0 : g_Ui.PasswordChar, 0);
    InvalidateRect(Field->Control, NULL, TRUE);
    if (On)
        SetCapture(Window);
    else if (GetCapture() == Window)
        ReleaseCapture();
}

static void LuhFocusForRetry(void);

/* The page's input controls (boxes, Sign in, links, check boxes), shown or hidden together. */
static void
LuhShowFields(
    _In_ BOOL Show)
{
    DWORD i;

    if (g_Ui.Page == NULL)
        return;
    for (i = 0; i < g_Ui.Page->Count; i++)
        if (g_Ui.Page->Fields[i].Control)
            ShowWindow(g_Ui.Page->Fields[i].Control, Show ? SW_SHOW : SW_HIDE);
    if (g_Ui.CancelButton)
        ShowWindow(g_Ui.CancelButton, Show ? SW_SHOW : SW_HIDE);
}

/* A refused sign-in, as Windows 11 shows it: the message where the fields were, and OK under it. */
static void
LuhShowResult(
    _In_ HWND Window,
    _In_ PCWSTR Text)
{
    RECT Client;

    LuhReveal(Window, FALSE);
    if (Text != g_Ui.Detail)
        StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), Text);
    g_Ui.SignInBusy = FALSE;
    g_Ui.ResultUp = TRUE;
    LuhShowFields(FALSE);
    if (g_Ui.OkButton == NULL)
    {
        GetClientRect(Window, &Client);
        g_Ui.OkButton = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_OK),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        Client.right / 2 - LuhPx(LUH_PILL_WIDTH) / 2,
                                        g_Ui.PageTop + LuhPx(LUH_RESULT_OK_DY), LuhPx(LUH_PILL_WIDTH),
                                        LuhPx(LUH_PILL_HEIGHT), Window, (HMENU)LUH_ID_RESULT_OK, NULL, NULL);
    }
    if (g_Ui.OkButton)
        SetFocus(g_Ui.OkButton);
    LuhUpdateCapsLock(Window);
    LuhPageChanged(Window);
}

/* OK, Enter or Esc: the fields come back, the keyboard goes to the cleared password. */
static void
LuhDismissResult(
    _In_ HWND Window)
{
    if (!g_Ui.ResultUp)
        return;
    g_Ui.ResultUp = FALSE;
    g_Ui.Detail[0] = UNICODE_NULL;
    if (g_Ui.OkButton)
    {
        DestroyWindow(g_Ui.OkButton);
        g_Ui.OkButton = NULL;
    }
    LuhShowFields(TRUE);
    LuhFocusForRetry();
    LuhUpdateCapsLock(Window);
    LuhPageChanged(Window);
}

/*
 * Status without a page (Welcome after the hand-over, Signing out, Please wait): black, the
 * dots above the centre and the text under them, as on the Windows 11 shots.
 */
static void
LuhComposeStatus(
    _Inout_ PLUH_SURFACE Surface,
    _In_ const RECT *Client)
{
    LONG Cy = Client->bottom / 2;
    RECT Rect;

    LuhFillRect(Surface, Client, RGB(0, 0, 0), 255);
    g_Ui.SpinnerY = Cy + LuhPx(LUH_STATUS_DOTS_DY);
    SetRect(&Rect, 0, Cy + LuhPx(LUH_STATUS_TEXT_DY) - LuhPx(20), Client->right, Cy + LuhPx(LUH_STATUS_TEXT_DY) + LuhPx(20));
    LuhDrawText(Surface, g_Ui.StatusFont, g_Ui.Title, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                RGB(255, 255, 255), 255);
    if (g_Ui.Detail[0] != UNICODE_NULL)
    {
        SetRect(&Rect, Client->right / 2 - LuhPx(LUH_DETAIL_WIDTH) / 2, Rect.bottom + LuhPx(8),
                Client->right / 2 + LuhPx(LUH_DETAIL_WIDTH) / 2, Rect.bottom + LuhPx(64));
        LuhDrawText(Surface, g_Ui.TextFont, g_Ui.Detail, &Rect, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS,
                    RGB(255, 255, 255), 255);
    }
}

/* ---- power flyout and full-screen notices ------------------------------------------- */

static void
LuhCloseFlyout(
    _In_ HWND Window,
    _In_ BOOL FocusPower)
{
    DWORD i;

    if (!g_Ui.FlyoutUp)
        return;
    g_Ui.FlyoutUp = FALSE;
    for (i = 0; i < ARRAYSIZE(g_Ui.FlyoutButtons); i++)
    {
        if (g_Ui.FlyoutButtons[i])
        {
            DestroyWindow(g_Ui.FlyoutButtons[i]);
            g_Ui.FlyoutButtons[i] = NULL;
        }
    }
    if (FocusPower && g_Ui.PowerButton)
        SetFocus(g_Ui.PowerButton);
    LuhPageChanged(Window);
}

static BOOL
LuhIsFlyoutRow(
    _In_opt_ HWND Control)
{
    DWORD i;

    for (i = 0; Control && i < ARRAYSIZE(g_Ui.FlyoutButtons); i++)
        if (g_Ui.FlyoutButtons[i] == Control)
            return TRUE;
    return FALSE;
}

static BOOL
LuhCanSleep(VOID)
{
    SYSTEM_POWER_CAPABILITIES Caps;

    ZeroMemory(&Caps, sizeof(Caps));
    if (CallNtPowerInformation(SystemPowerCapabilities, NULL, 0, &Caps, sizeof(Caps)) != 0)
        return FALSE;
    return Caps.SystemS3 || Caps.AoAc;
}

/*
 * Windows 11's power flyout: a small dark card above the power button, one row per action with
 * its symbol, the focused row outlined, and what it does in a tooltip above the card. The choice
 * goes to winlogon, which decides.
 */
static void
LuhOpenFlyout(
    _In_ HWND Window)
{
    static const UINT All[] = { LUH_ID_SLEEP, LUH_ID_SHUTDOWN, LUH_ID_RESTART };
    UINT Rows[ARRAYSIZE(All)];
    RECT Client, Button;
    LONG Width = LuhPx(LUH_FLYOUT_WIDTH), Row = LuhPx(LUH_FLYOUT_ROW), Pad = LuhPx(4), Top;
    DWORD i, Count = 0;

    /* Sleep only where the machine can sleep (S3 or modern standby), as on Windows 11. */
    for (i = 0; i < ARRAYSIZE(All); i++)
        if (All[i] != LUH_ID_SLEEP || LuhCanSleep())
            Rows[Count++] = All[i];

    GetClientRect(Window, &Client);
    GetWindowRect(g_Ui.PowerButton, &Button);
    MapWindowPoints(NULL, Window, (LPPOINT)&Button, 2);
    Top = Button.top - LuhPx(8) - (LONG)Count * Row - 2 * Pad;
    SetRect(&g_Ui.FlyoutCard, Client.right - LuhPx(4) - Width, Top, Client.right - LuhPx(4),
            Button.top - LuhPx(8));
    for (i = 0; i < Count; i++)
    {
        g_Ui.FlyoutButtons[i] = CreateWindowExW(0, L"BUTTON",
                                                LuhText(Rows[i] == LUH_ID_RESTART ? IDS_LUH_RESTART :
                                                        Rows[i] == LUH_ID_SLEEP ? IDS_LUH_SLEEP : IDS_LUH_SHUTDOWN),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
                                                g_Ui.FlyoutCard.left + Pad, Top + Pad + (LONG)i * Row,
                                                Width - 2 * Pad, Row, Window, (HMENU)(UINT_PTR)Rows[i], NULL, NULL);
    }
    g_Ui.FlyoutUp = TRUE;
    if (g_Ui.FlyoutButtons[0])
        SetFocus(g_Ui.FlyoutButtons[0]);
    LuhTrace("power", L"flyout");
    LuhPageChanged(Window);
}

/*
 * A full-screen notice on black (legal notice, emergency restart): caption, wrapped text and OK
 * (and Cancel), as Windows 11 shows the legal notice. Modal on the UI thread; winlogon's commands
 * are still dispatched under it, and a Dismiss or the desktop switch ends it.
 */
static BOOL
LuhRunNotice(
    _In_ HWND Window,
    _In_ PCWSTR Caption,
    _In_ PCWSTR Text,
    _In_ BOOL WithCancel)
{
    RECT Client, Measure;
    LONG Cx, Width, W = LuhPx(LUH_NOTICE_BUTTON_W), H = LuhPx(LUH_NOTICE_BUTTON_H), Y;
    HWND Ok, Cancel = NULL;
    HGDIOBJ Old;
    HDC Dc;
    MSG Msg;
    BOOL Got;

    GetClientRect(Window, &Client);
    Cx = Client.right / 2;
    Width = min(Client.right - LuhPx(64), LuhPx(LUH_NOTICE_TEXT_WIDTH));
    SetRect(&Measure, 0, 0, Width, 0);
    Dc = GetDC(Window);
    Old = SelectObject(Dc, g_Ui.TextFont);
    DrawTextW(Dc, Text, -1, &Measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(Dc, Old);
    ReleaseDC(Window, Dc);

    g_Ui.NoticeCaption = Caption;
    g_Ui.NoticeText = Text;
    SetRect(&g_Ui.NoticeTextRect, Cx - Width / 2, Client.bottom / 2 - LuhPx(4), Cx + Width / 2,
            Client.bottom / 2 - LuhPx(4) + (Measure.bottom - Measure.top));
    Y = g_Ui.NoticeTextRect.bottom + LuhPx(24);
    Ok = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_OK), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                         WithCancel ? Cx - W - LuhPx(4) : Cx - W / 2, Y, W, H, Window, (HMENU)LUH_ID_NOTICE_OK,
                         NULL, NULL);
    if (WithCancel)
        Cancel = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_CANCEL), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 Cx + LuhPx(4), Y, W, H, Window, (HMENU)LUH_ID_NOTICE_CANCEL, NULL, NULL);
    g_Ui.NoticeUp = TRUE;
    g_Ui.NoticeOk = FALSE;
    SetFocus(WithCancel && Cancel ? Cancel : Ok);   /* the safe choice first, as the old box did */
    LuhPageChanged(Window);

    while (g_Ui.NoticeUp && !g_Ui.Exiting)
    {
        Got = GetMessageW(&Msg, NULL, 0, 0);
        if (Got <= 0)
        {
            PostQuitMessage((int)Msg.wParam);   /* leave it for the main loop */
            break;
        }
        if (Msg.message == WM_KEYDOWN && (Msg.wParam == VK_RETURN || Msg.wParam == VK_ESCAPE))
        {
            if (Msg.wParam == VK_RETURN)
                g_Ui.NoticeOk = (GetFocus() != Cancel || Cancel == NULL);
            else if (!WithCancel)
                continue;                       /* a legal notice is acknowledged, not escaped */
            g_Ui.NoticeUp = FALSE;
            continue;
        }
        if (!IsDialogMessageW(Window, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }

    g_Ui.NoticeUp = FALSE;
    if (Ok)
        DestroyWindow(Ok);
    if (Cancel)
        DestroyWindow(Cancel);
    g_Ui.NoticeCaption = g_Ui.NoticeText = NULL;
    LuhPageChanged(Window);
    return g_Ui.NoticeOk;
}

/* The power button: the flyout, or with Ctrl on the Ctrl+Alt+Del screen, the emergency restart. */
static void
LuhPowerMenu(
    _In_ HWND Window)
{
    DWORD i, Shown = 0;
    BOOL Ok;

    if (g_Ui.PowerButton == NULL || g_Ui.PageBusy)
        return;
    if (g_Ui.FlyoutUp)
    {
        LuhCloseFlyout(Window, TRUE);
        return;
    }
    if (g_Ui.OptionsUp && (GetKeyState(VK_CONTROL) & 0x8000))
    {
        /* The options give way to the notice and come back if it is cancelled. */
        for (i = 0; i < ARRAYSIZE(g_Ui.OptionButtons); i++)
            if (g_Ui.OptionButtons[i])
                ShowWindow(g_Ui.OptionButtons[i], SW_HIDE);
        ShowWindow(g_Ui.PowerButton, SW_HIDE);
        Ok = LuhRunNotice(Window, LuhText(IDS_LUH_EMERGENCY_TITLE), LuhText(IDS_LUH_EMERGENCY_TEXT), TRUE);
        if (Ok)
        {
            LuhTrace("power", L"emergency");
            WluiClientRequestAction(&g_Connection, WluiActionEmergencyRestart);
        }
        if (g_Ui.OptionsUp)
        {
            for (i = 0; i < ARRAYSIZE(g_Ui.OptionButtons); i++)
            {
                if (g_Ui.OptionButtons[i])
                {
                    ShowWindow(g_Ui.OptionButtons[i], SW_SHOW);
                    if (Shown++ == 0 && !Ok)
                        SetFocus(g_Ui.OptionButtons[i]);
                }
            }
            if (g_Ui.PowerButton)
                ShowWindow(g_Ui.PowerButton, SW_SHOW);
        }
        return;
    }
    LuhOpenFlyout(Window);
}

/* Shutting down or Restarting with a sign-in page up: the page's controls go in one cut. */
static void
LuhHidePageChrome(
    _In_ HWND Window)
{
    DWORD i;

    LuhCloseFlyout(Window, FALSE);
    LuhShowFields(FALSE);
    for (i = 0; i < LUH_MAX_TILES; i++)
        if (g_Ui.TileButtons[i])
            ShowWindow(g_Ui.TileButtons[i], SW_HIDE);
    if (g_Ui.PowerButton)
        ShowWindow(g_Ui.PowerButton, SW_HIDE);
    if (g_Ui.OkButton)
        ShowWindow(g_Ui.OkButton, SW_HIDE);
}

/* Lays the page out as on Windows 11: avatar and name, then the fields in drawn boxes. */
static void
LuhCreatePage(
    _In_ HWND Window,
    _In_ PLUH_PAGE Page)
{
    RECT Client;
    LONG Cx, Cy, Y, Inner, Height;
    DWORD i, LastPassword = (DWORD)-1;
    HWND Focus = NULL;
    WCHAR Num[16];
    PLUH_FIELD Adjacent;
    TEXTMETRICW Metrics;
    HDC Dc;
    HGDIOBJ Old;
    ULONG Pixel;
    BOOL Change;
    LONG Pitch;

    LuhDestroyPage();
    g_Ui.Page = Page;
    GetClientRect(Window, &Client);
    Cx = Client.right / 2;
    Cy = Client.bottom / 2;

    g_Ui.NameField = (DWORD)-1;
    g_Ui.ArrowField = (DWORD)-1;
    SetRectEmpty(&g_Ui.Arrow);
    SetRectEmpty(&g_Ui.Eye);
    {
        /* The machine policy that removes the reveal button; on the logon desktop it is HKLM's. */
        DWORD Value = 0, Size = sizeof(Value);

        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CredUI",
                         L"DisablePasswordReveal", RRF_RT_REG_DWORD, NULL, &Value, &Size) != ERROR_SUCCESS)
            Value = 0;
        g_Ui.NoReveal = (Value != 0);
    }
    for (i = 0; i < Page->Count; i++)
    {
        if (!LuhFieldVisible(&Page->Fields[i]))
            continue;
        if (Page->Fields[i].Type == CPFT_PASSWORD_TEXT)
            LastPassword = i;
        if (Page->Fields[i].Type == CPFT_LARGE_TEXT && g_Ui.NameField == (DWORD)-1)
            g_Ui.NameField = i;
    }
    /* The submit button next to a box is the arrow inside that box, as on Windows 11. */
    Adjacent = LuhFindField(Page->SubmitAdjacentTo);
    if (LuhFindField(Page->SubmitField) && Adjacent && LuhFieldVisible(Adjacent) &&
        (Adjacent->Type == CPFT_EDIT_TEXT || Adjacent->Type == CPFT_PASSWORD_TEXT))
        g_Ui.ArrowField = (DWORD)(Adjacent - Page->Fields);

    /* One solid box colour, from the backdrop under the boxes, so the edits' own background matches. */
    GdiFlush();
    Pixel = g_Ui.Soft.Bits[(SIZE_T)min(Cy + LuhPx(LUH_BOX_DY), g_Ui.Soft.Height - 1) * g_Ui.Soft.Width + Cx];
    g_Ui.BoxFill = RGB(((Pixel >> 16) & 0xFF) / 2, ((Pixel >> 8) & 0xFF) / 2, (Pixel & 0xFF) / 2);
    g_Ui.BoxBrush = CreateSolidBrush(g_Ui.BoxFill);

    Dc = GetDC(Window);
    Old = SelectObject(Dc, g_Ui.TextFont);
    GetTextMetricsW(Dc, &Metrics);
    SelectObject(Dc, Old);
    ReleaseDC(Window, Dc);
    Height = Metrics.tmHeight + 2;

    Change = (Page->Scenario == CPUS_CHANGE_PASSWORD);
    Pitch = LuhPx(Change ? LUH_CHANGE_PITCH : LUH_BOX_PITCH);
    g_Ui.AvatarY = Cy + LuhPx(Change ? LUH_CHANGE_AVATAR_DY : LUH_AVATAR_DY);
    Y = Cy + LuhPx(LUH_NAME_DY) - LuhPx(18);
    if (Change)
    {
        /* Windows 11 puts the account in the first box, under the page title. */
        Y = Cy + LuhPx(LUH_CHANGE_FIELDS_DY);
        if (g_Ui.NameField != (DWORD)-1)
        {
            SetRect(&Page->Fields[g_Ui.NameField].Box, Cx - LuhPx(LUH_BOX_WIDTH) / 2, Y,
                    Cx + LuhPx(LUH_BOX_WIDTH) / 2, Y + LuhPx(LUH_BOX_HEIGHT));
            Y += Pitch;
        }
    }
    for (i = 0; i < Page->Count; i++)
    {
        PLUH_FIELD Field = &Page->Fields[i];

        if (!LuhFieldVisible(Field) || (Field->Type != CPFT_LARGE_TEXT && Field->Type != CPFT_SMALL_TEXT))
            continue;
        if (Change && (i == g_Ui.NameField || Field->Type == CPFT_SMALL_TEXT))
        {
            if (i != g_Ui.NameField)
                SetRectEmpty(&Field->Box);  /* the provider label: Windows 11 shows none here */
            continue;
        }
        if (i == g_Ui.NameField)
        {
            SetRect(&Field->Box, 0, Y, Client.right, Y + LuhPx(36));
            Y += LuhPx(40);
        }
        else
        {
            SetRect(&Field->Box, 0, Y, Client.right, Y + LuhPx(22));
            Y += LuhPx(24);
        }
    }
    if (!Change)
        Y = max(Y + LuhPx(8), Cy + LuhPx(LUH_BOX_DY));
    g_Ui.PageTop = Change && g_Ui.NameField != (DWORD)-1 ? Page->Fields[g_Ui.NameField].Box.top : Y;
    g_Ui.CapsTop = 0;

    for (i = 0; i < Page->Count; i++)
    {
        PLUH_FIELD Field = &Page->Fields[i];
        DWORD Style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
        LONG Left = Cx - LuhPx(LUH_BOX_WIDTH) / 2, Right = Cx + LuhPx(LUH_BOX_WIDTH) / 2;

        if (!LuhFieldVisible(Field))
            continue;

        switch (Field->Type)
        {
            case CPFT_EDIT_TEXT:
            case CPFT_PASSWORD_TEXT:
                SetRect(&Field->Box, Left, Y, Right, Y + LuhPx(LUH_BOX_HEIGHT));
                Inner = Right;
                if (i == g_Ui.ArrowField)
                {
                    SetRect(&g_Ui.Arrow, Right - LuhPx(LUH_BOX_BUTTON), Y, Right, Field->Box.bottom);
                    Inner = g_Ui.Arrow.left;
                    if (Field->Type == CPFT_PASSWORD_TEXT && !g_Ui.NoReveal)
                    {
                        SetRect(&g_Ui.Eye, Inner - LuhPx(LUH_BOX_BUTTON), Y, Inner, Field->Box.bottom);
                        Inner = g_Ui.Eye.left;
                    }
                }
                Style |= ES_AUTOHSCROLL | (Field->Type == CPFT_PASSWORD_TEXT ? ES_PASSWORD : 0);
                Field->Control = CreateWindowExW(0, L"EDIT", Field->Type == CPFT_EDIT_TEXT ? Field->Value : L"", Style,
                                                 Left + LuhPx(10), Y + (LuhPx(LUH_BOX_HEIGHT) - Height) / 2,
                                                 Inner - Left - LuhPx(12), Height, Window,
                                                 (HMENU)(UINT_PTR)(LUH_ID_FIELD_BASE + i), NULL, NULL);
                if (Field->Control)
                {
                    SendMessageW(Field->Control, WM_SETFONT, (WPARAM)g_Ui.TextFont, FALSE);
                    SendMessageW(Field->Control, EM_SETLIMITTEXT, LUH_FIELD_TEXT - 1, 0);
                    g_Ui.EditProc = (WNDPROC)SetWindowLongPtrW(Field->Control, GWLP_WNDPROC, (LONG_PTR)LuhEditProc);
                    if (Field->Interactive == CPFIS_DISABLED)
                        EnableWindow(Field->Control, FALSE);
                    else if (Focus == NULL || Field->Interactive == CPFIS_FOCUSED)
                        Focus = Field->Control;
                }
                Y += Pitch;
                if (i == LastPassword)
                {
                    /* The Caps Lock line sits right under the last password box. */
                    g_Ui.CapsTop = Field->Box.bottom + LuhPx(8);
                    Y += LuhPx(LUH_CAPS_GAP);
                }
                break;

            case CPFT_SUBMIT_BUTTON:
                if (g_Ui.ArrowField != (DWORD)-1)
                    break;
                Field->Control = CreateWindowExW(0, L"BUTTON", Field->Label, Style | BS_OWNERDRAW,
                                                 Cx - LuhPx(LUH_PILL_WIDTH) / 2, Y, LuhPx(LUH_PILL_WIDTH),
                                                 LuhPx(LUH_PILL_HEIGHT), Window,
                                                 (HMENU)(UINT_PTR)(LUH_ID_FIELD_BASE + i), NULL, NULL);
                if (Field->Control && Focus == NULL)
                    Focus = Field->Control;
                Y += LuhPx(LUH_PILL_HEIGHT + 12);
                break;

            case CPFT_COMMAND_LINK:
                Field->Control = CreateWindowExW(0, L"BUTTON", Field->Label, Style | BS_OWNERDRAW,
                                                 Left, Y, Right - Left, LuhPx(28), Window,
                                                 (HMENU)(UINT_PTR)(LUH_ID_FIELD_BASE + i), NULL, NULL);
                Y += LuhPx(32);
                break;

            case CPFT_CHECKBOX:
                Field->Control = CreateWindowExW(0, L"BUTTON", Field->Label, Style | BS_AUTOCHECKBOX,
                                                 Left, Y, Right - Left, LuhPx(28), Window,
                                                 (HMENU)(UINT_PTR)(LUH_ID_FIELD_BASE + i), NULL, NULL);
                if (Field->Control)
                    SendMessageW(Field->Control, WM_SETFONT, (WPARAM)g_Ui.TextFont, FALSE);
                Y += LuhPx(32);
                break;

            default:
                break;  /* texts and the picture are drawn; combo boxes are not offered by our providers */
        }
    }
    g_Ui.DetailTop = Y + LuhPx(4);

    if (g_Ui.ChangeCancel && Page->Scenario == CPUS_CHANGE_PASSWORD)
    {
        g_Ui.CancelButton = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_CANCEL),
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                            Cx - LuhPx(LUH_PILL_WIDTH) / 2,
                                            max(Cy + LuhPx(LUH_CHANGE_CANCEL_DY) - LuhPx(16), Y + LuhPx(24)),
                                            LuhPx(LUH_PILL_WIDTH), LuhPx(LUH_PILL_HEIGHT), Window,
                                            (HMENU)LUH_ID_OPTION_CANCEL, NULL, NULL);
    }
    LuhCreateTileList(Window, Page);
    if (!Change)
        LuhCreatePowerButton(Window);   /* Windows 11 has no power button on the change page */
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
    LuhPageChanged(Window);
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

/* The legal notice, before any credential page exists (winlogon sets the flag): Policies\System
 * first, then the Winlogon key; shown full screen on black as Windows 11 does. */
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
    /* Modal on the UI thread: the heartbeat timer is still dispatched by the notice's loop. */
    LuhRunNotice(Window, Caption[0] ? Caption : LuhText(IDS_LUH_NOTICE), Text, FALSE);
    HeapFree(GetProcessHeap(), 0, Text);
}

static void
LuhShowPolicyPage(
    _In_ HWND Window)
{
    RECT Client;
    LONG X, Y, W = LuhPx(LUH_NOTICE_BUTTON_W), H = LuhPx(LUH_NOTICE_BUTTON_H);

    LuhDestroyPage();
    GetClientRect(Window, &Client);
    StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), LuhText(IDS_LUH_NO_SIGNIN));
    StringCchCopyW(g_Ui.Detail, ARRAYSIZE(g_Ui.Detail), LuhText(IDS_LUH_CONTACT_ADMIN));
    LuhSetSpinner(Window, FALSE);
    X = Client.right / 2;
    Y = Client.bottom / 2 + LuhPx(LUH_STATUS_TEXT_DY) + LuhPx(100);
    /* ShutdownWithoutLogon=0: no power buttons; winlogon refuses the action anyway. */
    if (!g_Options.NoPower)
    {
        g_Ui.PolicyButtons[0] = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_RESTART),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                                X - W - LuhPx(4), Y, W, H, Window, (HMENU)LUH_ID_RESTART, NULL, NULL);
        g_Ui.PolicyButtons[1] = CreateWindowExW(0, L"BUTTON", LuhText(IDS_LUH_SHUTDOWN),
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                                X + LuhPx(4), Y, W, H, Window, (HMENU)LUH_ID_SHUTDOWN, NULL, NULL);
    }
    LuhTrace("policy-page", NULL);
    LuhPageChanged(Window);
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
    /* Enter while the eye is held: mask again before anything else happens. */
    LuhReveal(Window, FALSE);

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
            /* Windows 11: the fields give way to the dots and Welcome, on the same picture. */
            LuhTrace("submit-status", Status);
            g_Ui.Detail[0] = UNICODE_NULL;
            LuhShowFields(FALSE);
            g_Ui.SignInBusy = TRUE;
            LuhSetSpinner(Window, TRUE);
            break;                          /* ReportResult will re-enable the page */
        case LuhSubmitNotFinished:
        case LuhSubmitFailed:
        case LuhSubmitRefused:
            LuhClearSecrets();
            LuhEnablePage(TRUE);
            LuhShowResult(Window, Status[0] ? Status : LuhText(IDS_LUH_SIGNIN_FAILED));
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
        /* Refused: keep the page; the plate stays on the selected tile. */
        if (g_Ui.TileButtons[Index])
            InvalidateRect(g_Ui.TileButtons[Index], NULL, FALSE);
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
    /* Texts are drawn, not controls: the still picture has to follow. */
    if (g_Ui.Window)
        LuhPageChanged(g_Ui.Window);
}

/* ---- the curtain ------------------------------------------------------------------ */

/* ---- backdrop and lock screen ----------------------------------------------------- */

#define LUH_SOFT_SCALE      8       /* the blur runs on an eighth of the screen, then scales up */
#define LUH_SOFT_RADIUS     3
#define LUH_SOFT_DARKEN     96      /* about 38 % black over the blurred wallpaper (Win11 shots) */
#define LUH_LIFT_MS         350     /* a whole-screen lift; a partial one takes its share */
#define LUH_LIFT_MIN_MS     120
#define LUH_FADE_IN_MS      400     /* lock: the curtain comes up from black (Win11 lock-cad frames) */
#define WM_LUH_FADE_IN      (WM_APP + 16)
#define WM_LUH_FLYOUT_FOCUS (WM_APP + 17)
#define LUH_LIFT_VELOCITY   0.6     /* pixels per millisecond upwards at release: a flick lifts */

/*
 * The host runs as SYSTEM on the secure desktop, so the picture it decodes must be one only
 * the system and administrators can change: the machine's lock-screen image (policy), else
 * the system default. The user's own wallpaper is never read here.
 */
#define LUH_LOCK_POLICY_KEY L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization"
#define LUH_LOCK_DEFAULT    L"%SystemRoot%\\Web\\Wallpaper\\Layered Glass 16x9.png"
#define LUH_FILE_WRITE      (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER | \
                             GENERIC_WRITE | GENERIC_ALL)
#define LUH_DIR_WRITE       (FILE_ADD_FILE | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | \
                             GENERIC_WRITE | GENERIC_ALL)

/* SYSTEM, Administrators or TrustedInstaller. */
static BOOL
LuhSidIsAdmin(
    _In_ PSID Sid)
{
    SID_IDENTIFIER_AUTHORITY Nt = SECURITY_NT_AUTHORITY;
    PSID Installer = NULL;
    BOOL Match;

    if (IsWellKnownSid(Sid, WinLocalSystemSid) || IsWellKnownSid(Sid, WinBuiltinAdministratorsSid))
        return TRUE;
    if (!AllocateAndInitializeSid(&Nt, 6, SECURITY_SERVICE_ID_BASE_RID, 956008885, 3418522649,
                                  1831038044, 1853292631, 2271478464, 0, 0, &Installer))
        return FALSE;
    Match = EqualSid(Sid, Installer);
    FreeSid(Installer);
    return Match;
}

/* Owned by an administrator, and no one else is granted any of WriteMask. */
static BOOL
LuhOnlyAdminsWrite(
    _In_opt_ PSID Owner,
    _In_opt_ PACL Dacl,
    _In_ ACCESS_MASK WriteMask)
{
    ACL_SIZE_INFORMATION Size;
    PACE_HEADER Ace;
    DWORD i;

    /* A NULL DACL grants everyone everything; the owner can always rewrite the DACL. */
    if (Owner == NULL || Dacl == NULL || !LuhSidIsAdmin(Owner) ||
        !GetAclInformation(Dacl, &Size, sizeof(Size), AclSizeInformation))
        return FALSE;
    for (i = 0; i < Size.AceCount; i++)
    {
        if (!GetAce(Dacl, i, (PVOID *)&Ace))
            return FALSE;
        if (Ace->AceFlags & INHERIT_ONLY_ACE)
            continue;
        if (Ace->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (Ace->AceType != ACCESS_ALLOWED_ACE_TYPE)
            return FALSE;   /* object or callback grants: not judged, so not trusted */
        if ((((PACCESS_ALLOWED_ACE)Ace)->Mask & WriteMask) &&
            !LuhSidIsAdmin((PSID)&((PACCESS_ALLOWED_ACE)Ace)->SidStart))
            return FALSE;
    }
    return TRUE;
}

/* Every directory above the file: a writable one would let a user swap the file or a folder. */
static BOOL
LuhParentsAdminOnly(
    _In_ PCWSTR Path)
{
    WCHAR Dir[MAX_PATH];
    PWSTR Slash;
    PSID Owner;
    PACL Dacl;
    PSECURITY_DESCRIPTOR Sd;
    DWORD Attributes, Error;
    BOOL Ok;

    if (FAILED(StringCchCopyW(Dir, ARRAYSIZE(Dir), Path)))
        return FALSE;
    for (;;)
    {
        Slash = wcsrchr(Dir, L'\\');
        if (Slash == NULL || Slash == Dir)
            return FALSE;
        /* "C:\" keeps its backslash and is the last one checked. */
        if (Slash == Dir + 2 && Dir[1] == L':')
            Slash[1] = UNICODE_NULL;
        else
            Slash[0] = UNICODE_NULL;

        Attributes = GetFileAttributesW(Dir);
        if (Attributes == INVALID_FILE_ATTRIBUTES || !(Attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (Attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return FALSE;
        Error = GetNamedSecurityInfoW(Dir, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                      &Owner, NULL, &Dacl, NULL, &Sd);
        if (Error != ERROR_SUCCESS)
            return FALSE;
        Ok = LuhOnlyAdminsWrite(Owner, Dacl, LUH_DIR_WRITE);
        LocalFree(Sd);
        if (!Ok)
            return FALSE;
        if (Dir[1] == L':' && Dir[2] == L'\\' && Dir[3] == UNICODE_NULL)
            return TRUE;
    }
}

/*
 * Opens the backdrop picture for reading, or returns INVALID_HANDLE_VALUE. The checks run on
 * the open handle (write and delete sharing denied), so the file cannot change after them.
 */
static HANDLE
LuhOpenBackdrop(VOID)
{
    WCHAR Path[MAX_PATH], Full[MAX_PATH];
    DWORD Size = sizeof(Path), Error;
    BY_HANDLE_FILE_INFORMATION Info;
    PSID Owner;
    PACL Dacl;
    PSECURITY_DESCRIPTOR Sd;
    HANDLE File;
    BOOL Ok;

    if (RegGetValueW(HKEY_LOCAL_MACHINE, LUH_LOCK_POLICY_KEY, L"LockScreenImage",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, Path, &Size) != ERROR_SUCCESS ||
        Path[0] == UNICODE_NULL)
    {
        Size = ExpandEnvironmentStringsW(LUH_LOCK_DEFAULT, Path, ARRAYSIZE(Path));
        if (Size == 0 || Size > ARRAYSIZE(Path))
            return INVALID_HANDLE_VALUE;
    }
    /* Local absolute paths only: no UNC share, no device path, no relative name. */
    if (!GetFullPathNameW(Path, ARRAYSIZE(Full), Full, NULL) || Full[1] != L':' || Full[2] != L'\\')
        return INVALID_HANDLE_VALUE;

    File = CreateFileW(Full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    /* One link only: a hard link elsewhere could carry a different, writable parent. */
    Ok = GetFileInformationByHandle(File, &Info) &&
         !(Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
         Info.nNumberOfLinks == 1;
    if (Ok)
    {
        Error = GetSecurityInfo(File, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                &Owner, NULL, &Dacl, NULL, &Sd);
        Ok = (Error == ERROR_SUCCESS) && LuhOnlyAdminsWrite(Owner, Dacl, LUH_FILE_WRITE);
        if (Error == ERROR_SUCCESS)
            LocalFree(Sd);
    }
    if (Ok)
        Ok = LuhParentsAdminOnly(Full);
    if (!Ok)
    {
        LuhTrace("backdrop-refused", Full);
        CloseHandle(File);
        return INVALID_HANDLE_VALUE;
    }
    return File;
}

static VOID
LuhFreeBackdrop(VOID)
{
    if (g_Ui.SoftBrush)
        DeleteObject(g_Ui.SoftBrush);
    g_Ui.SoftBrush = NULL;
    LuhSurfaceDestroy(&g_Ui.Back);
    LuhSurfaceDestroy(&g_Ui.Sharp);
    LuhSurfaceDestroy(&g_Ui.Still);
    LuhSurfaceDestroy(&g_Ui.Soft);
    LuhSurfaceDestroy(&g_Ui.Face);
}

/*
 * Built once per launch: the machine's lock-screen picture filled to the screen (the
 * curtain), and the same picture blurred and darkened (behind every other page).
 */
static BOOL
LuhBuildBackdrop(
    _In_ LONG Width,
    _In_ LONG Height)
{
    LUH_SURFACE Image, Small;
    RECT All = { 0, 0, Width, Height };
    HANDLE File;
    BOOL Loaded = FALSE;

    if (!LuhSurfaceCreate(&g_Ui.Back, Width, Height) || !LuhSurfaceCreate(&g_Ui.Sharp, Width, Height) ||
        !LuhSurfaceCreate(&g_Ui.Still, Width, Height) ||
        !LuhSurfaceCreate(&g_Ui.Soft, Width, Height) || !LuhSurfaceCreate(&g_Ui.Face, Width, Height) ||
        !LuhSurfaceCreate(&Small, max(Width / LUH_SOFT_SCALE, 1), max(Height / LUH_SOFT_SCALE, 1)))
    {
        LuhFreeBackdrop();
        return FALSE;
    }

    File = LuhOpenBackdrop();
    if (File != INVALID_HANDLE_VALUE)
    {
        /* Decoded at most at twice the screen: enough for a sharp downscale, never more. */
        Loaded = LuhSurfaceLoad(&Image, File, 2 * Width, 2 * Height);
        CloseHandle(File);
    }
    if (Loaded)
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
    if (g_Ui.SoftBrush == NULL)
    {
        LuhFreeBackdrop();
        return FALSE;
    }
    return TRUE;
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

/*
 * Lock: one white dot on black for LUH_FADE_HOLD_MS, then the curtain picture comes up from black
 * over LUH_FADE_IN_MS, one constant-alpha blend a frame (Windows 11 lock-cad frames). Keys and
 * clicks during these 650 ms are dropped on purpose, as on Windows 11: the curtain is not yet
 * there to lift.
 */
static void
LuhFadeInCurtain(
    _In_ HWND Window)
{
    LUH_ANIM Anim;
    double T, Ms;
    HDC Dc;

    if (!g_Ui.Curtain || !g_Ui.CurtainFadeIn)
        return;
    LuhAnimStart(&Anim, "curtain-fade-in", LUH_FADE_HOLD_MS + LUH_FADE_IN_MS);
    for (;;)
    {
        T = LuhAnimFrame(&Anim);
        Ms = T * (LUH_FADE_HOLD_MS + LUH_FADE_IN_MS);
        BitBlt(g_Ui.Back.Dc, 0, 0, g_Ui.Back.Width, g_Ui.Back.Height, NULL, 0, 0, BLACKNESS);
        if (Ms < LUH_FADE_HOLD_MS)
            LuhFillCircle(&g_Ui.Back, g_Ui.Back.Width / 2 - LuhPx(26), g_Ui.Back.Height / 2 - LuhPx(3),
                          max(LuhPx(3), 2), RGB(255, 255, 255), 255);
        else
            LuhBlend(&g_Ui.Back, 0, 0, &g_Ui.Face, (BYTE)(255.0 * LuhEaseOut((Ms - LUH_FADE_HOLD_MS) / LUH_FADE_IN_MS)));
        Dc = GetDC(Window);
        if (Dc)
        {
            BitBlt(Dc, 0, 0, g_Ui.Back.Width, g_Ui.Back.Height, g_Ui.Back.Dc, 0, 0, SRCCOPY);
            ReleaseDC(Window, Dc);
        }
        if (T >= 1.0)
            break;
        LuhAnimWait(&Anim);
    }
    LuhAnimEnd(&Anim);
    g_Ui.CurtainFadeIn = FALSE;
    InvalidateRect(Window, NULL, FALSE);
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
 * The run blocks the UI thread for at most LUH_LIFT_MS; commands winlogon posts meanwhile
 * (a SAS, a cancel) wait in the queue and are handled as soon as it returns.
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
    /* On a lock Windows 11 shows black first, then fades the lock screen in. */
    g_Ui.CurtainFadeIn = (g_Options.Scenario == CPUS_UNLOCK_WORKSTATION);
    g_Ui.FadeInPosted = FALSE;
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
            if (g_Ui.Page && !g_Ui.Curtain)
            {
                /* Windows 11 keeps the sign-in picture, the account and its name for Shutting down
                 * and Restarting; everything else on the page goes in one cut. */
                LuhHidePageChrome(Window);
                g_Ui.StatusOnPage = TRUE;
                LuhPageChanged(Window);
            }
            LuhSetSpinner(Window, (Command->Flags & WLUI_FLAG_SPINNER) != 0);
            LuhTrace("status", g_Ui.Title);
            InvalidateRect(Window, NULL, TRUE);
            break;

        case WluiCmdDisplayWelcome:
            if (g_Options.Scenario == CPUS_UNLOCK_WORKSTATION && g_Ui.Page && g_Ui.SignInBusy)
            {
                /* Unlock: no profile to load, so Welcome stays where the box was on the sign-in
                 * picture; winlogon captures this frame for its crossfade to the desktop. */
                LuhTrace("welcome", Command->User);
                break;
            }
            LuhDropCurtain(Window);
            LuhDestroyPage();
            StringCchCopyW(g_Ui.Title, ARRAYSIZE(g_Ui.Title), LuhText(IDS_LUH_WELCOME));
            g_Ui.Detail[0] = UNICODE_NULL;  /* Windows 11 shows Welcome alone */
            LuhSetSpinner(Window, TRUE);
            LuhTrace("welcome", Command->User);
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
            g_Ui.SignInBusy = FALSE;
            LuhTrace("result", g_Ui.Detail);
            if (g_Ui.Page)
            {
                LuhShowResult(Window, g_Ui.Detail);
            }
            else
            {
                LuhFocusForRetry();
                LuhPageChanged(Window);
            }
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

/* The flyout card, and over it the focused row's tooltip, right-aligned with the card. */
static void
LuhComposeFlyout(
    _Inout_ PLUH_SURFACE Surface)
{
    HWND Focus = GetFocus();
    PCWSTR Tip = NULL;
    SIZE Size;
    RECT Rect;

    LuhFillRound(Surface, &g_Ui.FlyoutCard, LuhPx(6), LUH_FLYOUT_FILL, 255);
    LuhFrameRound(Surface, &g_Ui.FlyoutCard, LuhPx(6), 1, RGB(255, 255, 255), 30);
    if (LuhIsFlyoutRow(Focus))
        Tip = LuhText(GetDlgCtrlID(Focus) == LUH_ID_RESTART ? IDS_LUH_TIP_RESTART :
                      GetDlgCtrlID(Focus) == LUH_ID_SLEEP ? IDS_LUH_TIP_SLEEP : IDS_LUH_TIP_SHUTDOWN);
    if (Tip == NULL || Tip[0] == UNICODE_NULL)
        return;
    Size = LuhMeasureText(g_Ui.SmallFont, Tip);
    SetRect(&Rect, g_Ui.FlyoutCard.right - Size.cx - LuhPx(16), g_Ui.FlyoutCard.top - LuhPx(10) - Size.cy - LuhPx(12),
            g_Ui.FlyoutCard.right, g_Ui.FlyoutCard.top - LuhPx(10));
    LuhFillRound(Surface, &Rect, LuhPx(4), RGB(43, 43, 43), 255);
    LuhFrameRound(Surface, &Rect, LuhPx(4), 1, RGB(255, 255, 255), 30);
    LuhDrawText(Surface, g_Ui.SmallFont, Tip, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 255, 255), 255);
}

/* The still picture of whatever is up, drawn only when it changes. */
static void
LuhComposeStill(
    _In_ const RECT *Client)
{
    PLUH_SURFACE Back = &g_Ui.Still;

    if (g_Ui.NoticeUp)
    {
        RECT Rect;

        LuhFillRect(Back, Client, RGB(0, 0, 0), 255);
        SetRect(&Rect, 0, Client->bottom / 2 + LuhPx(LUH_NOTICE_CAPTION_DY) - LuhPx(20), Client->right,
                Client->bottom / 2 + LuhPx(LUH_NOTICE_CAPTION_DY) + LuhPx(20));
        LuhDrawText(Back, g_Ui.StatusFont, g_Ui.NoticeCaption, &Rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                    RGB(255, 255, 255), 255);
        LuhDrawText(Back, g_Ui.TextFont, g_Ui.NoticeText, &g_Ui.NoticeTextRect, DT_CENTER | DT_TOP | DT_WORDBREAK,
                    RGB(255, 255, 255), 255);
        return;
    }
    if (g_Ui.OptionsUp)
    {
        /* Ctrl+Alt+Del: plain black under the list, as on Windows 11. */
        LuhFillRect(Back, Client, RGB(0, 0, 0), 255);
    }
    else
    {
        BitBlt(Back->Dc, 0, 0, Back->Width, Back->Height, g_Ui.Soft.Dc, 0, 0, SRCCOPY);
        if (g_Ui.Page)
            LuhComposeSignIn(Back, Client);
        else
            LuhComposeStatus(Back, Client);
    }
    if (g_Ui.FlyoutUp)
        LuhComposeFlyout(Back);
}

/*
 * A frame is the cached still picture plus what moves (the spinner), composed in Back and
 * blitted once; child controls paint themselves on top (WS_CLIPCHILDREN).
 */
static void
LuhCompose(
    _In_ const RECT *Client,
    _In_ const RECT *Paint,
    _In_ BOOL Rebuild)
{
    int i;

    if (Rebuild || !g_Ui.StillValid)
    {
        LuhComposeStill(Client);
        g_Ui.StillValid = TRUE;
    }

    /* Only the damaged part: a spinner tick repaints its own square and nothing else. */
    BitBlt(g_Ui.Back.Dc, Paint->left, Paint->top, Paint->right - Paint->left, Paint->bottom - Paint->top,
           g_Ui.Still.Dc, Paint->left, Paint->top, SRCCOPY);
    if (g_Ui.Spinner)
    {
        /* The phase follows the clock, so a late tick shows the right position, not a step behind. */
        double Cycle = fmod(LuhAnimElapsedMs(&g_Ui.SpinnerAnim), LUH_SPINNER_PERIOD) / LUH_SPINNER_PERIOD;

        LuhAnimFrame(&g_Ui.SpinnerAnim);
        /* Windows 11's ring of dots: each dot runs one eased lap a little behind the one before,
         * so they bunch at the top and spread out on the way round. */
        for (i = 0; i < LUH_SPINNER_DOTS; i++)
        {
            double U = Cycle - i * 0.07, A;

            U -= floor(U);
            A = (LuhEaseInOut(U) * 360.0 - 90.0) * 3.14159265358979 / 180.0;
            LuhFillCircle(&g_Ui.Back, Client->right / 2 + (LONG)(LuhPx(LUH_SPINNER_RING) * cos(A)),
                          g_Ui.SpinnerY + (LONG)(LuhPx(LUH_SPINNER_RING) * sin(A)), max(LuhPx(3), 2),
                          RGB(255, 255, 255), 255);
        }
    }
}

/* Owner-drawn controls copy the still picture under them: redraw them after it changed. */
static BOOL CALLBACK
LuhRedrawOwnerDrawn(
    _In_ HWND Child,
    _In_ LPARAM Unused)
{
    WCHAR Class[16];

    UNREFERENCED_PARAMETER(Unused);
    if (GetClassNameW(Child, Class, ARRAYSIZE(Class)) && !_wcsicmp(Class, L"Button") &&
        (GetWindowLongPtrW(Child, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
        InvalidateRect(Child, NULL, FALSE);
    return TRUE;
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
        if (g_Ui.CurtainFadeIn)
        {
            /* The first frame is black; the fade runs once that frame is out. */
            FillRect(Dc, &Ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (!g_Ui.FadeInPosted)
            {
                g_Ui.FadeInPosted = TRUE;
                PostMessageW(Window, WM_LUH_FADE_IN, 0, 0);
            }
        }
        else
        {
            /* The curtain is one cached picture; mid-drag a repaint shows the same position. */
            LuhBlitLift(Dc, g_Ui.Lift);
        }
    }
    else
    {
        /* A paint of the whole window is a page change (every such path invalidates it all);
         * a spinner tick never is, so it only blits. */
        BOOL Rebuild = EqualRect(&Ps.rcPaint, &Client);

        LuhCompose(&Client, &Ps.rcPaint, Rebuild);
        BitBlt(Dc, Ps.rcPaint.left, Ps.rcPaint.top, Ps.rcPaint.right - Ps.rcPaint.left,
               Ps.rcPaint.bottom - Ps.rcPaint.top, g_Ui.Back.Dc, Ps.rcPaint.left, Ps.rcPaint.top, SRCCOPY);
        if (Rebuild)
            EnumChildWindows(Window, LuhRedrawOwnerDrawn, 0);
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

        case WM_DRAWITEM:
            LuhDrawItem((const DRAWITEMSTRUCT *)lParam);
            return TRUE;

        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wParam, RGB(255, 255, 255));
            SetBkColor((HDC)wParam, g_Ui.BoxFill);
            return (LRESULT)(g_Ui.BoxBrush ? g_Ui.BoxBrush : GetStockObject(BLACK_BRUSH));

        case WM_CTLCOLORSTATIC:
            /* A disabled or read-only edit asks here: it keeps its box colour. */
            if (g_Ui.BoxBrush && LuhFieldOfControl((HWND)lParam) &&
                (LuhFieldOfControl((HWND)lParam)->Type == CPFT_EDIT_TEXT ||
                 LuhFieldOfControl((HWND)lParam)->Type == CPFT_PASSWORD_TEXT))
            {
                SetTextColor((HDC)wParam, RGB(200, 200, 205));
                SetBkColor((HDC)wParam, g_Ui.BoxFill);
                return (LRESULT)g_Ui.BoxBrush;
            }
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
                RECT Square;

                /* The dots' square only; the still page under it comes from the cache. */
                GetClientRect(Window, &Square);
                SetRect(&Square, Square.right / 2 - LuhPx(LUH_SPINNER_RING + 6), g_Ui.SpinnerY - LuhPx(LUH_SPINNER_RING + 6),
                        Square.right / 2 + LuhPx(LUH_SPINNER_RING + 6), g_Ui.SpinnerY + LuhPx(LUH_SPINNER_RING + 6));
                InvalidateRect(Window, &Square, FALSE);
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
            if ((HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) && LuhFieldOfControl((HWND)lParam))
            {
                /* The focused box carries the accent line: repaint that box only. */
                RECT Box = LuhFieldOfControl((HWND)lParam)->Box;

                InflateRect(&Box, LuhPx(2), LuhPx(2));
                g_Ui.StillValid = FALSE;
                InvalidateRect(Window, &Box, FALSE);
                return 0;
            }
            if (HIWORD(wParam) == EN_CHANGE && g_Ui.ArrowField != (DWORD)-1 && g_Ui.Page &&
                (HWND)lParam == g_Ui.Page->Fields[g_Ui.ArrowField].Control)
            {
                BOOL Has = GetWindowTextLengthW((HWND)lParam) > 0;

                if (Has != g_Ui.HasText)
                {
                    g_Ui.HasText = Has;
                    LuhPageChanged(Window);
                }
                return 0;
            }
            if ((HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS) && LuhIsFlyoutRow((HWND)lParam))
            {
                /* The tooltip follows the focus; focus leaving the rows closes the flyout. */
                if (HIWORD(wParam) == BN_KILLFOCUS)
                    PostMessageW(Window, WM_LUH_FLYOUT_FOCUS, 0, 0);
                LuhPageChanged(Window);
                return 0;
            }
            if (HIWORD(wParam) == BN_CLICKED)
            {
                UINT Id = LOWORD(wParam);
                if (Id == LUH_ID_NOTICE_OK || Id == LUH_ID_NOTICE_CANCEL)
                {
                    g_Ui.NoticeOk = (Id == LUH_ID_NOTICE_OK);
                    g_Ui.NoticeUp = FALSE;
                }
                else if (Id == LUH_ID_SLEEP)
                {
                    LuhCloseFlyout(Window, FALSE);
                    LuhTrace("power", L"sleep");
                    WluiClientRequestAction(&g_Connection, WluiActionSleep);
                }
                else if (Id == LUH_ID_RESTART || Id == LUH_ID_SHUTDOWN)
                {
                    LuhCloseFlyout(Window, FALSE);
                    LuhTrace("power", Id == LUH_ID_RESTART ? L"restart" : L"shutdown");
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
                else if (Id == LUH_ID_RESULT_OK)
                {
                    LuhDismissResult(Window);
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
                /* winlogon switches the screen to us only after this paint (INVESTIGACION.md 6.3) */
                UpdateWindow(Window);
                WluiClientCommandShown(&g_Connection, Command->Sequence);
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

        case WM_LUH_FLYOUT_FOCUS:
            if (g_Ui.FlyoutUp && !LuhIsFlyoutRow(GetFocus()) && GetFocus() != g_Ui.PowerButton)
                LuhCloseFlyout(Window, FALSE);
            return 0;

        case WM_LUH_FADE_IN:
            LuhFadeInCurtain(Window);
            return 0;

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
                if (!g_Ui.CadCurtain && !g_Ui.Dragging && !g_Ui.CurtainFadeIn)
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
                if (!g_Ui.CadCurtain && !g_Ui.CurtainFadeIn)
                    LuhDragBegin(Window, (SHORT)HIWORD(lParam));
                return 0;
            }
            if (g_Ui.FlyoutUp)
            {
                LuhCloseFlyout(Window, FALSE);
                return 0;
            }
            if (g_Ui.Page && !g_Ui.PageBusy)
            {
                POINT Point = { (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam) };

                if (PtInRect(&g_Ui.Arrow, Point))
                {
                    LuhReveal(Window, FALSE);
                    LuhSubmit(Window);
                    return 0;
                }
                if (g_Ui.HasText && PtInRect(&g_Ui.Eye, Point))
                {
                    LuhReveal(Window, TRUE);
                    return 0;
                }
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
            if (g_Ui.Revealing)
            {
                LuhReveal(Window, FALSE);
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
    g_Ui.TitleFont = LuhFontCreate(28, FW_SEMIBOLD);
    g_Ui.TextFont = LuhFontCreate(15, FW_NORMAL);
    g_Ui.NameFont = LuhFontCreate(26, FW_SEMIBOLD);
    g_Ui.StrongFont = LuhFontCreate(15, FW_SEMIBOLD);
    g_Ui.StatusFont = LuhFontCreate(22, FW_SEMIBOLD);
    g_Ui.SmallFont = LuhFontCreate(12, FW_NORMAL);
    g_Ui.NameField = (DWORD)-1;
    g_Ui.ArrowField = (DWORD)-1;
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
    g_Ui.Window = Window;
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
        if (Msg.message == WM_KEYDOWN && Msg.wParam == VK_ESCAPE && g_Ui.FlyoutUp)
        {
            LuhCloseFlyout(Window, TRUE);
            continue;
        }
        /* Enter or Esc on a result dismisses it, as its OK does. */
        if (Msg.message == WM_KEYDOWN && (Msg.wParam == VK_RETURN || Msg.wParam == VK_ESCAPE) && g_Ui.ResultUp)
        {
            LuhDismissResult(Window);
            continue;
        }
        if (Msg.message == WM_KEYDOWN && Msg.wParam == VK_RETURN && g_Ui.Page && !g_Ui.PageBusy && !g_Ui.SignInBusy)
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
    if (g_Ui.NameFont) DeleteObject(g_Ui.NameFont);
    if (g_Ui.StrongFont) DeleteObject(g_Ui.StrongFont);
    if (g_Ui.StatusFont) DeleteObject(g_Ui.StatusFont);
    if (g_Ui.SmallFont) DeleteObject(g_Ui.SmallFont);
    LuhFreeBackdrop();
    LuhGfxShutdown();
    return (int)Msg.wParam;
}
