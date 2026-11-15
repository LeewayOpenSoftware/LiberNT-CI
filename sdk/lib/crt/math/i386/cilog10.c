#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(log10)
#endif

/*
 * @implemented
 */
double	CDECL	_CIlog10(void)
{
	FPU_DOUBLE(x);
	return log10(x);
}
