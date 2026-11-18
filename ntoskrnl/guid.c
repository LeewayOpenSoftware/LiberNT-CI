/* DO NOT USE THE PRECOMPILED HEADER FOR THIS FILE! */

#include <wdm.h>
#include <initguid.h>
#include <batclass.h>
#include <poclass.h>
#include <diskguid.h>

/* NO CODE HERE, THIS IS JUST REQUIRED FOR THE GUID DEFINITIONS */

/* fsrtltypes.h only declares the oplock key ECPs that fsrtl/oplock.c reads */
DEFINE_GUID(GUID_ECP_OPLOCK_KEY, 0x48850596, 0x3050, 0x4be7, 0x98, 0x63, 0xfe, 0xc3, 0x50, 0xce, 0x8d, 0x7f);
DEFINE_GUID(GUID_ECP_DUAL_OPLOCK_KEY, 0x41621a14, 0xb08b, 0x4df1, 0xb6, 0x76, 0xa0, 0x5f, 0xfd, 0xf0, 0x1b, 0xea);
