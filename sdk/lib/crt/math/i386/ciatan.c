#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(atan)
#endif

/*
 * @implemented
 */
double	CDECL	_CIatan(void)
{
	FPU_DOUBLE(x);
	return atan(x);
}
