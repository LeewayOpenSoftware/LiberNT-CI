/* Host stand-in for nvs/pool/include/poolenv.h, for the LRM-RS differential only (not kernel).
 * Provides exactly what special.c + poolspecial.h use on the host. Our test support file. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint32_t ULONG, *PULONG;
typedef uint8_t  UCHAR, BOOLEAN;
typedef void    *PVOID;
typedef uint8_t *PUCHAR;
typedef size_t   SIZE_T;
typedef uintptr_t ULONG_PTR;
#define PAGE_SIZE  4096u
#define PAGE_SHIFT 12
#define RtlFillMemory(d, len, val) memset((d), (int)(val), (size_t)(len))
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef _In_
#define _In_
#define _Out_
#define _Inout_
#define _Out_opt_
#define _In_reads_bytes_(x)
#endif
