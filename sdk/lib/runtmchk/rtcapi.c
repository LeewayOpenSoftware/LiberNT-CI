/*
 * PROJECT:         MSVC runtime check support library
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * PURPOSE:         Provides support functions for MSVC runtime checks
 * PROGRAMMER:      Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#include <rtcapi.h>
#include <stdarg.h>

#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:__CRT_RTC_INITW=__CRT_RTC_INITW0")
#elif defined(_M_IA64) || defined(_M_AMD64) || defined(_M_ARM) || defined(_M_ARM64)
#pragma comment(linker, "/alternatename:_CRT_RTC_INITW=_CRT_RTC_INITW0")
#else
#error Unsupported platform
#endif

// Provide a fallback memset for libraries like kbdrost.dll
#if defined(_M_ARM) || defined(_M_ARM64)
void* __cdecl memset_fallback(void* src, int val, size_t count)
{
    char *char_src = (char *)src;
    while(count > 0)
    {
        *char_src = val;
        char_src++;
        count--;
    }
    return src;
}
#pragma comment(linker, "/alternatename:memset=memset_fallback")
#pragma comment(linker, "/alternatename:__RTC_memset=memset_fallback")
#endif

/* DbgPrint is provided by ntdll in user mode, the kernel, and FreeLdr, but a
 * few ultra-minimal modules link runtmchk without importing it (keyboard
 * layout DLLs import nothing). Provide a no-op fallback via /alternatename,
 * the same mechanism used for memset above, so those modules stay linkable
 * while everything else gets the real DbgPrint. Declared here to avoid
 * pulling a debug header into this low-level support file. */
unsigned long __cdecl DbgPrint(const char *Format, ...);

unsigned long __cdecl DbgPrint_fallback(const char *Format, ...)
{
    (void)Format;
    return 0;
}
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:_DbgPrint=_DbgPrint_fallback")
#else
#pragma comment(linker, "/alternatename:DbgPrint=DbgPrint_fallback")
#endif

int
__cdecl
_RTC_DefaultErrorFuncW(
    int errType,
    const wchar_t *file,
    int line,
    const wchar_t *module,
    const wchar_t *format,
    ...)
{
    static const char *const _RtcErrNames[] =
    {
        "stack check", "value truncation", "stack corruption",
        "uninitialized local use", "corrupted alloca", "illegal"
    };
    const char *name = ((errType >= 0) && (errType <= _RTC_ILLEGAL)) ?
                       _RtcErrNames[errType] : "unknown";

    /* Report the failure before breaking, so a /RTC trip is self-describing
     * in the debug log instead of a bare int3 (STATUS_BREAKPOINT). For a use
     * of an uninitialized local, the caller's wide format carries the variable
     * name as a narrow string (%S), so pull it and name it. */
    if (errType == _RTC_UNINIT_LOCAL_USE)
    {
        va_list args;
        const char *var;
        va_start(args, format);
        var = va_arg(args, const char *);
        va_end(args);
        DbgPrint("RTC failure %d (%s): uninitialized local '%s' used\n",
                 errType, name, var ? var : "?");
    }
    else
    {
        DbgPrint("RTC failure %d (%s)\n", errType, name);
    }

    __debugbreak();
    return 0;
}

_RTC_error_fnW _RTC_pErrorFuncW = _RTC_DefaultErrorFuncW;

/*
    Default CRT RTC init, if we don't link to CRT
*/
_RTC_error_fnW
__cdecl
_CRT_RTC_INITW0(
    void *_Res0,
    void **_Res1,
    int _Res2,
    int _Res3,
    int _Res4)
{
    return &_RTC_DefaultErrorFuncW;
}

void
__cdecl
_RTC_InitBase(void)
{
    static char initialized = 0;
    _RTC_error_fnW errorFunc;

    if (!initialized)
    {
        errorFunc = _CRT_RTC_INITW(0, 0, 0, 1, 0);
        _RTC_SetErrorFuncW(errorFunc);
        initialized = 1;
    }
}

