/*
 * PROJECT:     LiberNT SDK Headers
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Trace Data Helper definitions
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware <281492504+LeewayOpenSoftware@users.noreply.github.com>
 */

#ifndef _TDH_H_
#define _TDH_H_

#include <evntcons.h>

#ifdef __cplusplus
extern "C" {
#endif

TDHSTATUS
WINAPI
TdhGetEventInformation(
    _In_ PEVENT_RECORD Event,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT TdhContext,
    _Out_writes_bytes_opt_(*BufferSize) PTRACE_EVENT_INFO Buffer,
    _Inout_ PULONG BufferSize);

#ifdef __cplusplus
}
#endif

#endif /* _TDH_H_ */
