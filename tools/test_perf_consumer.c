/* Test Performance Counter Consumer - Milestone B independent consumer */
#include <windows.h>
#include <winperf.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    HKEY hKey;
    DWORD cbData;
    BYTE buffer[8192];
    PERF_DATA_BLOCK *pdb;
    PERF_OBJECT_TYPE *pobj;
    int loop, status;

    printf("=== Test Performance Counter Consumer ===\n");
    printf("Reading HKEY_PERFORMANCE_DATA (2 4 238 230)\n\n");

    for (loop = 0; loop < 10; loop++)
    {
        status = RegOpenKeyExW(HKEY_PERFORMANCE_DATA, L"2 4 238 230", 0, KEY_READ, &hKey);
        if (status != ERROR_SUCCESS)
        {
            printf("LOOP %d: RegOpenKeyEx failed (0x%x)\n", loop, status);
            Sleep(1000);
            continue;
        }

        cbData = sizeof(buffer);
        status = RegQueryValueExW(hKey, L"Global", NULL, NULL, buffer, &cbData);
        RegCloseKey(hKey);

        if (status != ERROR_SUCCESS)
        {
            printf("LOOP %d: RegQueryValueEx failed (0x%x)\n", loop, status);
            Sleep(1000);
            continue;
        }

        if (cbData < sizeof(PERF_DATA_BLOCK))
        {
            printf("LOOP %d: Buffer too small\n", loop);
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
