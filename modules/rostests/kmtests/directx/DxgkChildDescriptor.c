/*
 * PROJECT:     LiberNT kernel-mode tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Normalization of the child descriptors a display miniport reports
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <kmt_test.h>
#include <windef.h>
#include "child_core.h"

START_TEST(DxgkChildDescriptor)
{
    DXGK_CHILD_DESCRIPTOR Children[4];

    RtlZeroMemory(Children, sizeof(Children));
    /* Comet Lake-H laptop iGPU: integrated eDP panel and an HDMI output. */
    Children[0].ChildDeviceType = TypeIntegratedDisplay;
    Children[0].ChildCapabilities.Type.IntegratedDisplayChild.InterfaceTechnology = D3DKMDT_VOT_DISPLAYPORT_EMBEDDED;
    Children[0].ChildCapabilities.Type.IntegratedDisplayChild.DescriptorLength = 128;
    Children[0].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
    Children[0].AcpiUid = 0x400;
    Children[0].ChildUid = 0x40F04;
    Children[1].ChildDeviceType = TypeVideoOutput;
    Children[1].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HDMI;
    Children[1].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    Children[1].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
    Children[1].ChildUid = 0x31003;
    Children[3].ChildDeviceType = TypeIntegratedDisplay;

    DxgkChildNormalizeDescriptors(Children, RTL_NUMBER_OF(Children));

    ok_eq_long(Children[0].ChildDeviceType, TypeVideoOutput);
    ok_eq_long(Children[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology, D3DKMDT_VOT_INTERNAL);
    ok_eq_long(Children[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness, D3DKMDT_MOA_NONE);
    ok_eq_bool(Children[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes, FALSE);
    ok_eq_long(Children[0].ChildCapabilities.HpdAwareness, HpdAwarenessInterruptible);
    ok_eq_ulong(Children[0].AcpiUid, 0x400UL);
    ok_eq_ulong(Children[0].ChildUid, 0x40F04UL);

    ok_eq_long(Children[1].ChildDeviceType, TypeVideoOutput);
    ok_eq_long(Children[1].ChildCapabilities.Type.VideoOutput.InterfaceTechnology, D3DKMDT_VOT_HDMI);
    ok_eq_ulong(Children[1].ChildUid, 0x31003UL);

    ok_eq_long(Children[3].ChildDeviceType, TypeIntegratedDisplay);
}
