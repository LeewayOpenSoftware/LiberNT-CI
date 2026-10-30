/*
 * PROJECT:     LiberNT KernelBase
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     GetDurationFormat and GetDurationFormatEx, written from the
 *              published picture-string rules
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#include <windows.h>
#include <winternl.h>

#define TICKS_PER_SECOND 10000000ULL
#define TICKS_PER_MINUTE (60ULL * TICKS_PER_SECOND)
#define TICKS_PER_HOUR   (60ULL * TICKS_PER_MINUTE)
#define TICKS_PER_DAY    (24ULL * TICKS_PER_HOUR)

/* The picture used when the caller gives none */
#define DEFAULT_DURATION_FORMAT L"h:mm:ss"

typedef enum _FIELD
{
    FieldDays,
    FieldHours,
    FieldMinutes,
    FieldSeconds,
    FieldFraction
} FIELD;

static
ULONGLONG
FieldUnit(
    _In_ FIELD Field)
{
    switch (Field)
    {
        case FieldDays: return TICKS_PER_DAY;
        case FieldHours: return TICKS_PER_HOUR;
        case FieldMinutes: return TICKS_PER_MINUTE;
        default: return TICKS_PER_SECOND;
    }
}

static
SIZE_T
PrintNumber(
    _In_ ULONGLONG Value,
    _In_ SIZE_T MinDigits,
    _Out_writes_(32) PWCHAR Out)
{
    WCHAR Reversed[24];
    SIZE_T Count = 0, Length = 0;

    do
    {
        Reversed[Count++] = (WCHAR)(L'0' + (Value % 10));
        Value /= 10;
    } while (Value);

    while (Count < MinDigits)
        Reversed[Count++] = L'0';

    while (Count)
        Out[Length++] = Reversed[--Count];

    return Length;
}

/*
 * Reads one numeric field starting at a format character. Returns the field and
 * how many characters it spans, or FALSE when the run is longer than allowed.
 */
static
BOOL
ParseField(
    _In_ PCWSTR Format,
    _Out_ FIELD *Field,
    _Out_ SIZE_T *Run,
    _Out_ BOOL *IsField)
{
    WCHAR Ch = Format[0];
    SIZE_T Length = 1;
    SIZE_T Max;

    *IsField = TRUE;
    switch (Ch)
    {
        case L'd': *Field = FieldDays; Max = 2; break;
        case L'h': case L'H': *Field = FieldHours; Max = 2; break;
        case L'm': *Field = FieldMinutes; Max = 2; break;
        case L's': *Field = FieldSeconds; Max = 2; break;
        case L'f': *Field = FieldFraction; Max = 9; break;
        default:
            *IsField = FALSE;
            *Run = 1;
            return TRUE;
    }

    while (Format[Length] == Ch || (Ch == L'h' && Format[Length] == L'H') ||
           (Ch == L'H' && Format[Length] == L'h'))
        Length++;

    *Run = Length;
    return Length <= Max;
}

/*
 * The value a field shows. The first field absorbs every larger unit; the later
 * ones show what is left after the field printed before them.
 */
static
ULONGLONG
FieldValue(
    _In_ ULONGLONG Ticks,
    _In_ FIELD Field,
    _In_ SIZE_T Digits,
    _In_ BOOL IsFirst,
    _In_ BOOL HasPrevious,
    _In_ FIELD Previous)
{
    if (Field == FieldFraction)
    {
        /* Fractions below the first field are the part of a second */
        ULONGLONG Sub = IsFirst ? Ticks : Ticks % TICKS_PER_SECOND;

        if (Digits <= 7)
        {
            ULONGLONG Divisor = 1;
            SIZE_T i;

            for (i = Digits; i < 7; i++)
                Divisor *= 10;
            return Sub / Divisor;
        }
        return Sub;
    }

    if (IsFirst)
        return Ticks / FieldUnit(Field);

    if (HasPrevious && Previous != FieldFraction)
        return (Ticks % FieldUnit(Previous)) / FieldUnit(Field);

    return Ticks / FieldUnit(Field);
}

