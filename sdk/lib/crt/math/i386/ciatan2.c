#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(atan2)
#endif

/*
 * @implemented
 */
double	CDECL	_CIatan2(void)
{
	FPU_DOUBLES(y, x);
	return atan2(y, x);
}
