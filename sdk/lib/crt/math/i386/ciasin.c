#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(asin)
#endif

/*
 * @implemented
 */
double	CDECL	_CIasin(void)
{
	FPU_DOUBLE(x);
	return asin(x);
}
