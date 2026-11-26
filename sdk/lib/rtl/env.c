/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            lib/rtl/env.c
 * PURPOSE:         Environment functions
 * PROGRAMMER:      Eric Kohl
 */

/* INCLUDES ******************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 *
 * Replaces the current environment block with a copy of NewEnvironment, as
 * Windows 11 26100 does: the block is not validated, NewEnvironmentSize bytes
 * are copied, EnvironmentSize becomes NewEnvironmentSize and EnvironmentVersion
 * is incremented. A block that fits the current allocation is copied in
 * place; otherwise a new block is allocated and the old one destroyed.
 */
NTSTATUS
NTAPI
RtlSetEnvironmentStrings(
    _In_reads_bytes_(NewEnvironmentSize) PCWCHAR NewEnvironment,
    _In_ SIZE_T NewEnvironmentSize)
{
    PRTL_USER_PROCESS_PARAMETERS Params;
    PWSTR Current, NewBlock = NULL;

    RtlAcquirePebLock();
    Params = NtCurrentPeb()->ProcessParameters;
    Current = Params->Environment;

    /* The PEB environment is a process-heap block, as RtlSetEnvironmentVariable assumes */
    if (Current && NewEnvironmentSize <= RtlSizeHeap(RtlGetProcessHeap(), 0, Current))
    {
        RtlMoveMemory(Current, NewEnvironment, NewEnvironmentSize);
    }
    else
    {
        /* Two spare zero characters keep an unterminated block bounded */
        if (NewEnvironmentSize > MAXULONG_PTR - 2 * sizeof(WCHAR))
        {
            RtlReleasePebLock();
            return STATUS_NO_MEMORY;
        }
        NewBlock = RtlAllocateHeap(RtlGetProcessHeap(),
                                   HEAP_ZERO_MEMORY,
                                   NewEnvironmentSize + 2 * sizeof(WCHAR));
        if (!NewBlock)
        {
            RtlReleasePebLock();
            return STATUS_NO_MEMORY;
        }
        RtlCopyMemory(NewBlock, NewEnvironment, NewEnvironmentSize);
        Params->Environment = NewBlock;
    }
    Params->EnvironmentSize = NewEnvironmentSize;
    Params->EnvironmentVersion++;
    RtlReleasePebLock();

    if (NewBlock && Current)
        RtlDestroyEnvironment(Current);
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlCreateEnvironment(
    _In_ BOOLEAN Inherit,
    _Out_ PWSTR *OutEnvironment)
{
    PWSTR CurrentEnvironment, End, NewEnvironment = NULL;
    SIZE_T Size;

    if (Inherit)
    {
        RtlAcquirePebLock();

        CurrentEnvironment = NtCurrentPeb()->ProcessParameters->Environment;
        if (CurrentEnvironment != NULL)
        {
            End = CurrentEnvironment;
            while (*End)
                End += wcslen(End) + 1;
            Size = (End + 1 - CurrentEnvironment) * sizeof(WCHAR);

            NewEnvironment = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
            if (NewEnvironment != NULL)
                RtlCopyMemory(NewEnvironment, CurrentEnvironment, Size);
        }

        RtlReleasePebLock();
    }

    if ((NewEnvironment == NULL) && (!Inherit || !NtCurrentPeb()->ProcessParameters->Environment))
        NewEnvironment = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, 2 * sizeof(WCHAR));

    *OutEnvironment = NewEnvironment;
    return NewEnvironment ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}


/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlDestroyEnvironment(_In_ PWSTR Environment)
{
    RtlFreeHeap(RtlGetProcessHeap(), 0, Environment);
    return STATUS_SUCCESS;
}


