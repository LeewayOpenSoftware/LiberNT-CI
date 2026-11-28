/* Kernelbase-owned environment aliases and performance counters. */

#include <windows.h>
BOOL WINAPI FreeEnvironmentStringsA(LPSTR Environment)
{
    return FreeEnvironmentStringsW((LPWSTR)Environment);
}
