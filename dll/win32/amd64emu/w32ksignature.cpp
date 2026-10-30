/*
 * PROJECT:     LiberNT AMD64 emulation host
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Derive the argument widths of the native win32k services from their prototypes
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#define WIN32_NO_STATUS
#define CINTERFACE
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#define NTOS_MODE_USER
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <objbase.h>
#include <unknwn.h>
#include <ndk/ntndk.h>
#include <wingdi.h>
#include <winuser.h>
#include <imm.h>
#include <winddi.h>
#include <prntfont.h>
#include <ddrawi.h>
#include <ddrawgdi.h>
#include <d3dkmthk.h>
#include <ntgdityp.h>
#include <ntgdi.h>
#include <ntgdihdl.h>
#include <ntgdibad.h>
#include <immdev.h>
#include <imm32_undoc.h>
#include <ntusrtyp.h>
#include <ntuser.h>
#include "signature.h"

extern "C"
{

LONG NTAPI NtDCompositionCreateConnection(BOOL ConnectionFlag, HANDLE EventHandle, HANDLE *Identifier);
LONG NTAPI NtDCompositionDestroyConnection(HANDLE Identifier);
LONG NTAPI NtDCompositionCreateChannel(UINT *Identifier, UINT *SectionSize, PVOID *SectionBase);
LONG NTAPI NtDCompositionProcessChannelBatchBuffer(UINT Identifier, UINT ByteCount, UINT *CommandCount, BOOLEAN *State);
LONG NTAPI NtDCompositionCommitChannel(UINT Identifier, UINT *CommitId, BOOLEAN *State, ULONG Flags,
                                       HANDLE Synchronization, const VOID *Commands, const UINT *Resources,
                                       UINT ResourceCount);
LONG NTAPI NtDCompositionDestroyChannel(UINT Identifier);
DWORD APIENTRY NtDxEngGetRedirectionBitmap(DWORD Unknown0);
BOOL APIENTRY NtGdiGetRealizationInfo(HDC hdc, PREALIZATION_INFO pri, HFONT hf);

}

#define SVC_(Name, Count) EMU_SIGNATURE<Count, decltype(&Nt##Name), false>::Get(),

extern "C" const ULONG64 EmuWin32kSignatures[] =
{
#include <w32ksvc64.h>
};