static
SIZE_T
RtlpEnvironmentSize(_In_opt_ PWSTR Environment)
{
    if (!Environment)
        return 0;
    return RtlSizeHeap(RtlGetProcessHeap(), 0, Environment);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlExpandEnvironmentStrings(
    _In_opt_ PVOID Environment,
    _In_reads_(SourceLength) PCWSTR SourceBuffer,
    _In_ SIZE_T SourceLength,
    _Out_writes_(DestMax) PWSTR Destination,
    _In_ SIZE_T DestMax,
    _Out_opt_ PSIZE_T Length)
{
    UNICODE_STRING Variable;
    UNICODE_STRING Value;
    NTSTATUS Status;
    PCWSTR CopyBuffer;
    PCWSTR VariableEnd;
    SIZE_T CopyLength;
    SIZE_T Tail;
    SIZE_T Position = 0;
    SIZE_T Available;
    SIZE_T TotalLength = 1; /* for terminating NULL */
    BOOLEAN Stopped = FALSE;

    DPRINT("RtlExpandEnvironmentStrings(%p, %S, %Iu, %p, %Iu, %p)\n",
           Environment, SourceBuffer, SourceLength, Destination, DestMax, Length);

    if (!Destination)
        DestMax = 0;

    while (SourceLength)
    {
        if (*SourceBuffer != L'%')
        {
            CopyBuffer = SourceBuffer;
            CopyLength = 0;
            while (SourceLength != 0 && *SourceBuffer != L'%')
            {
                SourceBuffer++;
                CopyLength++;
                SourceLength--;
            }
        }
        else
        {
           /* Process environment variable. */

            VariableEnd = SourceBuffer + 1;
            Tail = SourceLength - 1;
            while (*VariableEnd != L'%' && Tail != 0)
            {
                VariableEnd++;
                Tail--;
            }

            if (Tail != 0)
            {
                Variable.MaximumLength =
                    Variable.Length = (USHORT)(VariableEnd - (SourceBuffer + 1)) * sizeof(WCHAR);
                Variable.Buffer = (PWSTR)SourceBuffer + 1;

                Available = Stopped ? 0 : DestMax - Position;
                Value.Length = 0;
                Value.MaximumLength = (USHORT)min(Available, UNICODE_STRING_MAX_CHARS) * sizeof(WCHAR);
                Value.Buffer = Destination + Position;

                Status = RtlQueryEnvironmentVariable_U(Environment, &Variable,
                                                       &Value);
                if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_TOO_SMALL)
                {
                    SourceBuffer = VariableEnd + 1;
                    SourceLength = Tail - 1;
                    TotalLength += Value.Length / sizeof(WCHAR);
                    if (Status == STATUS_BUFFER_TOO_SMALL)
                        Stopped = TRUE;
                    else if (!Stopped)
                        Position += Value.Length / sizeof(WCHAR);
                    continue;
                }
                else
                {
                   /* Variable not found. */
                    CopyBuffer = SourceBuffer;
                    CopyLength = SourceLength - Tail + 1;
                    SourceLength -= CopyLength;
                    SourceBuffer += CopyLength;
                }
            }
            else
            {
               /* Unfinished variable name. */
                CopyBuffer = SourceBuffer;
                CopyLength = SourceLength;
                SourceLength = 0;
            }
        }

        TotalLength += CopyLength;
        if (!Stopped)
        {
            Available = (DestMax > Position + 1) ? DestMax - Position - 1 : 0;
            if (CopyLength > Available)
            {
                CopyLength = Available;
                Stopped = TRUE;
            }
            RtlCopyMemory(Destination + Position, CopyBuffer, CopyLength * sizeof(WCHAR));
            Position += CopyLength;
        }
    }

    if (!Stopped && Position < DestMax)
        Destination[Position] = UNICODE_NULL;

    if (Length != NULL)
        *Length = TotalLength;

    return (TotalLength > DestMax) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RtlExpandEnvironmentStrings_U(
    _In_opt_ PWSTR Environment,
    _In_ PUNICODE_STRING SourceString,
    _Inout_ PUNICODE_STRING DestinationString,
    _In_ PULONG ReturnLength)
{
    SIZE_T ResultLength;
    NTSTATUS Status;

    DPRINT("RtlExpandEnvironmentStrings_U %p %wZ %p %p\n",
           Environment, SourceString, DestinationString, ReturnLength);

    /* Call the buffer version */
    Status = RtlExpandEnvironmentStrings(
        Environment,
        SourceString->Buffer,
        SourceString->Length / sizeof(WCHAR),
        DestinationString->Buffer,
        DestinationString->MaximumLength / sizeof(WCHAR),
        &ResultLength);

    /* Make sure the result length does not exceed the maximum */
    if (ResultLength > UNICODE_STRING_MAX_CHARS)
    {
        DPRINT("ReturnLength %lu exceeds maximum\n", ResultLength);
        Status = STATUS_UNSUCCESSFUL;
        ResultLength = 0;
    }

    if (NT_SUCCESS(Status))
    {
        DestinationString->Length =
            ((USHORT)ResultLength * sizeof(WCHAR)) - sizeof(UNICODE_NULL);
    }

    if (ReturnLength != NULL)
    {
        *ReturnLength = (ULONG)ResultLength * sizeof(WCHAR);
    }

    return Status;
}

/*
 * @implemented
 */
VOID
NTAPI
RtlSetCurrentEnvironment(
    _In_ PWSTR NewEnvironment,
    _Out_opt_ PWSTR *OldEnvironment)
{
    PVOID EnvPtr;

    DPRINT("NewEnvironment 0x%p OldEnvironment 0x%p\n",
           NewEnvironment, OldEnvironment);

    RtlAcquirePebLock();

    EnvPtr = NtCurrentPeb()->ProcessParameters->Environment;
    NtCurrentPeb()->ProcessParameters->Environment = NewEnvironment;
    NtCurrentPeb()->ProcessParameters->EnvironmentSize = RtlpEnvironmentSize(NewEnvironment);

    RtlReleasePebLock();

    if (OldEnvironment != NULL)
        *OldEnvironment = EnvPtr;
    else
        RtlDestroyEnvironment(EnvPtr);
}


static
LONG
RtlpCompareEnvironmentName(
    _In_reads_(Length1) PCWSTR Name1,
    _In_ SIZE_T Length1,
    _In_reads_(Length2) PCWSTR Name2,
    _In_ SIZE_T Length2)
{
    SIZE_T Index;
    LONG Result;

    for (Index = 0; Index < min(Length1, Length2); Index++)
    {
        Result = (LONG)RtlUpcaseUnicodeChar(Name1[Index]) - (LONG)RtlUpcaseUnicodeChar(Name2[Index]);
        if (Result)
            return Result;
    }
    return (Length1 < Length2) ? -1 : (Length1 > Length2);
}

NTSTATUS
NTAPI
RtlSetEnvironmentVar(
    _Inout_opt_ PWSTR *Environment,
    _In_reads_(NameLength) PCWSTR Name,
    _In_ SIZE_T NameLength,
    _In_reads_opt_(ValueLength) PCWSTR Value,
    _In_ SIZE_T ValueLength)
{
    size_t hole_len, new_len, env_len = 0, var_len;
    WCHAR *new_env = 0, *env_end = 0, *wcs, *env, *val = 0, *tail = 0, *hole = 0, *var;
    PCWSTR name_wcs;
    PWSTR head = NULL;
    SIZE_T new_size;
    LONG f = 1;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("RtlSetEnvironmentVar(Environment %p Name %.*S Value %p)\n",
           Environment, (int)NameLength, Name, Value);

    /* Variable name must not be empty */
    if (NameLength == 0)
        return STATUS_INVALID_PARAMETER;

     /* Variable names can't contain a '=' except as a first character. */
    for (name_wcs = Name + 1; name_wcs < Name + NameLength; name_wcs++)
    {
        if (*name_wcs == L'=')
            return STATUS_INVALID_PARAMETER;
    }

    if (Value && ValueLength > (MAXSIZE_T / sizeof(WCHAR)) - NameLength - 2)
        return STATUS_NO_MEMORY;

    if (Environment)
    {
        env = *Environment;
    }
    else
    {
        RtlAcquirePebLock();
        env = NtCurrentPeb()->ProcessParameters->Environment;
    }

    if (env)
    {
       /* get environment length */
        wcs = env_end = env;
        do
        {
            env_end += wcslen(env_end) + 1;
        } while (*env_end);
        env_end++;
        env_len = env_end - env;
        DPRINT("environment length %lu characters\n", env_len);

        /* find where to insert */
        while (*wcs)
        {
            var = wcs++;
            wcs = wcschr(wcs, L'=');
            if (wcs == NULL)
            {
                wcs = var + wcslen(var);
            }
            if (*wcs)
            {
                var_len = wcs - var;
                val = ++wcs;
                wcs += wcslen(wcs);
                f = RtlpCompareEnvironmentName(var, var_len, Name, NameLength);
                if (f >= 0)
                {
                    if (f) /* Insert before found */
                    {
                        hole = tail = var;
                    }
                    else /* Exact match */
                    {
                        head = var;
                        tail = ++wcs;
                        hole = val;
                    }
                    goto found;
                }
            }
            wcs++;
        }
        hole = tail = wcs; /* Append to environment */
    }

found:
    if (Value != NULL)
    {
        hole_len = tail - hole;
        /* calculate new environment size */
        new_size = (ValueLength + 1) * sizeof(WCHAR);
        /* adding new variable */
        if (f)
            new_size += (NameLength + 1) * sizeof(WCHAR);
        new_len = new_size / sizeof(WCHAR);
        if (hole_len < new_len)
        {
           /* enlarge environment size */
           /* check the size of available memory */
            new_size += (env_len - hole_len) * sizeof(WCHAR);
            if (!env)
                new_size += sizeof(WCHAR);
            DPRINT("new_size %lu\n", new_size);

            if (!env || new_size > RtlSizeHeap(RtlGetProcessHeap(), 0, env))
            {
               /* reallocate memory area */
                new_env = RtlAllocateHeap(RtlGetProcessHeap(), 0, new_size);
                if (new_env == NULL)
                {
                    if (Environment == NULL)
                    {
                        RtlReleasePebLock();
                    }
                    return STATUS_NO_MEMORY;
                }

                if (env)
                {
                    memmove(new_env,
                            env,
                            (hole - env) * sizeof(WCHAR));
                    hole = new_env + (hole - env);
                }
                else
                {
                   /* absolutely new environment */
                    tail = hole = new_env;
                    *hole = 0;
                    env_end = hole + 1;
                }
            }
        }

        /* move tail */
        memmove (hole + new_len, tail, (env_end - tail) * sizeof(WCHAR));

        if (new_env)
        {
           /* we reallocated environment, let's free the old one */
            if (Environment)
                *Environment = new_env;
            else
                NtCurrentPeb()->ProcessParameters->Environment = new_env;

            if (env)
                RtlFreeHeap(RtlGetProcessHeap(), 0, env);
        }

        /* and now copy given stuff */
        if (f)
        {
           /* copy variable name and '=' character */
            memmove(hole,
                    Name,
                    NameLength * sizeof(WCHAR));
            hole += NameLength;
            *hole++ = L'=';
        }

        /* copy value */
        memmove(hole,
                Value,
                ValueLength * sizeof(WCHAR));
        hole += ValueLength;
        *hole = 0;
    }
    else
    {
       /* remove the environment variable */
        if (f == 0)
        {
            memmove(head,
                    tail,
                    (env_end - tail) * sizeof(WCHAR));
        }
    }

    if (Environment == NULL)
    {
        NtCurrentPeb()->ProcessParameters->EnvironmentSize =
            RtlpEnvironmentSize(NtCurrentPeb()->ProcessParameters->Environment);
        RtlReleasePebLock();
    }

    return(Status);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlSetEnvironmentVariable(
    _In_z_ PWSTR *Environment,
    _In_ PUNICODE_STRING Name,
    _In_ PUNICODE_STRING Value)
{
    return RtlSetEnvironmentVar(Environment,
                                Name->Buffer,
                                Name->Length / sizeof(WCHAR),
                                (Value && Value->Buffer) ? Value->Buffer : NULL,
                                Value ? Value->Length / sizeof(WCHAR) : 0);
}


/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlQueryEnvironmentVariable(
    _In_opt_ PVOID Environment,
    _In_reads_(NameLength) PWSTR Name,
    _In_ SIZE_T NameLength,
    _Out_writes_(ValueLength) PWSTR Value,
    _In_ SIZE_T ValueLength,
    _Out_ PSIZE_T ReturnLength)
{
    NTSTATUS Status;
    PWSTR wcs;
    PWSTR varName;
    SIZE_T varNameLen;
    PWSTR val;
    BOOLEAN SysEnvUsed = FALSE;

    DPRINT("RtlQueryEnvironmentVariable_U Environment %p Variable %wZ Value %p\n",
           Environment, Name, Value);

    if (Environment == NULL)
    {
        PPEB Peb = RtlGetCurrentPeb();
        if (Peb)
        {
            RtlAcquirePebLock();
            Environment = Peb->ProcessParameters->Environment;
            SysEnvUsed = TRUE;
        }
    }

    *ReturnLength = 0;

    if (Environment == NULL)
    {
        if (SysEnvUsed)
            RtlReleasePebLock();
        return STATUS_VARIABLE_NOT_FOUND;
    }

    wcs = Environment;
    DPRINT("Starting search at :%p\n", wcs);
    while (*wcs)
    {
        varName = wcs++;
        wcs = wcschr(wcs, L'=');
        if (wcs == NULL)
        {
            wcs = varName + wcslen(varName);
            DPRINT("Search at :%S\n", wcs);
        }
        if (*wcs)
        {
            varNameLen = (wcs - varName);
            val = ++wcs;
            wcs += wcslen(wcs);
            DPRINT("Search at :%S\n", wcs);

            if ((varNameLen == NameLength) && _wcsnicmp(varName, Name, NameLength) == 0)
            {
                *ReturnLength = (wcs - val);
                if (*ReturnLength < ValueLength)
                {
                    memcpy(Value, val, (*ReturnLength + 1) * sizeof(WCHAR));
                    DPRINT("Value %S\n", val);
                    DPRINT("Return STATUS_SUCCESS\n");
                    Status = STATUS_SUCCESS;
                }
                else
                {
                    if (ValueLength > 0)
                        Value[0] = UNICODE_NULL;
                    DPRINT("Return STATUS_BUFFER_TOO_SMALL\n");
                    *ReturnLength += 1;
                    Status = STATUS_BUFFER_TOO_SMALL;
                }

                if (SysEnvUsed)
                    RtlReleasePebLock();

                return Status;
            }
        }

        wcs++;
    }

    if (SysEnvUsed)
        RtlReleasePebLock();

    DPRINT("Return STATUS_VARIABLE_NOT_FOUND: %wZ\n", Name);
    return STATUS_VARIABLE_NOT_FOUND;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
RtlQueryEnvironmentVariable_U(
    _In_opt_ PWSTR Environment,
    _In_ PCUNICODE_STRING Name,
    _Out_ PUNICODE_STRING Value)
{
    NTSTATUS Status;
    SIZE_T ReturnLength;

    Status = RtlQueryEnvironmentVariable(Environment,
                                         Name->Buffer,
                                         Name->Length / sizeof(WCHAR),
                                         Value->Buffer,
                                         Value->MaximumLength / sizeof(WCHAR),
                                         &ReturnLength);

    if (ReturnLength > UNICODE_STRING_MAX_CHARS)
    {
        return STATUS_NO_MEMORY;
    }

    /* Correct for extra length returned by RtlQueryEnvironmentVariable */
    if (Status == STATUS_BUFFER_TOO_SMALL)
    {
        ReturnLength -= 1;
    }

    Value->Length = (USHORT)ReturnLength * sizeof(WCHAR);
    return Status;
}

/* EOF */
