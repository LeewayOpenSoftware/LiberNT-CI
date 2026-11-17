/*
 * Win32 5.1 Themes
 *
 * Copyright (C) 2003 Kevin Koltzau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "uxthemedll.h"
#include <pseh/pseh2.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uxtheme);

/***********************************************************************/

/* For the moment, do nothing here. */
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, void *reserved)
{
    TRACE("%p 0x%lx %p\n", hInstDLL, fdwReason, reserved);
    switch(fdwReason) {
        case DLL_PROCESS_ATTACH:
        {
            DisableThreadLibraryCalls(hInstDLL);
            /* Park trust events across attach so no worker is queued before process init finishes;
               the finally clears the gate on every exit from InitSystem, an exception included. */
            UXTHEME_TrustBeginAttachDefer();
            _SEH2_TRY
            {
                UXTHEME_InitSystem(hInstDLL);
            }
            _SEH2_FINALLY
            {
                UXTHEME_TrustEndAttachDefer();
            }
            _SEH2_END;
            break;
        }
        case DLL_PROCESS_DETACH:
            if (reserved) break;
            UXTHEME_UninitSystem();
            break;
    }
    return TRUE;
}
