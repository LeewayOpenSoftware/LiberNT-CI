/*
 * PROJECT:     LiberNT logon image check library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Structural verification of a logon binary before winlogon
 *              launches it (no code-signing chain exists yet, so no signature
 *              claim is made).
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _WLIMG_STATUS
{
    WlImgOk = 0,
    WlImgBadPath,            /* not absolute, has "..", is a reparse point, or (with REQUIRE_SYSTEM32) not directly in System32 */
    WlImgOpenFailed,         /* CreateFile failed; GetLastError() has the reason */
    WlImgNotPe,              /* no MZ/PE headers, or headers truncated/implausible */
    WlImgWrongMachine,       /* PE machine or optional-header magic is not the native one */
    WlImgNotGuiExe,          /* not an executable image, is a DLL, or subsystem is not GUI */
    WlImgFileWritable,       /* the file DACL grants write-class access outside SYSTEM/Administrators/TrustedInstaller */
    WlImgDirectoryWritable,  /* same for the containing directory (with CHECK_DIRECTORY) */
    WlImgInternalError
} WLIMG_STATUS;

#define WLIMG_REQUIRE_SYSTEM32  0x00000001  /* path must be <SystemDirectory>\<name> */
#define WLIMG_CHECK_DIRECTORY   0x00000002  /* also verify the containing directory DACL */

/*
 * Opens ImagePath with FILE_SHARE_READ only (no write or delete sharing),
 * verifies it through that handle and returns the handle on success.
 * The caller keeps the handle open until CreateProcess has returned, then
 * CloseHandle()s it; that is what pins the verified bytes to the launch.
 * On any status other than WlImgOk, *ImageHandle is INVALID_HANDLE_VALUE.
 */
WLIMG_STATUS
NTAPI
WlImgOpenVerified(
    _In_ PCWSTR ImagePath,
    _In_ DWORD Flags,
    _Out_ PHANDLE ImageHandle);

/* Short, stable text for logs and event entries. */
PCWSTR
NTAPI
WlImgStatusName(
    _In_ WLIMG_STATUS Status);

#ifdef __cplusplus
}
#endif
