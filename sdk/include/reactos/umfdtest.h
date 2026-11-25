/*
 * PROJECT:     LiberNT win32k user-mode font driver channel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test-only contract between the UMFD channel tests and a fontdrvhost built with UMFD_TEST_HOOKS
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* A LOAD view that starts with this blob is a command, not a font (UMFD_TEST_HOOKS builds only) */
#define UMFD_TEST_MAGIC             "UMFDTEST"
#define UMFD_TEST_BLOB_SIZE         16384

#define UMFD_TEST_CRASH             1   /* fault inside the validation */
#define UMFD_TEST_STALL             2   /* sleep Param ms, read the whole view again, then reply */
#define UMFD_TEST_FREEZE            3   /* sleep Param ms without replying, then exit */
#define UMFD_TEST_EXIT              4   /* exit while the view is mapped and the request is open */
#define UMFD_TEST_RECYCLE           5   /* exit with the recycle code at once, as a hostile host could */

/* Host exit codes the tests check */
#define UMFD_TEST_EXIT_VIEW_GONE    0x55460001  /* a stalled request's view was unmapped under the host */
#define UMFD_TEST_EXIT_VIEW_LEAKED  0x55460002  /* a stalled request's view was still mapped at the next LOAD */
#define UMFD_TEST_EXIT_FROZEN       0x55460003
#define UMFD_TEST_EXIT_ON_REQUEST   0x55460004

typedef struct _UMFD_TEST_BLOB
{
    CHAR Magic[8];
    ULONG Command;
    ULONG Param;
} UMFD_TEST_BLOB;
