#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(acos)
#endif

/*
 * @implemented
 */
double	CDECL	_CIacos(void)
{
	FPU_DOUBLE(x);
	return acos(x);
}
