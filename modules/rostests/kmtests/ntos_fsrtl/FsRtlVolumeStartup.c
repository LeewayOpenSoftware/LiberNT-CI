/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     FsRtlAreVolumeStartupApplicationsComplete reflects the smss
 *              BootExecute phase
 */

#include <kmt_test.h>

/* kmtest runs from a booted session: smss has executed the BootExecute list
 * and called NtInitializeRegistry, so the answer must be TRUE here. The
 * FALSE-before-smss half is boot-time state that a loaded test cannot observe;
 * it is covered by the kernel setting the flag in exactly one place
 * (NtInitializeRegistry, smss phase), never at initialization. */
START_TEST(FsRtlVolumeStartup)
{
    ok_bool_true(FsRtlAreVolumeStartupApplicationsComplete(),
                 "FsRtlAreVolumeStartupApplicationsComplete after boot");
    /* Stable: repeated queries agree */
    ok_bool_true(FsRtlAreVolumeStartupApplicationsComplete(),
                 "FsRtlAreVolumeStartupApplicationsComplete, second query");
}
