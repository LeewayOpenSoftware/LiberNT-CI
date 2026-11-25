/*
 * PROJECT:     LiberNT winlogon
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Recovery levels for the logon UI process (PHASE3-PLAN §8.9):
 *              which tier to launch next, with bounded budgets. Pure logic
 *              with an injected clock so it can be tested without winlogon.
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _WLREC_LEVEL
{
    WlRecLevel1 = 1,    /* LogonUIHost with every registered provider */
    WlRecLevel2 = 2,    /* LogonUIHost with the fallback flag: PasswordProvider only */
    WlRecLevel3 = 3,    /* LogonRecovery: the minimal supervised component */
    WlRecLevel4 = 4     /* terminal state drawn by winlogon: restart or shut down only */
} WLREC_LEVEL;

typedef enum _WLREC_ACTION
{
    WlRecActLaunchHost = 1,
    WlRecActLaunchHostFallback,
    WlRecActLaunchRecovery,
    WlRecActTerminal
} WLREC_ACTION;

/* How a launch ended, as the supervisor saw it. */
typedef enum _WLREC_END
{
    WlRecEndNormal = 0,         /* the UI finished its job (desktop switch, dismiss) */
    WlRecEndCrash,              /* process exit with a request outstanding, or a non-zero exit */
    WlRecEndHang,               /* heartbeats stopped or a call exceeded its bound */
    WlRecEndLaunchFailed,       /* CreateProcess failed */
    WlRecEndImageInvalid,       /* the binary is missing or failed the structural check */
    WlRecEndProvidersMissing    /* the host could not load the password tier (credprovs.dll) */
} WLREC_END;

#define WLREC_LEVEL_WINDOW_MS   (60 * 1000)
#define WLREC_LEVEL_FAULTS      2           /* faults within the window that end N2 or N3 */
#define WLREC_CAP_LAUNCHES      10
#define WLREC_CAP_WINDOW_MS     (10 * 60 * 1000)

typedef ULONGLONG (NTAPI *WLREC_CLOCK)(VOID);

typedef struct _WLREC_STATE
{
    WLREC_CLOCK Clock;
    WLREC_LEVEL Level;
    ULONG FaultCount;                               /* faults at the current level */
    ULONGLONG FaultTimes[WLREC_LEVEL_FAULTS];
    ULONGLONG LaunchTimes[WLREC_CAP_LAUNCHES];      /* ring of recent launch times */
    ULONG LaunchHead;
    ULONG LaunchCount;
    BOOL Capped;                                    /* the global cap was hit: N4 until reboot */
    ULONG TotalLaunches;
} WLREC_STATE, *PWLREC_STATE;

VOID NTAPI WlRecInit(_Out_ PWLREC_STATE State, _In_opt_ WLREC_CLOCK Clock);

/* Called on every SAS: a fresh attempt starts at N1 unless the cap was hit. */
VOID NTAPI WlRecOnSas(_Inout_ PWLREC_STATE State);

/*
 * What to launch now. A launch action is counted against the global cap; when
 * the cap is reached the state drops to N4 and stays there, so the answer is
 * WlRecActTerminal, never a blank desktop. A normal end clears the count, so
 * the cap sees only consecutive launches that never did their job.
 */
WLREC_ACTION NTAPI WlRecNextAction(_Inout_ PWLREC_STATE State);

/* How the last launch ended; moves the level according to the budgets. */
VOID NTAPI WlRecOnEnd(_Inout_ PWLREC_STATE State, _In_ WLREC_END End);

WLREC_LEVEL NTAPI WlRecLevel(_In_ const WLREC_STATE *State);
PCWSTR NTAPI WlRecLevelName(_In_ WLREC_LEVEL Level);
PCWSTR NTAPI WlRecEndName(_In_ WLREC_END End);

#ifdef __cplusplus
}
#endif
