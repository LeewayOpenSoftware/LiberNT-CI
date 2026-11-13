/* Win11 standalone harness: runs the real shimgvw ImageView_Main as an exe. */
#include <windows.h>
#include <shellapi.h>
extern HINSTANCE g_hInstance;
extern LONG ImageView_Main(HWND hwnd, LPCWSTR path);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g_hInstance = inst;
    ImageView_Main(NULL, (argv && argc > 1) ? argv[1] : L"");
    LocalFree(argv);
    return 0;
}