void
__cdecl
_RTC_Shutdown(void)
{
    __debugbreak();
}

void
__cdecl
_RTC_Initialize(void)
{
    /* Usually this function would walk an array of function pointers and call
       each of these, like done with global ctors, but since these are currently
       only _RTC_InitBase, we simply call that function once. */
    _RTC_InitBase();
}

void
__cdecl
_RTC_Failure(
    void* retaddr,
    int errnum)
{
    _RTC_pErrorFuncW(errnum,
                     L"unknown file",
                     -1,
                     L"unknown module",
                     L"Invalid stack pointer value caught at %p, error %d\n",
                     retaddr,
                     errnum);
}

void
__cdecl
_RTC_UninitUse(
    const char *_Varname)
{
    _RTC_pErrorFuncW(_RTC_UNINIT_LOCAL_USE,
                     L"unknown file",
                     -1,
                     L"unknown module",
                     L"Use of uninitialized variable %S!\n",
                     _Varname);
}

void
__fastcall
_RTC_CheckStackVars(
    void *_Esp,
    _RTC_framedesc *_Fd)
{
    int i, *guard1, *guard2;

    /* Loop all variables in the descriptor */
    for (i = 0; i < _Fd->varCount; i++)
    {
        /* Get the 2 guards below and above the variable */
        guard1 = (int*)((char*)_Esp + _Fd->variables[i].addr - sizeof(*guard1));
        guard2 = (int*)((char*)_Esp + _Fd->variables[i].addr +_Fd->variables[i].size);

        /* Check if they contain the guard bytes */
        if ((*guard1 != 0xCCCCCCCC) || (*guard2 != 0xCCCCCCCC))
        {
            _RTC_pErrorFuncW(_RTC_CORRUPT_STACK,
                             L"unknown file",
                             -1,
                             L"unknown module",
                             L"Stack corruption near '%s'\n",
                             _Fd->variables[i].name);
        }
    }
}

void
__fastcall
_RTC_CheckStackVars2(
    void *_Esp,
    _RTC_framedesc *_Fd,
    _RTC_ALLOCA_NODE *_AllocaList)
{
    _RTC_ALLOCA_NODE *current;
    int *guard;

    /* Process normal variables */
    _RTC_CheckStackVars(_Esp, _Fd);

    /* Process the alloca list */
    for (current = _AllocaList; current != 0; current = current->next)
    {
        /* Get the upper guard */
        guard = (int*)((char*)current + current->allocaSize - sizeof(*guard));

        /* Check if all guard locations are still ok */
        if ((current->guard1 != 0xCCCCCCCC) ||
            (current->guard2[0] != 0xCCCCCCCC) ||
            (current->guard2[1] != 0xCCCCCCCC) ||
            (current->guard2[2] != 0xCCCCCCCC) ||
            (*guard != 0xCCCCCCCC))
        {
            _RTC_pErrorFuncW(_RTC_CORRUPTED_ALLOCA,
                             L"unknown file",
                             -1,
                             L"unknown module",
                             L"Stack corruption in alloca frame\n");
        }
    }
}

void
__fastcall
_RTC_AllocaHelper(
    _RTC_ALLOCA_NODE *_PAllocaBase,
    size_t _CbSize,
    _RTC_ALLOCA_NODE **_PAllocaInfoList)
{
    unsigned long i;

    /* Check if we got any allocation */
    if ((_PAllocaBase != 0) &&
        (_CbSize != 0) &&
        (_PAllocaInfoList != 0))
    {
        /* Mark the whole range */
        char *guard = (char*)_PAllocaBase;
        for (i = 0; i < _CbSize; i++)
        {
            guard[i] = 0xCC;
        }

        /* Initialize the alloca base frame */
        _PAllocaBase->allocaSize = _CbSize;

        /* Insert this frame into the alloca list */
        _PAllocaBase->next = *_PAllocaInfoList;
        *_PAllocaInfoList = _PAllocaBase;
    }
}