static
int
FormatDuration(
    _In_ ULONGLONG Ticks,
    _In_ PCWSTR Format,
    _Out_writes_opt_(Size) PWSTR Buffer,
    _In_ int Size)
{
    SIZE_T FormatLength = wcslen(Format);
    PWSTR Work;
    SIZE_T Out = 0;
    BOOL SeenField = FALSE;
    BOOL HasPrevious = FALSE;
    FIELD Previous = FieldDays;
    PCWSTR Cursor = Format;
    BOOL InQuote = FALSE;

    Work = HeapAlloc(GetProcessHeap(), 0, (FormatLength * 32 + 1) * sizeof(WCHAR));
    if (!Work)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    while (*Cursor)
    {
        FIELD Field;
        SIZE_T Run;
        BOOL IsField;

        if (*Cursor == L'\'')
        {
            if (InQuote && Cursor[1] == L'\'')
            {
                Work[Out++] = L'\'';
                Cursor += 2;
                continue;
            }
            InQuote = !InQuote;
            Cursor++;
            continue;
        }

        if (InQuote)
        {
            Work[Out++] = *Cursor++;
            continue;
        }

        if (!ParseField(Cursor, &Field, &Run, &IsField))
        {
            HeapFree(GetProcessHeap(), 0, Work);
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }

        if (!IsField)
        {
            Work[Out++] = *Cursor++;
            continue;
        }

        if (Field == FieldFraction)
        {
            ULONGLONG Value = FieldValue(Ticks, Field, Run, !SeenField, HasPrevious, Previous);

            if (!SeenField && Run > 7)
            {
                /* More digits than the 100 ns resolution holds: the extra ones are zeros */
                SIZE_T Extra;

                Out += PrintNumber(Value, 0, Work + Out);
                for (Extra = 7; Extra < Run; Extra++)
                    Work[Out++] = L'0';
            }
            else if (!SeenField)
            {
                Out += PrintNumber(Value, 0, Work + Out);
            }
            else
            {
                Out += PrintNumber(Value, Run > 7 ? 7 : Run, Work + Out);
                if (Run > 7)
                {
                    SIZE_T Extra;

                    for (Extra = 7; Extra < Run; Extra++)
                        Work[Out++] = L'0';
                }
            }
        }
        else
        {
            ULONGLONG Value = FieldValue(Ticks, Field, Run, !SeenField, HasPrevious, Previous);

            Out += PrintNumber(Value, Run, Work + Out);
        }

        SeenField = TRUE;
        HasPrevious = TRUE;
        Previous = Field;
        Cursor += Run;
    }

    Work[Out] = UNICODE_NULL;

    if (Size == 0)
    {
        HeapFree(GetProcessHeap(), 0, Work);
        return (int)(Out + 1);
    }

    if (!Buffer || (SIZE_T)Size < Out + 1)
    {
        HeapFree(GetProcessHeap(), 0, Work);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    RtlCopyMemory(Buffer, Work, (Out + 1) * sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, Work);
    return (int)(Out + 1);
}

static
BOOL
IsKnownLocaleName(
    _In_opt_ LPCWSTR Name)
{
    /* NULL and the predefined names are always acceptable */
    if (!Name || !Name[0] || Name[0] == L'!')
        return TRUE;

    return IsValidLocaleName(Name);
}

int
WINAPI
GetDurationFormatEx(
    _In_opt_ LPCWSTR LocaleName,
    _In_ DWORD Flags,
    _In_opt_ const SYSTEMTIME *Duration,
    _In_ ULONGLONG DurationTicks,
    _In_opt_ LPCWSTR Format,
    _Out_writes_opt_(Size) LPWSTR DurationStr,
    _In_ int Size)
{
    ULONGLONG Ticks;

    if (Size < 0 || !IsKnownLocaleName(LocaleName))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    /* Only the locale default picture can take LOCALE_NOUSEROVERRIDE */
    if (Flags & ~(Format ? 0 : LOCALE_NOUSEROVERRIDE))
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }

    if (Duration)
    {
        /* Year, month and day of week are ignored; wDay counts days */
        Ticks = (ULONGLONG)Duration->wDay * TICKS_PER_DAY +
                (ULONGLONG)Duration->wHour * TICKS_PER_HOUR +
                (ULONGLONG)Duration->wMinute * TICKS_PER_MINUTE +
                (ULONGLONG)Duration->wSecond * TICKS_PER_SECOND +
                (ULONGLONG)Duration->wMilliseconds * 10000ULL;
    }
    else
    {
        Ticks = DurationTicks;
    }

    return FormatDuration(Ticks, Format ? Format : DEFAULT_DURATION_FORMAT, DurationStr, Size);
}

int
WINAPI
GetDurationFormat(
    _In_ LCID Locale,
    _In_ DWORD Flags,
    _In_opt_ const SYSTEMTIME *Duration,
    _In_ ULONGLONG DurationTicks,
    _In_opt_ LPCWSTR Format,
    _Out_writes_opt_(Size) LPWSTR DurationStr,
    _In_ int Size)
{
    WCHAR Name[LOCALE_NAME_MAX_LENGTH];

    /* Predefined locale ids carry no name of their own */
    if (Locale == LOCALE_USER_DEFAULT || Locale == LOCALE_SYSTEM_DEFAULT || Locale == LOCALE_NEUTRAL)
        return GetDurationFormatEx(NULL, Flags, Duration, DurationTicks, Format, DurationStr, Size);

    if (!LCIDToLocaleName(Locale, Name, ARRAYSIZE(Name), 0))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    return GetDurationFormatEx(Name, Flags, Duration, DurationTicks, Format, DurationStr, Size);
}
