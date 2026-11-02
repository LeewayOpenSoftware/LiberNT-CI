/*
 * PerfOS Provider - System, Memory, Processor performance objects
 * Implements Windows-compatible perflib provider interface
 * Objects: System (2), Memory (4), Processor (238)
 * Uses shared measurement helpers from perfdata.c for real kernel data
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <stdlib.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include <winperf.h>
#include "winternl.h"

#define NDEBUG
#include <debug.h>

/* Forward declarations - shared measurement helpers from perfdata.c */
extern NTSTATUS perf_get_processor_time(ULONGLONG *kernel_time, ULONGLONG *user_time, ULONGLONG *idle_time);
extern NTSTATUS perf_get_available_memory(ULONGLONG *available_pages);
extern NTSTATUS perf_get_committed_memory(ULONGLONG *committed_pages);
extern NTSTATUS perf_get_total_memory(ULONGLONG *total_pages);

static DWORD g_dwOpenCount = 0;

DWORD APIENTRY OpenPerfOS(LPWSTR lpDeviceNames)
{
    g_dwOpenCount++;
    return ERROR_SUCCESS;
}

DWORD APIENTRY ClosePerfOS(void)
{
    if (g_dwOpenCount > 0)
        g_dwOpenCount--;
    return ERROR_SUCCESS;
}

/* Helper: Initialize a counter definition structure */
static void init_counter_def(
    PERF_COUNTER_DEFINITION *def,
    DWORD counter_id,
    DWORD counter_type,
    DWORD counter_size,
    DWORD counter_offset)
{
    def->ByteLength = sizeof(PERF_COUNTER_DEFINITION);
    def->CounterNameTitleIndex = counter_id;
    def->CounterNameTitle = 0;
    def->CounterHelpTitleIndex = counter_id + 1;
    def->CounterHelpTitle = 0;
    def->DefaultScale = 0;
    def->DetailLevel = PERF_DETAIL_NOVICE;
    def->CounterType = counter_type;
    def->CounterSize = counter_size;
    def->CounterOffset = counter_offset;
}

DWORD APIENTRY CollectPerfOSData(LPWSTR lpValueName, LPVOID *lppData, LPDWORD lpcbData, LPDWORD lpNumObjectTypes)
{
    PERF_DATA_BLOCK *pdb;
    PERF_OBJECT_TYPE *pobj;
    PERF_COUNTER_DEFINITION *pcnt;
    PERF_COUNTER_BLOCK *pcblk;
    ULONGLONG kernel_time, user_time, idle_time;
    ULONGLONG avail_mem, committed_mem, total_mem;
    DWORD offset, obj_offset;
    BYTE *p;
    ULONGLONG *pullData;
    DWORD *pdwData;

    if (!lppData || !lpcbData || !lpNumObjectTypes)
        return ERROR_INVALID_PARAMETER;

    /* Require minimum buffer size */
    if (!*lppData || *lpcbData < 4096)
        return ERROR_MORE_DATA;

    /* Collect all kernel data first */
    if (!NT_SUCCESS(perf_get_processor_time(&kernel_time, &user_time, &idle_time)) ||
        !NT_SUCCESS(perf_get_available_memory(&avail_mem)) ||
        !NT_SUCCESS(perf_get_committed_memory(&committed_mem)) ||
        !NT_SUCCESS(perf_get_total_memory(&total_mem)))
        return ERROR_NOT_READY;

    p = (BYTE *)*lppData;
    pdb = (PERF_DATA_BLOCK *)p;

    /* Initialize PERF_DATA_BLOCK header */
    pdb->Signature[0] = 'P';
    pdb->Signature[1] = 'E';
    pdb->Signature[2] = 'R';
    pdb->Signature[3] = 'F';
#ifdef WORDS_BIGENDIAN
    pdb->LittleEndian = FALSE;
#else
    pdb->LittleEndian = TRUE;
#endif
    pdb->Version = PERF_DATA_VERSION;
    pdb->Revision = PERF_DATA_REVISION;
    pdb->TotalByteLength = 0;  /* Update at end */
    pdb->HeaderLength = sizeof(PERF_DATA_BLOCK);
    pdb->NumObjectTypes = 3;
    pdb->DefaultObject = 238;  /* Processor */

    NtQueryPerformanceCounter(&pdb->PerfTime, &pdb->PerfFreq);

    offset = sizeof(PERF_DATA_BLOCK);

    /* ===== SYSTEM OBJECT (2) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 2;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 3;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 1;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = 1;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = pdb->PerfTime.QuadPart;
    pobj->PerfFreq.QuadPart = pdb->PerfFreq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* System counter: System Up Time (counter ID 674) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 674, PERF_100NSEC_TIMER, sizeof(ULONGLONG), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = sizeof(PERF_COUNTER_DEFINITION);

    /* System counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(ULONGLONG);

    pullData = (ULONGLONG *)(pcblk + 1);
    *pullData = pdb->PerfTime.QuadPart;  /* System up time in performance counter units */

    offset += pcblk->ByteLength;
    pobj->TotalByteLength = offset - obj_offset;

    /* ===== MEMORY OBJECT (4) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 4;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 5;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 2;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = 1;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = pdb->PerfTime.QuadPart;
    pobj->PerfFreq.QuadPart = pdb->PerfFreq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* Counter 1: Available MBytes (counter ID 6490) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 6490, PERF_COUNTER_RAWCOUNT, sizeof(DWORD), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);

    /* Counter 2: Committed Bytes (counter ID 6492) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 6492, PERF_COUNTER_LARGE_RAWCOUNT, sizeof(ULONGLONG),
                     sizeof(PERF_COUNTER_BLOCK) + sizeof(DWORD));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = 2 * sizeof(PERF_COUNTER_DEFINITION);

    /* Memory counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(DWORD) + sizeof(ULONGLONG);

    pdwData = (DWORD *)(pcblk + 1);
    *pdwData = (DWORD)(avail_mem * 4096 / (1024 * 1024));  /* Convert pages to MBytes */

    pullData = (ULONGLONG *)(pdwData + 1);
    *pullData = committed_mem * 4096;  /* Convert pages to bytes */

    offset += pcblk->ByteLength;
    pobj->TotalByteLength = offset - obj_offset;

    /* ===== PROCESSOR OBJECT (238) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 238;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 239;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 1;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = 1;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = pdb->PerfTime.QuadPart;
    pobj->PerfFreq.QuadPart = pdb->PerfFreq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* Counter: % Processor Time (counter ID 6) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 6, PERF_100NSEC_TIMER_INV, sizeof(ULONGLONG), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = sizeof(PERF_COUNTER_DEFINITION);

    /* Processor counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(ULONGLONG);

    pullData = (ULONGLONG *)(pcblk + 1);
    /* For PERF_100NSEC_TIMER_INV: store (KernelTime + UserTime) */
    *pullData = kernel_time + user_time;

    offset += pcblk->ByteLength;
    pobj->TotalByteLength = offset - obj_offset;

    /* Update final totals */
    pdb->TotalByteLength = offset;
    *lpNumObjectTypes = 3;
    *lpcbData = offset;

    return ERROR_SUCCESS;
}

/* DLL Entry Point */
BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
    (void)hInstance;
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_PROCESS_DETACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}
