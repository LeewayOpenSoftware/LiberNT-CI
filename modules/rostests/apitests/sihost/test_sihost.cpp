// test_sihost.cpp - API test for sihost.exe
// Verifies that sihost.exe exists and exits with ERROR_NOT_SUPPORTED (Phase 0 honest failure).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winerror.h>
#include <stdio.h>

START_TEST(sihost_phase0)
{
	WCHAR sihostPath[MAX_PATH];
	DWORD pathLen;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD exitCode;
	BOOL bSuccess;

	// Get system32 directory path
	pathLen = GetSystemDirectory(sihostPath, MAX_PATH - 1);
	ok(pathLen > 0 && pathLen < MAX_PATH - 1, "GetSystemDirectory failed\n");
	if (pathLen == 0 || pathLen >= MAX_PATH - 1) {
		return;  // Can't find system32
	}

	// Append sihost.exe
	wcscat_s(sihostPath, MAX_PATH, L"\\sihost.exe");

	// Check if sihost.exe exists
	HANDLE hFile = CreateFileW(sihostPath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile == INVALID_HANDLE_VALUE) {
		skip("sihost.exe not installed\n");
		return;
	}

	CloseHandle(hFile);

	// Spawn sihost.exe
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	bSuccess = CreateProcessW(sihostPath, NULL, NULL, NULL, FALSE,
		0, NULL, NULL, &si, &pi);

	ok(bSuccess, "CreateProcessW failed: 0x%lx\n", GetLastError());
	if (!bSuccess) {
		return;
	}

	// Wait for sihost to exit (Phase 0: should exit immediately)
	WaitForSingleObject(pi.hProcess, INFINITE);

	// Verify exit code is ERROR_NOT_SUPPORTED (Phase 0 honest failure)
	GetExitCodeProcess(pi.hProcess, &exitCode);
	ok(exitCode == ERROR_NOT_SUPPORTED,
	   "Expected exit code ERROR_NOT_SUPPORTED (1), got 0x%lx\n", exitCode);

	// Cleanup
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}
