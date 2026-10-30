
#include "umwdm.h"
#include <ntintsafe.h>
#include <karray.h>
#include <wil/resource.h>

#include <nxtrace.hpp>
#include <nxtracelogging.hpp>
#include <netclientbuffer.h>

#ifndef _KERNEL_MODE
#include <mockdma.h>
#endif

#define BUFFER_MANAGER_POOL_TAG 'mbxc'

