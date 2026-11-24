// sihost.exe - Shell Infrastructure Host
// Phase 0 (honest): exits with ERROR_NOT_SUPPORTED until CoreUI/CoreMessaging infrastructure exists.
// Not resident; not simulated success.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winerror.h>
#include <stdio.h>

// Phase 0: CoreUI/CoreMessaging infrastructure not present in Orosi.
// Until that layer exists, sihost cannot host shell COM classes.
// Honest failure: exit at startup with ERROR_NOT_SUPPORTED.

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
	LPWSTR lpCmdLine, int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;

	// Phase 0: CoreUI/CoreMessaging not implemented
	OutputDebugStringW(L"[sihost] Phase 0: CoreUI/CoreMessaging infrastructure not available; exiting ERROR_NOT_SUPPORTED\n");

	// Honest error: the required infrastructure does not exist yet
	ExitProcess(ERROR_NOT_SUPPORTED);

	return ERROR_NOT_SUPPORTED;  // unreachable; placed for completeness
}
