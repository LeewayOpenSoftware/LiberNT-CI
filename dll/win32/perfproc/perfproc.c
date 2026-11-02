/*
 * PerfProc Provider - Process and Thread performance objects
 * Implements Windows-compatible perflib provider interface
 * Objects: Process (230), Thread (232)
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
extern NTSTATUS perf_get_process_count(ULONG *count);
extern NTSTATUS perf_get_thread_count(ULONG *count);

static DWORD g_dwOpenCount = 0;

DWORD APIENTRY OpenPerfProc(LPWSTR lpDeviceNames)
{
    g_dwOpenCount++;
    return ERROR_SUCCESS;
}

DWORD APIENTRY ClosePerfProc(void)
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

DWORD APIENTRY CollectPerfProcData(LPWSTR lpValueName, LPVOID *lppData, LPDWORD lpcbData, LPDWORD lpNumObjectTypes)
{
    PERF_DATA_BLOCK *pdb;
    PERF_OBJECT_TYPE *pobj;
    PERF_COUNTER_DEFINITION *pcnt;
    PERF_COUNTER_BLOCK *pcblk;
    ULONG process_count, thread_count;
    DWORD offset, obj_offset;
    BYTE *p;
    DWORD *pdwData;

    if (!lppData || !lpcbData || !lpNumObjectTypes)
        return ERROR_INVALID_PARAMETER;

    /* Require minimum buffer size */
    if (!*lppData || *lpcbData < 2048)
        return ERROR_MORE_DATA;

    /* Collect all kernel data first */
    if (!NT_SUCCESS(perf_get_process_count(&process_count)) ||
        !NT_SUCCESS(perf_get_thread_count(&thread_count)))
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
    pdb->NumObjectTypes = 2;
    pdb->DefaultObject = 230;  /* Process */

    NtQueryPerformanceCounter(&pdb->PerfTime, &pdb->PerfFreq);

    offset = sizeof(PERF_DATA_BLOCK);

    /* ===== PROCESS OBJECT (230) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 230;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 231;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 1;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = 1;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = pdb->PerfTime.QuadPart;
    pobj->PerfFreq.QuadPart = pdb->PerfFreq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* Process counter: Process Count (counter ID 248) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 248, PERF_COUNTER_RAWCOUNT, sizeof(DWORD), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = sizeof(PERF_COUNTER_DEFINITION);

    /* Process counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(DWORD);

    pdwData = (DWORD *)(pcblk + 1);
    *pdwData = process_count;

    offset += pcblk->ByteLength;
    pobj->TotalByteLength = offset - obj_offset;

    /* ===== THREAD OBJECT (232) ===== */
    obj_offset = offset;
    pobj = (PERF_OBJECT_TYPE *)(p + offset);

    pobj->TotalByteLength = 0;
    pobj->DefinitionLength = 0;
    pobj->HeaderLength = sizeof(PERF_OBJECT_TYPE);
    pobj->ObjectNameTitleIndex = 232;
    pobj->ObjectNameTitle = 0;
    pobj->ObjectHelpTitleIndex = 233;
    pobj->ObjectHelpTitle = 0;
    pobj->DetailLevel = PERF_DETAIL_NOVICE;
    pobj->NumCounters = 1;
    pobj->DefaultCounter = -1;
    pobj->NumInstances = 1;
    pobj->CodePage = 0;
    pobj->PerfTime.QuadPart = pdb->PerfTime.QuadPart;
    pobj->PerfFreq.QuadPart = pdb->PerfFreq.QuadPart;

    offset += sizeof(PERF_OBJECT_TYPE);

    /* Thread counter: Thread Count (counter ID 250) */
    pcnt = (PERF_COUNTER_DEFINITION *)(p + offset);
    init_counter_def(pcnt, 250, PERF_COUNTER_RAWCOUNT, sizeof(DWORD), sizeof(PERF_COUNTER_BLOCK));

    offset += sizeof(PERF_COUNTER_DEFINITION);
    pobj->DefinitionLength = sizeof(PERF_COUNTER_DEFINITION);

    /* Thread counter block with data */
    pcblk = (PERF_COUNTER_BLOCK *)(p + offset);
    pcblk->ByteLength = sizeof(PERF_COUNTER_BLOCK) + sizeof(DWORD);

    pdwData = (DWORD *)(pcblk + 1);
    *pdwData = thread_count;

    offset += pcblk->ByteLength;
    pobj->TotalByteLength = offset - obj_offset;

    /* Update final totals */
    pdb->TotalByteLength = offset;
    *lpNumObjectTypes = 2;
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
