/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/config/cmsecach.c
 * PURPOSE:         Configuration Manager - Security Cache
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES ******************************************************************/

#include "ntoskrnl.h"
#define NDEBUG
#include "debug.h"

/* GLOBALS *******************************************************************/

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
CmpInitSecurityCache(IN PCMHIVE Hive)
{
    ULONG i;

    /* Reset data */
    Hive->SecurityCount = 0;
    Hive->SecurityCacheSize = 0;
    Hive->SecurityHitHint = -1;
    Hive->SecurityCache = NULL;

    /* Loop every security hash */
    for (i = 0; i < CMP_SECURITY_HASH_LISTS; i++)
    {
        /* Initialize it */
        InitializeListHead(&Hive->SecurityHash[i]);
    }
}

VOID
NTAPI
CmpDestroySecurityCache(IN PCMHIVE Hive)
{
    /* FIXME: clean Hive->SecurityHash and/or Hive->SecurityCache */

    /* Reset data */
    Hive->SecurityCount = 0;
    Hive->SecurityCacheSize = 0;
    Hive->SecurityHitHint = -1;
    Hive->SecurityCache = NULL;
}

/* cmlib calls this when it frees a security cell: the cell must leave the
 * hive's descriptor cache before its space is reused. The array is sorted
 * by cell index; this kernel does not fill it yet, so the search is short. */
VOID
NTAPI
CmpRemoveFromSecurityCache(IN PHHIVE Hive,
                           IN HCELL_INDEX SecurityCell)
{
    PCMHIVE CmHive = CONTAINING_RECORD(Hive, CMHIVE, Hive);
    PCM_KEY_SECURITY_CACHE Cached;
    ULONG Low = 0, High, Middle, Move;

    if ((SecurityCell == HCELL_NIL) || (CmHive->SecurityCount == 0)) return;
    ASSERT(CmHive->SecurityCache != NULL);

    /* Binary search for the cell */
    High = CmHive->SecurityCount - 1;
    while (Low <= High)
    {
        Middle = Low + (High - Low) / 2;
        if (CmHive->SecurityCache[Middle].Cell == SecurityCell) break;
        if (CmHive->SecurityCache[Middle].Cell < SecurityCell)
        {
            Low = Middle + 1;
            continue;
        }
        if (Middle == 0) return;
        High = Middle - 1;
    }
    if (Low > High) return;

    /* Drop the cached descriptor and close the gap in the array. Whatever fills
     * the cache must allocate CM_KEY_SECURITY_CACHE with TAG_CM. */
    Cached = CmHive->SecurityCache[Middle].CachedSecurity;
    if (Cached)
    {
        RemoveEntryList(&Cached->List);
        ExFreePoolWithTag(Cached, TAG_CM);
    }
    Move = CmHive->SecurityCount - Middle - 1;
    if (Move)
    {
        RtlMoveMemory(&CmHive->SecurityCache[Middle],
                      &CmHive->SecurityCache[Middle + 1],
                      Move * sizeof(CM_KEY_SECURITY_CACHE_ENTRY));
    }
    CmHive->SecurityCount--;
    CmHive->SecurityHitHint = -1;
}

/* EOF */
