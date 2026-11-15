/*
 * PROJECT:     Leeway Display & Personalization
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Multi-display position and primary display over ChangeDisplaySettingsEx
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <leewayui/display_backend.h>

int
LDispGetLayout(LDISP_LAYOUT *out, int max)
{
    LDISP_MONITOR monitors[LDISP_MAX_MONITORS];
    int total = LDispEnumMonitors(monitors, ARRAYSIZE(monitors));
    int i, count = 0;

    for (i = 0; i < total && count < max; i++)
    {
        if (!monitors[i].Attached)
            continue;
        ZeroMemory(&out[count], sizeof(out[count]));
        lstrcpynW(out[count].Device, monitors[i].Device, ARRAYSIZE(out[count].Device));
        out[count].X = monitors[i].Bounds.left;
        out[count].Y = monitors[i].Bounds.top;
        out[count].Primary = monitors[i].Primary;
        count++;
    }
    return count;
}

/* The layout must describe exactly the attached displays (no attach/detach) and name one primary. */
static LONG
Validate(const LDISP_LAYOUT *layout, int count, LONG *originX, LONG *originY)
{
    LDISP_LAYOUT current[LDISP_MAX_MONITORS];
    int have = LDispGetLayout(current, ARRAYSIZE(current));
    int i, j, primaries = 0;

    if (!layout || count <= 0 || count != have)
        return DISP_CHANGE_BADPARAM;
    for (i = 0; i < count; i++)
    {
        BOOL known = FALSE;

        for (j = 0; j < have; j++)
            known |= !lstrcmpiW(layout[i].Device, current[j].Device);
        for (j = 0; j < i; j++)
        {
            if (!lstrcmpiW(layout[i].Device, layout[j].Device))
                return DISP_CHANGE_BADPARAM;
        }
        if (!known)
            return DISP_CHANGE_BADPARAM;
        if (layout[i].Primary)
        {
            primaries++;
            *originX = layout[i].X;
            *originY = layout[i].Y;
        }
    }
    return primaries == 1 ? DISP_CHANGE_SUCCESSFUL : DISP_CHANGE_BADPARAM;
}

/* Same mode as now, new position; the primary goes through CDS_SET_PRIMARY at 0,0. */
static LONG
ChangeOne(const LDISP_LAYOUT *entry, LONG originX, LONG originY, DWORD flags)
{
    DEVMODEW dm;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(entry->Device, ENUM_CURRENT_SETTINGS, &dm, 0))
        return DISP_CHANGE_FAILED;
    dm.dmPosition.x = entry->X - originX;
    dm.dmPosition.y = entry->Y - originY;
    dm.dmFields |= DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (entry->Primary)
        flags |= CDS_SET_PRIMARY;
    return ChangeDisplaySettingsExW(entry->Device, &dm, NULL, flags, NULL);
}

LONG
LDispTestLayout(const LDISP_LAYOUT *layout, int count)
{
    LONG ox = 0, oy = 0, rc;
    int i;

    rc = Validate(layout, count, &ox, &oy);
    if (rc != DISP_CHANGE_SUCCESSFUL)
        return rc;
    for (i = 0; i < count; i++)
    {
        rc = ChangeOne(&layout[i], ox, oy, CDS_TEST);
        if (rc != DISP_CHANGE_SUCCESSFUL)
            return rc;
    }
    return DISP_CHANGE_SUCCESSFUL;
}

static LONG
ApplyWithFlags(const LDISP_LAYOUT *layout, int count, DWORD flags)
{
    LONG ox = 0, oy = 0, rc;
    int i, pass;

    rc = Validate(layout, count, &ox, &oy);
    if (rc != DISP_CHANGE_SUCCESSFUL)
        return rc;
    /* Primary first so the other positions are relative to the new origin. */
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < count; i++)
        {
            if ((layout[i].Primary != FALSE) != (pass == 0))
                continue;
            rc = ChangeOne(&layout[i], ox, oy, flags);
            if (rc != DISP_CHANGE_SUCCESSFUL)
                return rc;
        }
    }
    return DISP_CHANGE_SUCCESSFUL;
}

LONG
LDispApplyLayout(const LDISP_LAYOUT *layout, int count)
{
    LONG rc = LDispTestLayout(layout, count);

    if (rc != DISP_CHANGE_SUCCESSFUL)
        return rc;
    return ApplyWithFlags(layout, count, 0);
}

LONG
LDispCommitLayout(const LDISP_LAYOUT *layout, int count)
{
    LONG rc = ApplyWithFlags(layout, count, CDS_UPDATEREGISTRY | CDS_NORESET);

    if (rc != DISP_CHANGE_SUCCESSFUL)
        return rc;
    return ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
}

/* True when 2+ displays are attached and the system accepts the current arrangement for each of them. */
BOOL
LDispLayoutWorks(void)
{
    LDISP_LAYOUT current[LDISP_MAX_MONITORS];
    int count = LDispGetLayout(current, ARRAYSIZE(current));

    return count >= 2 && LDispTestLayout(current, count) == DISP_CHANGE_SUCCESSFUL;
}
