#define _CRT_NON_CONFORMING_WCSTOK 1
#define COBJMACROS
/* Force-included for the Win11 standalone build of shimgvw. */
#pragma once
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#ifndef __cplusplus
#include <strsafe.h>
#endif
#ifndef __analysis_noreturn
#define __analysis_noreturn
#endif
#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))
#endif
