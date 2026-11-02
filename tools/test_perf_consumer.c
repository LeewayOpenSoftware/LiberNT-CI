/* Test Performance Counter Consumer - Milestone B independent consumer
   Includes independent reference values from kernel APIs for validation */
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windows.h>
#include <winperf.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

typedef NTSTATUS (WINAPI *pNtQuerySystemInformation)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

int main(void)
{
    DWORD cbData;
    BYTE *buffer = NULL;
    DWORD bufSize = 8192;
    PERF_DATA_BLOCK *pdb;
    PERF_OBJECT_TYPE *pobj;
    int loop, status;
    MEMORYSTATUSEX memStat;
    HANDLE hSnapshot;
    PROCESSENTRY32 procEntry;
    THREADENTRY32 threadEntry;
    DWORD procCount = 0, threadCount = 0;
    ULONGLONG uptime;
    pNtQuerySystemInformation NtQuerySystemInformation;
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION procPerf[8];
    ULONG retLen;

    buffer = malloc(bufSize);
    if (!buffer)
        return 1;

    printf("=== Test Performance Counter Consumer + Reference Values ===\n");
    printf("Reading HKEY_PERFORMANCE_DATA (2 4 238 230) with kernel reference data\n\n");

    NtQuerySystemInformation = (pNtQuerySystemInformation)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");

    for (loop = 0; loop < 10; loop++)
    {
        /* Collect reference values */
        memStat.dwLength = sizeof(memStat);
        GlobalMemoryStatusEx(&memStat);
        DWORD memAvailMB = (DWORD)(memStat.ullAvailPhys / (1024 * 1024));

        uptime = GetTickCount64() / 1000;

        procCount = 0;
        threadCount = 0;
        hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
        if (hSnapshot != INVALID_HANDLE_VALUE)
        {
            procEntry.dwSize = sizeof(procEntry);
            if (Process32First(hSnapshot, &procEntry))
            {
                do {
                    procCount++;
                    threadEntry.dwSize = sizeof(threadEntry);
                    if (Thread32First(hSnapshot, &threadEntry))
                    {
                        do {
                            if (threadEntry.th32OwnerProcessID == procEntry.th32ProcessID)
                                threadCount++;
                        } while (Thread32Next(hSnapshot, &threadEntry));
                    }
                } while (Process32Next(hSnapshot, &procEntry));
            }
            CloseHandle(hSnapshot);
        }

        /* Query processor time via NtQuerySystemInformation */
        ULONGLONG procTimeKernel = 0, procTimeUser = 0, procTimeIdle = 0;
        if (NtQuerySystemInformation)
        {
            retLen = 0;
            if (NT_SUCCESS(NtQuerySystemInformation(SystemProcessorPerformanceInformation, procPerf, sizeof(procPerf), &retLen)))
            {
                if (retLen >= sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION))
                {
                    procTimeKernel = procPerf[0].KernelTime.QuadPart;
                    procTimeUser = procPerf[0].UserTime.QuadPart;
                    procTimeIdle = procPerf[0].IdleTime.QuadPart;
                }
            }
        }

        /* Query performance data from HKEY_PERFORMANCE_DATA */
        cbData = bufSize;
        status = RegQueryValueExW(HKEY_PERFORMANCE_DATA, L"2 4 238 230", NULL, NULL, buffer, &cbData);

        /* The perflib reports ERROR_MORE_DATA when the buffer is too small: grow it and ask again */
        while (status == ERROR_MORE_DATA && bufSize < 4 * 1024 * 1024)
        {
            BYTE *bigger = realloc(buffer, bufSize * 2);

            if (!bigger)
                break;
            buffer = bigger;
            bufSize *= 2;
            cbData = bufSize;
            status = RegQueryValueExW(HKEY_PERFORMANCE_DATA, L"2 4 238 230", NULL, NULL, buffer, &cbData);
        }

        if (status == ERROR_MORE_DATA)
        {
            printf("LOOP %d: Buffer too small for query (need %lu bytes)\n", loop, cbData);
            printf("  REF: MemAvail=%lu MB, ProcCount=%lu, ThreadCount=%lu, Uptime=%llu sec\n",
                   memAvailMB, procCount, threadCount, uptime);
            Sleep(1000);
            continue;
        }

        if (status != ERROR_SUCCESS)
        {
            printf("LOOP %d: RegQueryValueEx failed (0x%x)\n", loop, status);
            printf("  REF: MemAvail=%lu MB, ProcCount=%lu, ThreadCount=%lu, Uptime=%llu sec\n",
                   memAvailMB, procCount, threadCount, uptime);
            Sleep(1000);
            continue;
        }

        if (cbData < sizeof(PERF_DATA_BLOCK))
        {
            printf("LOOP %d: Returned data too small (%lu bytes)\n", loop, cbData);
            Sleep(1000);
            continue;
        }

        pdb = (PERF_DATA_BLOCK *)buffer;
        if (pdb->Signature[0] != 'P' || pdb->Signature[1] != 'E' ||
            pdb->Signature[2] != 'R' || pdb->Signature[3] != 'F')
        {
            printf("LOOP %d: Invalid signature\n", loop);
            Sleep(1000);
            continue;
        }

        printf("LOOP %d: PERF_DATA_BLOCK (v%lu.%lu, %lu objects, %lu bytes)\n",
               loop, pdb->Version, pdb->Revision, pdb->NumObjectTypes, pdb->TotalByteLength);
        printf("  REF: MemAvail=%lu MB, ProcCount=%lu, ThreadCount=%lu, Uptime=%llu sec\n",
               memAvailMB, procCount, threadCount, uptime);
        printf("  CPU: Kernel=%llu, User=%llu, Idle=%llu (100ns units)\n",
               procTimeKernel, procTimeUser, procTimeIdle);

        pobj = (PERF_OBJECT_TYPE *)((BYTE *)pdb + pdb->HeaderLength);
        for (int i = 0; i < pdb->NumObjectTypes && (BYTE *)pobj < buffer + cbData; i++)
        {
            printf("  Obj %d: ID=%lu, ctrs=%ld, inst=%ld\n",
                   i, pobj->ObjectNameTitleIndex, pobj->NumCounters, pobj->NumInstances);
            pobj = (PERF_OBJECT_TYPE *)((BYTE *)pobj + pobj->TotalByteLength);
        }
        printf("\n");

        if (loop < 9)
            Sleep(1000);
    }

    return 0;
}
