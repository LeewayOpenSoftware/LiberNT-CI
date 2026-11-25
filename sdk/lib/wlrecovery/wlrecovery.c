/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Recovery levels for the logon UI process (see wlrecovery.h).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windef.h>
#include <winbase.h>

#include "wlrecovery.h"

static ULONGLONG NTAPI
WlRecDefaultClock(VOID)
{
    return GetTickCount64();
}

VOID
NTAPI
WlRecInit(
    _Out_ PWLREC_STATE State,
    _In_opt_ WLREC_CLOCK Clock)
{
    ZeroMemory(State, sizeof(*State));
    State->Clock = Clock ? Clock : WlRecDefaultClock;
    State->Level = WlRecLevel1;
    WlRecBudgetInit(&State->Budget, WLREC_CAP_LAUNCHES, WLREC_CAP_WINDOW_MS);
}

/* ---- launch budget (shared with other supervised launchers) --------------------- */

VOID
NTAPI
WlRecBudgetInit(
    _Out_ PWLREC_BUDGET Budget,
    _In_ ULONG MaxLaunches,
    _In_ ULONGLONG WindowMs)
{
    ZeroMemory(Budget, sizeof(*Budget));
    Budget->MaxLaunches = MaxLaunches > WLREC_BUDGET_RING ? WLREC_BUDGET_RING : MaxLaunches;
    Budget->WindowMs = WindowMs;
}

BOOL
NTAPI
WlRecBudgetAdmit(
    _Inout_ PWLREC_BUDGET Budget,
    _In_ ULONGLONG Now)
{
    ULONG i, Recent = 0;

    if (Budget->MaxLaunches == 0)
        return TRUE;
    Budget->Times[Budget->Head] = Now;
    Budget->Head = (Budget->Head + 1) % Budget->MaxLaunches;
    if (Budget->Count < Budget->MaxLaunches)
        Budget->Count++;

    for (i = 0; i < Budget->Count; i++)
    {
        if (Now - Budget->Times[i] <= Budget->WindowMs)
            Recent++;
    }
    return Recent < Budget->MaxLaunches;
}

VOID
NTAPI
WlRecBudgetReset(
    _Inout_ PWLREC_BUDGET Budget)
{
    Budget->Count = 0;
    Budget->Head = 0;
}

static VOID
WlRecEnter(
    _Inout_ PWLREC_STATE State,
    _In_ WLREC_LEVEL Level)
{
    State->Level = Level;
    State->FaultCount = 0;
    ZeroMemory(State->FaultTimes, sizeof(State->FaultTimes));
}

VOID
NTAPI
WlRecOnSas(
    _Inout_ PWLREC_STATE State)
{
    if (State->Capped)
        return;
    WlRecEnter(State, WlRecLevel1);
}

/* TRUE when WLREC_CAP_LAUNCHES launches would fall inside the cap window. */
static BOOL
WlRecCountLaunch(
    _Inout_ PWLREC_STATE State)
{
    State->TotalLaunches++;
    return !WlRecBudgetAdmit(&State->Budget, State->Clock());
}

WLREC_ACTION
NTAPI
WlRecNextAction(
    _Inout_ PWLREC_STATE State)
{
    if (State->Capped || State->Level == WlRecLevel4)
        return WlRecActTerminal;

    if (WlRecCountLaunch(State))
    {
        /* The cap is a hard stop until reboot; the terminal dialog still offers restart/shutdown. */
        State->Capped = TRUE;
        WlRecEnter(State, WlRecLevel4);
        return WlRecActTerminal;
    }

    switch (State->Level)
    {
        case WlRecLevel1: return WlRecActLaunchHost;
        case WlRecLevel2: return WlRecActLaunchHostFallback;
        case WlRecLevel3: return WlRecActLaunchRecovery;
        default:          return WlRecActTerminal;
    }
}

/* Records a fault at the current level; TRUE when the level's budget is spent. */
static BOOL
WlRecRecordFault(
    _Inout_ PWLREC_STATE State)
{
    ULONGLONG Now = State->Clock();
    ULONG i, Recent = 0;

    if (State->Level == WlRecLevel1)
        return TRUE;        /* one fault is all N1 gets */

    State->FaultTimes[State->FaultCount % WLREC_LEVEL_FAULTS] = Now;
    if (State->FaultCount < WLREC_LEVEL_FAULTS)
        State->FaultCount++;
    for (i = 0; i < State->FaultCount; i++)
    {
        if (Now - State->FaultTimes[i] <= WLREC_LEVEL_WINDOW_MS)
            Recent++;
    }
    return Recent >= WLREC_LEVEL_FAULTS;
}

VOID
NTAPI
WlRecOnEnd(
    _Inout_ PWLREC_STATE State,
    _In_ WLREC_END End)
{
    BOOL Spent;

    if (State->Capped || State->Level == WlRecLevel4)
        return;

    if (End == WlRecEndNormal)
    {
        /* The cap measures a crash loop, not use: a launch that did its job (lock, logon,
         * status page, dismiss) clears the history, so only launches with no normal end
         * between them count towards it. */
        WlRecBudgetReset(&State->Budget);
        return;
    }

    switch (End)
    {
        case WlRecEndCrash:
        case WlRecEndHang:
            Spent = WlRecRecordFault(State);
            break;
        case WlRecEndLaunchFailed:
        case WlRecEndImageInvalid:
        case WlRecEndProvidersMissing:
            /* Nothing to retry at this tier: the binary or its password tier is not there. */
            Spent = TRUE;
            break;
        default:
            return;
    }

    if (!Spent)
        return;

    switch (State->Level)
    {
        case WlRecLevel1: WlRecEnter(State, WlRecLevel2); break;
        case WlRecLevel2: WlRecEnter(State, WlRecLevel3); break;
        case WlRecLevel3: WlRecEnter(State, WlRecLevel4); break;
        default: break;
    }
}

WLREC_LEVEL
NTAPI
WlRecLevel(
    _In_ const WLREC_STATE *State)
{
    return State->Level;
}

PCWSTR
NTAPI
WlRecLevelName(
    _In_ WLREC_LEVEL Level)
{
    switch (Level)
    {
        case WlRecLevel1: return L"N1";
        case WlRecLevel2: return L"N2";
        case WlRecLevel3: return L"N3";
        case WlRecLevel4: return L"N4";
        default:          return L"?";
    }
}

PCWSTR
NTAPI
WlRecEndName(
    _In_ WLREC_END End)
{
    switch (End)
    {
        case WlRecEndNormal:           return L"normal";
        case WlRecEndCrash:            return L"crash";
        case WlRecEndHang:             return L"hang";
        case WlRecEndLaunchFailed:     return L"launch-failed";
        case WlRecEndImageInvalid:     return L"image-invalid";
        case WlRecEndProvidersMissing: return L"providers-missing";
        default:                       return L"?";
    }
}
