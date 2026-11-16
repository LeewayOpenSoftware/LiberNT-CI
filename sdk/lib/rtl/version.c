/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * PURPOSE:         Runtime code
 * FILE:            lib/rtl/version.c
 * PROGRAMERS:      Filip Navara
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

NTSTATUS
NTAPI
RtlGetVersion(OUT PRTL_OSVERSIONINFOW lpVersionInformation);

/* FUNCTIONS ****************************************************************/

static UCHAR
RtlpVerGetCondition(IN ULONGLONG ConditionMask,
                    IN ULONG TypeMask);

static BOOLEAN
RtlpVerCompare(ULONG left, ULONG right, UCHAR Condition)
{
    switch (Condition)
    {
        case VER_EQUAL:
            return (left == right);
        case VER_GREATER:
            return (left > right);
        case VER_GREATER_EQUAL:
            return (left >= right);
        case VER_LESS:
            return (left < right);
        case VER_LESS_EQUAL:
            return (left <= right);
        default:
            break;
    }
    return FALSE;
}

/*
* @implemented
*/
NTSTATUS
NTAPI
RtlVerifyVersionInfo(IN PRTL_OSVERSIONINFOEXW VersionInfo,
                     IN ULONG TypeMask,
                     IN ULONGLONG ConditionMask)
{
    RTL_OSVERSIONINFOEXW Version;
    BOOLEAN Comparison;
    BOOLEAN DoNextCheck;
    NTSTATUS Status;
    UCHAR Condition;
    static const ULONG CheckFields[] =
    {
        VER_MINORVERSION, VER_MAJORVERSION, VER_BUILDNUMBER, VER_PLATFORMID,
        VER_SERVICEPACKMINOR, VER_SERVICEPACKMAJOR, VER_SUITENAME, VER_PRODUCT_TYPE
    };
    ULONG i;

    /* FIXME:
        - Check the following special case on Windows (various versions):
          o lp->wSuiteMask == 0 and ver.wSuiteMask != 0 and VER_AND/VER_OR
          o lp->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW)
        - MSDN talks about some tests being impossible. Check what really happens.
     */

    Version.dwOSVersionInfoSize = sizeof(Version);

    Status = RtlGetVersion((PRTL_OSVERSIONINFOW)&Version);
    if (Status != STATUS_SUCCESS)
    {
        return Status;
    }

    if (!TypeMask || !ConditionMask)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((ConditionMask & (1ULL << 63)) == 0)
    {
        /* Without the marker bit VerSetConditionMask sets, a listed field
           whose condition is zero fails the call (ERROR_OLD_WIN_VERSION). */
        for (i = 0; i < RTL_NUMBER_OF(CheckFields); i++)
        {
            if ((TypeMask & CheckFields[i]) &&
                RtlpVerGetCondition(ConditionMask, CheckFields[i]) == 0)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }
    }

    if (TypeMask & VER_PRODUCT_TYPE)
    {
        Condition = RtlpVerGetCondition(ConditionMask, VER_PRODUCT_TYPE);
        if (Condition != 0)
        {
            Comparison = RtlpVerCompare(Version.wProductType,
                                        VersionInfo->wProductType,
                                        Condition);
            if (!Comparison)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }
    }

    if (TypeMask & VER_SUITENAME)
    {
        switch (RtlpVerGetCondition(ConditionMask, VER_SUITENAME))
        {
            case 0:
                /* Unconditioned field under the VerSetConditionMask bit: pass. */
                break;

            case VER_AND:
            {
                if ((VersionInfo->wSuiteMask & Version.wSuiteMask) != VersionInfo->wSuiteMask)
                {
                    return STATUS_REVISION_MISMATCH;
                }
            }
            break;

            case VER_OR:
            {
                if (!(VersionInfo->wSuiteMask & Version.wSuiteMask) && VersionInfo->wSuiteMask)
                {
                    return STATUS_REVISION_MISMATCH;
                }
                break;
            }

            default:
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
    }

    if (TypeMask & VER_PLATFORMID)
    {
        Condition = RtlpVerGetCondition(ConditionMask, VER_PLATFORMID);
        if (Condition != 0)
        {
            Comparison = RtlpVerCompare(Version.dwPlatformId,
                                        VersionInfo->dwPlatformId,
                                        Condition);
            if (!Comparison)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }
    }

    if (TypeMask & VER_BUILDNUMBER)
    {
        Condition = RtlpVerGetCondition(ConditionMask, VER_BUILDNUMBER);
        if (Condition != 0)
        {
            Comparison = RtlpVerCompare(Version.dwBuildNumber,
                                        VersionInfo->dwBuildNumber,
                                        Condition);
            if (!Comparison)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }
    }

    DoNextCheck = TRUE;
    Condition = VER_EQUAL;

    if (TypeMask & VER_MAJORVERSION)
    {
        Condition = RtlpVerGetCondition(ConditionMask, VER_MAJORVERSION);
        DoNextCheck = TRUE;
        if (Condition != 0)
        {
            DoNextCheck = (VersionInfo->dwMajorVersion == Version.dwMajorVersion);
            Comparison = RtlpVerCompare(Version.dwMajorVersion,
                                        VersionInfo->dwMajorVersion,
                                        Condition);

            if (!Comparison && !DoNextCheck)
            {
                return STATUS_REVISION_MISMATCH;
            }
        }
    }

    if (DoNextCheck)
    {
        if (TypeMask & VER_MINORVERSION)
        {
            if (Condition == VER_EQUAL)
            {
                Condition = RtlpVerGetCondition(ConditionMask, VER_MINORVERSION);
            }

            DoNextCheck = TRUE;
            if (Condition != 0)
            {
                DoNextCheck = (VersionInfo->dwMinorVersion == Version.dwMinorVersion);
                Comparison = RtlpVerCompare(Version.dwMinorVersion,
                                            VersionInfo->dwMinorVersion,
                                            Condition);

                if (!Comparison && !DoNextCheck)
                {
                    return STATUS_REVISION_MISMATCH;
                }
            }
        }

        if (DoNextCheck && (TypeMask & VER_SERVICEPACKMAJOR))
        {
            if (Condition == VER_EQUAL)
            {
                Condition = RtlpVerGetCondition(ConditionMask, VER_SERVICEPACKMAJOR);
            }

            DoNextCheck = TRUE;
            if (Condition != 0)
            {
                DoNextCheck = (VersionInfo->wServicePackMajor == Version.wServicePackMajor);
                Comparison = RtlpVerCompare(Version.wServicePackMajor,
                                            VersionInfo->wServicePackMajor,
                                            Condition);

                if (!Comparison && !DoNextCheck)
                {
                    return STATUS_REVISION_MISMATCH;
                }
            }

            if (DoNextCheck && (TypeMask & VER_SERVICEPACKMINOR))
            {
                if (Condition == VER_EQUAL)
                {
                    Condition = RtlpVerGetCondition(ConditionMask, VER_SERVICEPACKMINOR);
                }

                DoNextCheck = TRUE;
                if (Condition != 0)
                {
                    Comparison = RtlpVerCompare(Version.wServicePackMinor,
                                                VersionInfo->wServicePackMinor,
                                                Condition);

                    if (!Comparison)
                    {
                        return STATUS_REVISION_MISMATCH;
                    }
                }
            }
        }
    }

    return STATUS_SUCCESS;
}

static UCHAR
RtlpVerGetCondition(IN ULONGLONG ConditionMask,
                    IN ULONG TypeMask)
{
    UCHAR Condition = 0;

    if (TypeMask & VER_PRODUCT_TYPE)
        Condition |= ConditionMask >> (7 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SUITENAME)
        Condition |= ConditionMask >> (6 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_PLATFORMID)
        Condition |= ConditionMask >> (3 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_BUILDNUMBER)
        Condition |= ConditionMask >> (2 * VER_NUM_BITS_PER_CONDITION_MASK);
    /*
     * We choose here the lexicographical order on the 4D space
     * {(Major ; Minor ; SP Major ; SP Minor)} to select the
     * appropriate comparison operator.
     * Therefore the following 'else if' instructions must be in this order.
     */
    else if (TypeMask & VER_MAJORVERSION)
        Condition |= ConditionMask >> (1 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_MINORVERSION)
        Condition |= ConditionMask >> (0 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SERVICEPACKMAJOR)
        Condition |= ConditionMask >> (5 * VER_NUM_BITS_PER_CONDITION_MASK);
    else if (TypeMask & VER_SERVICEPACKMINOR)
        Condition |= ConditionMask >> (4 * VER_NUM_BITS_PER_CONDITION_MASK);

    Condition &= VER_CONDITION_MASK;

    return Condition;
}

/*
 * @implemented
 */
ULONGLONG
NTAPI
VerSetConditionMask(IN ULONGLONG ConditionMask,
                    IN ULONG TypeMask,
                    IN UCHAR Condition)
{
    ULONG Shift = 0;

    if (TypeMask == 0)
        return 0;

    do
    {
        TypeMask >>= 1;
        Shift += VER_NUM_BITS_PER_CONDITION_MASK;
    } while (TypeMask != 0);

    Shift = (Shift - VER_NUM_BITS_PER_CONDITION_MASK) & 63;
    return ConditionMask | ((ULONGLONG)(Condition & VER_CONDITION_MASK) << Shift) | (1ULL << 63);
}

/* EOF */
