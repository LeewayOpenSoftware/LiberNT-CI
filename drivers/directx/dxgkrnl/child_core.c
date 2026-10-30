/*
 * PROJECT:     LiberNT DirectX Graphics Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Testable normalization of the child descriptors a miniport reports
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include "child_core.h"

VOID
DxgkChildNormalizeDescriptors(
    _Inout_updates_(Count) PDXGK_CHILD_DESCRIPTOR Children,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Count && Children[Index].ChildDeviceType != TypeUninitialized; Index++)
    {
        PDXGK_CHILD_CAPABILITIES Capabilities = &Children[Index].ChildCapabilities;

        if (Children[Index].ChildDeviceType != TypeIntegratedDisplay)
            continue;

        /* TODO: keep IntegratedDisplayChild.InterfaceTechnology and DescriptorLength for the integrated display descriptor query */
        Children[Index].ChildDeviceType = TypeVideoOutput;
        Capabilities->Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
        Capabilities->Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        Capabilities->Type.VideoOutput.SupportsSdtvModes = FALSE;
    }
}
