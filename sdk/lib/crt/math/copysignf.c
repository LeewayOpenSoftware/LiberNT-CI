#include <math.h>
#include <stdint.h>

#ifdef _MSC_VER
#pragma warning(disable: 4164)
#pragma function(copysignf)
#endif

float __cdecl copysignf(float x, float y)
{
	union {float f; uint32_t i;} ux={x}, uy={y};
	ux.i &= 0x7fffffff;
	ux.i |= uy.i & 0x80000000;
	return ux.f;
}
