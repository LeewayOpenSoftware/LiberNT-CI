#include <precomp.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma function(fmod)
#endif

/*
 * @implemented
 */
double	CDECL	_CIfmod(void)
{
	FPU_DOUBLES(x, y);
	return fmod(x, y);
}
