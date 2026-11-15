/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Windows Notification Facility (WNF)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define TAG_WNF_INSTANCE 'IfnW'
#define TAG_WNF_DATA     'DfnW'
#define TAG_WNF_SUB      'SfnW'

#define WNF_NAME_BUCKET_COUNT 64

typedef struct _WNF_NAME_BUCKET
{
    EX_PUSH_LOCK Lock;
    LIST_ENTRY ListHead;
} WNF_NAME_BUCKET, *PWNF_NAME_BUCKET;

static WNF_NAME_BUCKET ExpWnfNameBuckets[WNF_NAME_BUCKET_COUNT];
static LONG64 ExpWnfNextUnique;
static LONG64 ExpWnfNextSubscriptionId;
static BOOLEAN ExpWnfInitialized;
static PSECURITY_DESCRIPTOR ExpWnfDefaultSecurityDescriptor;

extern PEPROCESS PsInitialSystemProcess;

#define WNF_QUERY_ACCESS  0x0001
#define WNF_UPDATE_ACCESS 0x0002
#define WNF_MAX_TEMPORARY_NAMES_PER_PROCESS 1024
#define WNF_MAX_SUBSCRIPTIONS_PER_PROCESS   1024

static GENERIC_MAPPING ExpWnfGenericMapping =
{
    WNF_QUERY_ACCESS,
    WNF_UPDATE_ACCESS,
    WNF_QUERY_ACCESS,
    WNF_QUERY_ACCESS | WNF_UPDATE_ACCESS | DELETE
};

/* PRIVATE FUNCTIONS *********************************************************/

CODE_SEG("INIT")
BOOLEAN
NTAPI
ExpWnfInitialization(VOID)
{
    ULONG Index;
    PACL Acl;
    ULONG AclSize;
    PSECURITY_DESCRIPTOR SecurityDescriptor;

    for (Index = 0; Index < WNF_NAME_BUCKET_COUNT; Index++)
    {
        ExInitializePushLock(&ExpWnfNameBuckets[Index].Lock);
        InitializeListHead(&ExpWnfNameBuckets[Index].ListHead);
    }

    AclSize = sizeof(ACL) +
              3 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG)) +
              RtlLengthSid(SeWorldSid) +
              RtlLengthSid(SeLocalSystemSid) +
              RtlLengthSid(SeAliasAdminsSid);
    Acl = ExAllocatePoolWithTag(PagedPool, AclSize, TAG_WNF_INSTANCE);
    SecurityDescriptor = ExAllocatePoolWithTag(PagedPool,
                                               sizeof(SECURITY_DESCRIPTOR),
                                               TAG_WNF_INSTANCE);
    if ((Acl != NULL) && (SecurityDescriptor != NULL))
    {
        if (NT_SUCCESS(RtlCreateAcl(Acl, AclSize, ACL_REVISION)) &&
            NT_SUCCESS(RtlAddAccessAllowedAce(Acl, ACL_REVISION, WNF_QUERY_ACCESS, SeWorldSid)) &&
            NT_SUCCESS(RtlAddAccessAllowedAce(Acl,
                                              ACL_REVISION,
                                              WNF_QUERY_ACCESS | WNF_UPDATE_ACCESS | DELETE,
                                              SeLocalSystemSid)) &&
            NT_SUCCESS(RtlAddAccessAllowedAce(Acl,
                                              ACL_REVISION,
                                              WNF_QUERY_ACCESS | WNF_UPDATE_ACCESS | DELETE,
                                              SeAliasAdminsSid)) &&
            NT_SUCCESS(RtlCreateSecurityDescriptor(SecurityDescriptor,
                                                   SECURITY_DESCRIPTOR_REVISION)) &&
            NT_SUCCESS(RtlSetOwnerSecurityDescriptor(SecurityDescriptor, SeLocalSystemSid, FALSE)) &&
            NT_SUCCESS(RtlSetGroupSecurityDescriptor(SecurityDescriptor, SeAliasAdminsSid, FALSE)) &&
            NT_SUCCESS(RtlSetDaclSecurityDescriptor(SecurityDescriptor, TRUE, Acl, FALSE)))
        {
            ExpWnfDefaultSecurityDescriptor = SecurityDescriptor;
        }
        else
        {
            ExFreePoolWithTag(Acl, TAG_WNF_INSTANCE);
            ExFreePoolWithTag(SecurityDescriptor, TAG_WNF_INSTANCE);
        }
    }
    else
    {
        if (Acl != NULL)
            ExFreePoolWithTag(Acl, TAG_WNF_INSTANCE);
        if (SecurityDescriptor != NULL)
            ExFreePoolWithTag(SecurityDescriptor, TAG_WNF_INSTANCE);
    }

    ExpWnfInitialized = TRUE;
    return TRUE;
}

static
ULONG64
ExpWnfEncodeStateName(ULONG64 InternalName)
{
    return InternalName ^ WNF_STATE_KEY;
}

static
VOID
ExpWnfEncodeToStateName(ULONG64 InternalName,
                        PWNF_STATE_NAME StateName)
{
    ULONG64 Encoded = ExpWnfEncodeStateName(InternalName);

    StateName->Data[0] = (ULONG)Encoded;
    StateName->Data[1] = (ULONG)(Encoded >> 32);
}

static
NTSTATUS
ExpWnfCaptureStateName(PCWNF_STATE_NAME StateName,
                       KPROCESSOR_MODE PreviousMode,
                       PWNF_STATE_NAME_INTERNAL InternalName)
{
    WNF_STATE_NAME CapturedName = {0};

    if (!ExpWnfInitialized)
        return STATUS_UNSUCCESSFUL;

    if (StateName == NULL)
        return STATUS_INVALID_PARAMETER;

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForRead(StateName, sizeof(*StateName), sizeof(ULONG));
            CapturedName = *StateName;
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }
    else
    {
        CapturedName = *StateName;
    }

    /* The XOR encoding is its own inverse */
    InternalName->Value = ExpWnfEncodeStateName(((ULONG64)CapturedName.Data[1] << 32) |
                                                CapturedName.Data[0]);

    if (InternalName->Version != WNF_STATE_NAME_VERSION)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

static
NTSTATUS
ExpWnfCaptureTypeId(PCWNF_TYPE_ID TypeId,
                    KPROCESSOR_MODE PreviousMode,
                    PWNF_TYPE_ID CapturedTypeId,
                    PBOOLEAN HasTypeId)
{
    *HasTypeId = FALSE;

    if (TypeId == NULL)
        return STATUS_SUCCESS;

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForRead(TypeId, sizeof(*TypeId), sizeof(ULONG));
            *CapturedTypeId = *TypeId;
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }
    else
    {
        *CapturedTypeId = *TypeId;
    }

    *HasTypeId = TRUE;
    return STATUS_SUCCESS;
}

static
PWNF_NAME_BUCKET
ExpWnfNameBucket(ULONG64 InternalName)
{
    ULONG Hash = (ULONG)(InternalName ^ (InternalName >> 32)) * 0x9E3779B1;

    return &ExpWnfNameBuckets[Hash >> 26];
}

static
VOID
ExpWnfLockBucketShared(PWNF_NAME_BUCKET Bucket)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Bucket->Lock);
}

static
VOID
ExpWnfUnlockBucketShared(PWNF_NAME_BUCKET Bucket)
{
    ExReleasePushLockShared(&Bucket->Lock);
    KeLeaveCriticalRegion();
}

static
VOID
ExpWnfLockBucketExclusive(PWNF_NAME_BUCKET Bucket)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Bucket->Lock);
}

static
VOID
ExpWnfUnlockBucketExclusive(PWNF_NAME_BUCKET Bucket)
{
    ExReleasePushLockExclusive(&Bucket->Lock);
    KeLeaveCriticalRegion();
}

static
VOID
ExpWnfLockShared(PEX_PUSH_LOCK Lock)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(Lock);
}

static
VOID
ExpWnfUnlockShared(PEX_PUSH_LOCK Lock)
{
    ExReleasePushLockShared(Lock);
    KeLeaveCriticalRegion();
}

static
VOID
ExpWnfLockExclusive(PEX_PUSH_LOCK Lock)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(Lock);
}

static
VOID
ExpWnfUnlockExclusive(PEX_PUSH_LOCK Lock)
{
    ExReleasePushLockExclusive(Lock);
    KeLeaveCriticalRegion();
}

static
PWNF_NAME_INSTANCE
ExpWnfFindNameInstance(PWNF_NAME_BUCKET Bucket,
                       ULONG64 InternalName)
{
    PLIST_ENTRY Entry;
    PWNF_NAME_INSTANCE Instance;

    for (Entry = Bucket->ListHead.Flink;
         Entry != &Bucket->ListHead;
         Entry = Entry->Flink)
    {
        Instance = CONTAINING_RECORD(Entry, WNF_NAME_INSTANCE, NameListEntry);
        if (Instance->StateName.Value == InternalName)
            return Instance;
    }

    return NULL;
}

static
ULONG
ExpWnfCountProcessTemporaryNames(PEPROCESS Process)
{
    ULONG Index, Count;
    PWNF_NAME_BUCKET Bucket;
    PLIST_ENTRY Entry;
    PWNF_NAME_INSTANCE Instance;

    Count = 0;
    for (Index = 0; Index < WNF_NAME_BUCKET_COUNT; Index++)
    {
        Bucket = &ExpWnfNameBuckets[Index];
        ExpWnfLockBucketShared(Bucket);
        for (Entry = Bucket->ListHead.Flink;
             Entry != &Bucket->ListHead;
             Entry = Entry->Flink)
        {
            Instance = CONTAINING_RECORD(Entry, WNF_NAME_INSTANCE, NameListEntry);
            if ((Instance->Lifetime == WnfTemporaryStateName) &&
                (Instance->CreatorProcess == Process))
            {
                Count++;
            }
        }
        ExpWnfUnlockBucketShared(Bucket);
    }
    return Count;
}

static
ULONG
ExpWnfCountProcessSubscriptions(PEPROCESS Process)
{
    ULONG Index, Count;
    PWNF_NAME_BUCKET Bucket;
    PLIST_ENTRY Entry, SubEntry;
    PWNF_NAME_INSTANCE Instance;
    PWNF_SUBSCRIPTION Subscription;

    Count = 0;
    for (Index = 0; Index < WNF_NAME_BUCKET_COUNT; Index++)
    {
        Bucket = &ExpWnfNameBuckets[Index];
        ExpWnfLockBucketShared(Bucket);
        for (Entry = Bucket->ListHead.Flink;
             Entry != &Bucket->ListHead;
             Entry = Entry->Flink)
        {
            Instance = CONTAINING_RECORD(Entry, WNF_NAME_INSTANCE, NameListEntry);
            ExpWnfLockShared(&Instance->SubscriptionListLock);
            for (SubEntry = Instance->SubscriptionListHead.Flink;
                 SubEntry != &Instance->SubscriptionListHead;
                 SubEntry = SubEntry->Flink)
            {
                Subscription = CONTAINING_RECORD(SubEntry,
                                                 WNF_SUBSCRIPTION,
                                                 SubscriptionListEntry);
                if (Subscription->Process == Process)
                    Count++;
            }
            ExpWnfUnlockShared(&Instance->SubscriptionListLock);
        }
        ExpWnfUnlockBucketShared(Bucket);
    }
    return Count;
}

static
PWNF_NAME_INSTANCE
ExpWnfReferenceNameInstance(ULONG64 InternalName)
{
    PWNF_NAME_BUCKET Bucket = ExpWnfNameBucket(InternalName);
    PWNF_NAME_INSTANCE Instance;

    ExpWnfLockBucketShared(Bucket);
    Instance = ExpWnfFindNameInstance(Bucket, InternalName);
    if ((Instance != NULL) && !ExAcquireRundownProtection(&Instance->RunRef))
        Instance = NULL;
    ExpWnfUnlockBucketShared(Bucket);

    return Instance;
}

static
VOID
ExpWnfDereferenceNameInstance(PWNF_NAME_INSTANCE Instance)
{
    ExReleaseRundownProtection(&Instance->RunRef);
}

static
PWNF_NAME_INSTANCE
ExpWnfAllocateNameInstance(PWNF_STATE_NAME_INTERNAL InternalName,
                           PCWNF_TYPE_ID TypeId,
                           ULONG MaximumStateSize,
                           PSECURITY_DESCRIPTOR CapturedSd,
                           BOOLEAN ChargeQuota)
{
    PWNF_NAME_INSTANCE Instance;

    if (ChargeQuota)
    {
        Instance = ExAllocatePoolWithQuotaTag(PagedPool | POOL_QUOTA_FAIL_INSTEAD_OF_RAISE,
                                              sizeof(*Instance),
                                              TAG_WNF_INSTANCE);
    }
    else
    {
        Instance = ExAllocatePoolWithTag(PagedPool, sizeof(*Instance), TAG_WNF_INSTANCE);
    }
    if (Instance == NULL)
        return NULL;

    RtlZeroMemory(Instance, sizeof(*Instance));
    ExInitializeRundownProtection(&Instance->RunRef);
    ExInitializePushLock(&Instance->StateDataLock);
    ExInitializePushLock(&Instance->SubscriptionListLock);
    Instance->StateName.Value = InternalName->Value;
    Instance->Lifetime = (WNF_STATE_NAME_LIFETIME)InternalName->NameLifetime;
    Instance->DataScope = (WNF_DATA_SCOPE)InternalName->DataScope;
    Instance->PersistData = (BOOLEAN)InternalName->PermanentData;
    if (TypeId != NULL)
    {
        Instance->HasTypeId = TRUE;
        Instance->TypeId = *TypeId;
    }
    Instance->MaximumStateSize = MaximumStateSize ? MaximumStateSize
                                                  : WNF_MAX_STATE_SIZE;
    Instance->SecurityDescriptor = CapturedSd;
    Instance->CreatorProcess = ChargeQuota ? PsGetCurrentProcess() : PsInitialSystemProcess;
    InitializeListHead(&Instance->SubscriptionListHead);
    return Instance;
}

static
PWNF_NAME_INSTANCE
ExpWnfInsertNameInstance(PWNF_NAME_INSTANCE Instance)
{
    PWNF_NAME_BUCKET Bucket = ExpWnfNameBucket(Instance->StateName.Value);
    PWNF_NAME_INSTANCE Existing;

    ExpWnfLockBucketExclusive(Bucket);
    Existing = ExpWnfFindNameInstance(Bucket, Instance->StateName.Value);
    if (Existing == NULL)
    {
        InsertTailList(&Bucket->ListHead, &Instance->NameListEntry);
        Existing = Instance;
    }
    if (!ExAcquireRundownProtection(&Existing->RunRef))
        Existing = NULL;
    ExpWnfUnlockBucketExclusive(Bucket);

    return Existing;
}

static
NTSTATUS
ExpWnfReferenceOrCreateNameInstance(PWNF_STATE_NAME_INTERNAL InternalName,
                                    KPROCESSOR_MODE AccessMode,
                                    PWNF_NAME_INSTANCE *OutInstance)
{
    PWNF_NAME_INSTANCE Instance;
    PWNF_NAME_INSTANCE Created;

    Instance = ExpWnfReferenceNameInstance(InternalName->Value);
    if (Instance != NULL)
    {
        *OutInstance = Instance;
        return STATUS_SUCCESS;
    }

    /* Well-known names would spring into existence on first use. That is
       for kernel callers only: there is no well-known table to validate a
       name from user mode against. */
    if (InternalName->NameLifetime != WnfWellKnownStateName)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (AccessMode != KernelMode)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    Created = ExpWnfAllocateNameInstance(InternalName,
                                         NULL,
                                         0,
                                         ExpWnfDefaultSecurityDescriptor,
                                         FALSE);
    if (Created == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Instance = ExpWnfInsertNameInstance(Created);
    if (Instance != Created)
        ExFreePoolWithTag(Created, TAG_WNF_INSTANCE);
    if (Instance == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *OutInstance = Instance;
    return STATUS_SUCCESS;
}

static
VOID
ExpWnfDestroyNameInstance(PWNF_NAME_INSTANCE Instance);

static
VOID
ExpWnfDetachSubscription(PWNF_SUBSCRIPTION Subscription,
                         PLIST_ENTRY ReapList);

static
VOID
ExpWnfReapSubscription(PWNF_SUBSCRIPTION Subscription);

VOID
NTAPI
ExpWnfCleanupProcess(IN PEPROCESS Process)
{
    ULONG Index;
    PWNF_NAME_BUCKET Bucket;
    PLIST_ENTRY Entry, NextEntry;
    PLIST_ENTRY SubEntry, NextSubEntry, SubListEntry;
    PWNF_NAME_INSTANCE Instance;
    PWNF_SUBSCRIPTION Subscription;
    LIST_ENTRY DeadNames, DeadSubs;
    BOOLEAN Removing;

    PAGED_CODE();

    InitializeListHead(&DeadNames);
    InitializeListHead(&DeadSubs);

    for (Index = 0; Index < WNF_NAME_BUCKET_COUNT; Index++)
    {
        Bucket = &ExpWnfNameBuckets[Index];
        ExpWnfLockBucketExclusive(Bucket);
        Entry = Bucket->ListHead.Flink;
        while (Entry != &Bucket->ListHead)
        {
            NextEntry = Entry->Flink;
            Instance = CONTAINING_RECORD(Entry, WNF_NAME_INSTANCE, NameListEntry);
            Removing = (Instance->Lifetime == WnfTemporaryStateName) &&
                       (Instance->CreatorProcess == Process);
            if (Removing)
            {
                /* DestroyNameInstance reaps its subscriptions wholesale. */
                RemoveEntryList(&Instance->NameListEntry);
                InsertTailList(&DeadNames, &Instance->NameListEntry);
            }
            else
            {
                /* Detach this process's subscriptions from surviving names. */
                ExpWnfLockExclusive(&Instance->SubscriptionListLock);
                SubEntry = Instance->SubscriptionListHead.Flink;
                while (SubEntry != &Instance->SubscriptionListHead)
                {
                    NextSubEntry = SubEntry->Flink;
                    Subscription = CONTAINING_RECORD(SubEntry,
                                                     WNF_SUBSCRIPTION,
                                                     SubscriptionListEntry);
                    if (Subscription->Process == Process)
                    {
                        ExpWnfDetachSubscription(Subscription, &DeadSubs);
                    }
                    SubEntry = NextSubEntry;
                }
                ExpWnfUnlockExclusive(&Instance->SubscriptionListLock);
            }
            Entry = NextEntry;
        }
        ExpWnfUnlockBucketExclusive(Bucket);
    }

    while (!IsListEmpty(&DeadSubs))
    {
        SubListEntry = RemoveHeadList(&DeadSubs);
        ExpWnfReapSubscription(CONTAINING_RECORD(SubListEntry,
                                                 WNF_SUBSCRIPTION,
                                                 SubscriptionListEntry));
    }

    while (!IsListEmpty(&DeadNames))
    {
        Entry = RemoveHeadList(&DeadNames);
        Instance = CONTAINING_RECORD(Entry, WNF_NAME_INSTANCE, NameListEntry);
        ExpWnfDestroyNameInstance(Instance);
    }
}

static
BOOLEAN
ExpWnfMatchTypeId(PWNF_NAME_INSTANCE Instance,
                  BOOLEAN HasTypeId,
                  PCWNF_TYPE_ID TypeId)
{
    if (!Instance->HasTypeId || !HasTypeId)
        return TRUE;

    return RtlCompareMemory(&Instance->TypeId, TypeId, sizeof(WNF_TYPE_ID)) ==
           sizeof(WNF_TYPE_ID);
}

static
NTSTATUS
ExpWnfCheckAccess(PWNF_NAME_INSTANCE Instance,
                  ACCESS_MASK DesiredAccess,
                  KPROCESSOR_MODE AccessMode)
{
    SECURITY_SUBJECT_CONTEXT SubjectContext;
    ACCESS_MASK GrantedAccess = 0;
    PPRIVILEGE_SET Privileges = NULL;
    NTSTATUS AccessStatus = STATUS_ACCESS_DENIED;
    BOOLEAN Granted;

    if (AccessMode == KernelMode)
        return STATUS_SUCCESS;

    if (Instance->SecurityDescriptor == NULL)
        return STATUS_ACCESS_DENIED;

    SeCaptureSubjectContext(&SubjectContext);
    SeLockSubjectContext(&SubjectContext);
    Granted = SeAccessCheck(Instance->SecurityDescriptor,
                            &SubjectContext,
                            TRUE,
                            DesiredAccess,
                            0,
                            &Privileges,
                            &ExpWnfGenericMapping,
                            AccessMode,
                            &GrantedAccess,
                            &AccessStatus);
    SeUnlockSubjectContext(&SubjectContext);
    SeReleaseSubjectContext(&SubjectContext);
    if (Privileges != NULL)
        SeFreePrivileges(Privileges);

    return Granted ? STATUS_SUCCESS : AccessStatus;
}


/* Release a stored or in-progress descriptor however it was built. */
static
VOID
ExpWnfReleaseSecurityDescriptor(PSECURITY_DESCRIPTOR SD)
{
    PISECURITY_DESCRIPTOR Sd;

    if (SD == NULL)
        return;

    Sd = (PISECURITY_DESCRIPTOR)SD;
    if (Sd->Control & SE_SELF_RELATIVE)
    {
        SeReleaseSecurityDescriptor(SD, KernelMode, TRUE);
    }
    else
    {
        if (Sd->Owner != NULL) ExFreePoolWithTag(Sd->Owner, TAG_WNF_INSTANCE);
        if (Sd->Group != NULL) ExFreePoolWithTag(Sd->Group, TAG_WNF_INSTANCE);
        if (Sd->Dacl != NULL) ExFreePoolWithTag(Sd->Dacl, TAG_WNF_INSTANCE);
        if (Sd->Sacl != NULL) ExFreePoolWithTag(Sd->Sacl, TAG_WNF_INSTANCE);
        ExFreePoolWithTag(Sd, TAG_WNF_INSTANCE);
    }
}

/* A caller-supplied descriptor may carry no owner or group; an
   owner-less descriptor must never reach SeAccessCheck. Complete it
   from the effective token like SeAssignSecurity does (owner = default
   owner, group = primary group), validating a supplied owner, and gate
   a supplied SACL on SeSecurityPrivilege. The result is an absolute
   descriptor whose pieces we own. */
static
NTSTATUS
ExpWnfCompleteCapturedSecurityDescriptor(PSECURITY_DESCRIPTOR *SdInOut,
                                         KPROCESSOR_MODE PreviousMode)
{
    PSECURITY_DESCRIPTOR CapturedSd = *SdInOut;
    PSECURITY_DESCRIPTOR NewSd = NULL;
    PSID NewOwner = NULL, NewGroup = NULL;
    PACL NewDacl = NULL, NewSacl = NULL;
    PSID Owner = NULL, Group = NULL;
    PACL Dacl = NULL, Sacl = NULL;
    BOOLEAN DaclPresent, SaclPresent, Defaulted;
    SECURITY_SUBJECT_CONTEXT SubjectContext;
    PACCESS_TOKEN Token;
    PTOKEN_OWNER TokenOwnerInfo = NULL;
    PTOKEN_PRIMARY_GROUP TokenGroupInfo = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    RtlGetOwnerSecurityDescriptor(CapturedSd, &Owner, &Defaulted);
    RtlGetGroupSecurityDescriptor(CapturedSd, &Group, &Defaulted);
    RtlGetDaclSecurityDescriptor(CapturedSd, &DaclPresent, &Dacl, &Defaulted);
    RtlGetSaclSecurityDescriptor(CapturedSd, &SaclPresent, &Sacl, &Defaulted);

    /* A supplied SACL requires SeSecurityPrivilege, like Windows. */
    if (SaclPresent && (Sacl != NULL) &&
        !SeSinglePrivilegeCheck(SeSecurityPrivilege, PreviousMode))
    {
        return STATUS_PRIVILEGE_NOT_HELD;
    }

    /* A supplied owner is validated however complete the descriptor is. */
    SeCaptureSubjectContext(&SubjectContext);
    SeLockSubjectContext(&SubjectContext);
    Token = SeQuerySubjectContextToken(&SubjectContext);

    if (Owner != NULL)
    {
        if (!SeSinglePrivilegeCheck(SeRestorePrivilege, PreviousMode))
        {
            PTOKEN_USER UserInfo;
            BOOLEAN ValidOwner = FALSE;

            Status = SeQueryInformationToken(Token, TokenUser, (PVOID*)&UserInfo);
            if (NT_SUCCESS(Status))
            {
                ValidOwner = RtlEqualSid(Owner, UserInfo->User.Sid);
                ExFreePool(UserInfo);
            }
            if (NT_SUCCESS(Status) && !ValidOwner)
            {
                PTOKEN_GROUPS GroupsInfo;

                Status = SeQueryInformationToken(Token, TokenGroups, (PVOID*)&GroupsInfo);
                if (NT_SUCCESS(Status))
                {
                    ULONG Index;

                    for (Index = 0; Index < GroupsInfo->GroupCount; Index++)
                    {
                        if ((GroupsInfo->Groups[Index].Attributes & SE_GROUP_OWNER) &&
                            RtlEqualSid(Owner, GroupsInfo->Groups[Index].Sid))
                        {
                            ValidOwner = TRUE;
                            break;
                        }
                    }
                    ExFreePool(GroupsInfo);
                }
            }
            if (!NT_SUCCESS(Status) || !ValidOwner)
            {
                SeUnlockSubjectContext(&SubjectContext);
                SeReleaseSubjectContext(&SubjectContext);
                if (NT_SUCCESS(Status))
                    Status = STATUS_INVALID_OWNER;
                return Status;
            }
        }
    }

    if ((Owner != NULL) && (Group != NULL))
    {
        SeUnlockSubjectContext(&SubjectContext);
        SeReleaseSubjectContext(&SubjectContext);
        return STATUS_SUCCESS;
    }

    if (Owner == NULL)
    {
        Status = SeQueryInformationToken(Token, TokenOwner, (PVOID*)&TokenOwnerInfo);
        if (NT_SUCCESS(Status))
            Owner = TokenOwnerInfo->Owner;
    }
    if (NT_SUCCESS(Status) && (Group == NULL))
    {
        Status = SeQueryInformationToken(Token, TokenPrimaryGroup, (PVOID*)&TokenGroupInfo);
        if (NT_SUCCESS(Status))
            Group = (PSID)TokenGroupInfo->PrimaryGroup;
    }

    SeUnlockSubjectContext(&SubjectContext);
    SeReleaseSubjectContext(&SubjectContext);

    if (NT_SUCCESS(Status))
    {
        if (Owner != NULL)
        {
            NewOwner = ExAllocatePoolWithTag(PagedPool, RtlLengthSid(Owner), TAG_WNF_INSTANCE);
            if (NewOwner == NULL)
                Status = STATUS_INSUFFICIENT_RESOURCES;
            else
                RtlCopySid(RtlLengthSid(Owner), NewOwner, Owner);
        }
        if (NT_SUCCESS(Status) && (Group != NULL))
        {
            NewGroup = ExAllocatePoolWithTag(PagedPool, RtlLengthSid(Group), TAG_WNF_INSTANCE);
            if (NewGroup == NULL)
                Status = STATUS_INSUFFICIENT_RESOURCES;
            else
                RtlCopySid(RtlLengthSid(Group), NewGroup, Group);
        }
        if (NT_SUCCESS(Status) && DaclPresent && (Dacl != NULL))
        {
            NewDacl = ExAllocatePoolWithTag(PagedPool, Dacl->AclSize, TAG_WNF_INSTANCE);
            if (NewDacl == NULL)
                Status = STATUS_INSUFFICIENT_RESOURCES;
            else
                RtlCopyMemory(NewDacl, Dacl, Dacl->AclSize);
        }
        if (NT_SUCCESS(Status) && SaclPresent && (Sacl != NULL) &&
            SeSinglePrivilegeCheck(SeSecurityPrivilege, PreviousMode))
        {
            NewSacl = ExAllocatePoolWithTag(PagedPool, Sacl->AclSize, TAG_WNF_INSTANCE);
            if (NewSacl == NULL)
                Status = STATUS_INSUFFICIENT_RESOURCES;
            else
                RtlCopyMemory(NewSacl, Sacl, Sacl->AclSize);
        }
    }

    if (TokenOwnerInfo != NULL)
        ExFreePool(TokenOwnerInfo);
    if (TokenGroupInfo != NULL)
        ExFreePool(TokenGroupInfo);

    if (!NT_SUCCESS(Status))
    {
        if (NewOwner != NULL) ExFreePoolWithTag(NewOwner, TAG_WNF_INSTANCE);
        if (NewGroup != NULL) ExFreePoolWithTag(NewGroup, TAG_WNF_INSTANCE);
        if (NewDacl != NULL) ExFreePoolWithTag(NewDacl, TAG_WNF_INSTANCE);
        if (NewSacl != NULL) ExFreePoolWithTag(NewSacl, TAG_WNF_INSTANCE);
        return Status;
    }

    NewSd = ExAllocatePoolWithTag(PagedPool, sizeof(SECURITY_DESCRIPTOR), TAG_WNF_INSTANCE);
    if (NewSd == NULL)
    {
        if (NewOwner != NULL) ExFreePoolWithTag(NewOwner, TAG_WNF_INSTANCE);
        if (NewGroup != NULL) ExFreePoolWithTag(NewGroup, TAG_WNF_INSTANCE);
        if (NewDacl != NULL) ExFreePoolWithTag(NewDacl, TAG_WNF_INSTANCE);
        if (NewSacl != NULL) ExFreePoolWithTag(NewSacl, TAG_WNF_INSTANCE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCreateSecurityDescriptor(NewSd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetDaclSecurityDescriptor(NewSd, DaclPresent, NewDacl, FALSE);
    RtlSetSaclSecurityDescriptor(NewSd, (NewSacl != NULL), NewSacl, FALSE);
    RtlSetOwnerSecurityDescriptor(NewSd, NewOwner, FALSE);
    RtlSetGroupSecurityDescriptor(NewSd, NewGroup, FALSE);

    SeReleaseSecurityDescriptor(CapturedSd, KernelMode, TRUE);
    *SdInOut = NewSd;
    return STATUS_SUCCESS;
}

static
VOID
ExpWnfMapAclGenericAccess(PACL Acl)
{
    ULONG Index;
    PACE_HEADER Ace;

    if (Acl == NULL)
        return;

    for (Index = 0; Index < Acl->AceCount; Index++)
    {
        if (!NT_SUCCESS(RtlGetAce(Acl, Index, (PVOID*)&Ace)))
            break;
        if (Ace->AceFlags & INHERIT_ONLY_ACE)
            continue;
        if (Ace->AceSize < FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart))
            continue;

        switch (Ace->AceType)
        {
            case ACCESS_ALLOWED_ACE_TYPE:
            case ACCESS_DENIED_ACE_TYPE:
            case SYSTEM_AUDIT_ACE_TYPE:
            case SYSTEM_ALARM_ACE_TYPE:
            case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
            case ACCESS_DENIED_OBJECT_ACE_TYPE:
            case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
            case SYSTEM_ALARM_OBJECT_ACE_TYPE:
                RtlMapGenericMask(&((PACCESS_ALLOWED_ACE)Ace)->Mask,
                                  &ExpWnfGenericMapping);
                break;
        }
    }
}

static
VOID
ExpWnfDereferenceSubscription(PWNF_SUBSCRIPTION Subscription)
{
    if (InterlockedDecrement(&Subscription->RefCount) == 0)
        KeSetEvent(&Subscription->RundownEvent, IO_NO_INCREMENT, FALSE);
}

static
VOID
ExpWnfReapSubscription(PWNF_SUBSCRIPTION Subscription)
{
    ExpWnfDereferenceSubscription(Subscription);
    KeWaitForSingleObject(&Subscription->RundownEvent,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);
    ExFreePoolWithTag(Subscription, TAG_WNF_SUB);
}

static
VOID
ExpWnfReapSubscriptionList(PLIST_ENTRY ReapList)
{
    PLIST_ENTRY Entry;

    while (!IsListEmpty(ReapList))
    {
        Entry = RemoveHeadList(ReapList);
        ExpWnfReapSubscription(CONTAINING_RECORD(Entry,
                                                 WNF_SUBSCRIPTION,
                                                 SubscriptionListEntry));
    }
}

static
VOID
NTAPI
ExpWnfCallbackWorker(PVOID Parameter)
{
    PWNF_SUBSCRIPTION Subscription = Parameter;
    PWNF_NAME_INSTANCE Instance = Subscription->NameInstance;
    WNF_STATE_NAME EncodedName;
    WNF_TYPE_ID TypeId = { { 0 } };
    PVOID Data = NULL;
    ULONG DataSize = 0;
    WNF_CHANGE_STAMP ChangeStamp = 0;
    BOOLEAN HasTypeId = FALSE;
    BOOLEAN Deliver = FALSE;

    InterlockedExchange(&Subscription->WorkQueued, 0);
    if (!InterlockedCompareExchange(&Subscription->Removed, 0, 0))
    {
        ExpWnfLockShared(&Instance->StateDataLock);
        DataSize = Instance->DataSize;
        if (DataSize != 0)
        {
            Data = ExAllocatePoolWithTag(PagedPool, DataSize, TAG_WNF_DATA);
            if (Data != NULL)
                RtlCopyMemory(Data, Instance->DataBuffer, DataSize);
            else
                DataSize = 0;
        }
        ChangeStamp = Instance->ChangeStamp;
        HasTypeId = Instance->HasTypeId;
        TypeId = Instance->TypeId;
        ExpWnfUnlockShared(&Instance->StateDataLock);
        ExpWnfEncodeToStateName(Instance->StateName.Value, &EncodedName);
        Deliver = TRUE;
    }

    if (Deliver)
    {
        Subscription->Callback(&EncodedName,
                               ChangeStamp,
                               HasTypeId ? &TypeId : NULL,
                               Subscription->CallbackContext,
                               Data,
                               DataSize);
        Subscription->ChangeStamp = ChangeStamp;
    }

    if (Data != NULL)
        ExFreePoolWithTag(Data, TAG_WNF_DATA);

    InterlockedDecrement(&Instance->DeliveriesInFlight);
    ExpWnfDereferenceNameInstance(Instance);
    ExpWnfDereferenceSubscription(Subscription);
}

static
VOID
ExpWnfSignalSubscription(PWNF_SUBSCRIPTION Subscription)
{
    PWNF_NAME_INSTANCE Instance = Subscription->NameInstance;

    KeSetEvent(&Subscription->NotificationEvent, IO_NO_INCREMENT, FALSE);

    if ((Subscription->Callback != NULL) &&
        (InterlockedCompareExchange(&Subscription->WorkQueued, 1, 0) == 0))
    {
        if (!ExAcquireRundownProtection(&Instance->RunRef))
        {
            InterlockedExchange(&Subscription->WorkQueued, 0);
            return;
        }
        InterlockedIncrement(&Subscription->RefCount);
        InterlockedIncrement(&Instance->DeliveriesInFlight);
        ExQueueWorkItem(&Subscription->WorkItem, DelayedWorkQueue);
    }
}

static
VOID
ExpWnfSignalSubscriptions(PWNF_NAME_INSTANCE Instance)
{
    PLIST_ENTRY Entry;

    ExpWnfLockShared(&Instance->SubscriptionListLock);
    for (Entry = Instance->SubscriptionListHead.Flink;
         Entry != &Instance->SubscriptionListHead;
         Entry = Entry->Flink)
    {
        ExpWnfSignalSubscription(CONTAINING_RECORD(Entry,
                                                   WNF_SUBSCRIPTION,
                                                   SubscriptionListEntry));
    }
    ExpWnfUnlockShared(&Instance->SubscriptionListLock);
}

static
PWNF_SUBSCRIPTION
ExpWnfAllocateSubscription(WNF_CHANGE_STAMP ChangeStamp,
                           ULONG EventMask,
                           PWNF_KERNEL_CALLBACK Callback,
                           PVOID CallbackContext)
{
    PWNF_SUBSCRIPTION Subscription;

    Subscription = ExAllocatePoolWithTag(NonPagedPool,
                                         sizeof(*Subscription),
                                         TAG_WNF_SUB);
    if (Subscription == NULL)
        return NULL;

    RtlZeroMemory(Subscription, sizeof(*Subscription));
    KeInitializeEvent(&Subscription->NotificationEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Subscription->RundownEvent, NotificationEvent, FALSE);
    Subscription->RefCount = 1;
    Subscription->ChangeStamp = ChangeStamp;
    Subscription->EventMask = EventMask;
    Subscription->Callback = Callback;
    Subscription->CallbackContext = CallbackContext;
    ExInitializeWorkItem(&Subscription->WorkItem,
                         ExpWnfCallbackWorker,
                         Subscription);
    return Subscription;
}

static
VOID
ExpWnfDetachSubscription(PWNF_SUBSCRIPTION Subscription,
                         PLIST_ENTRY ReapList)
{
    RemoveEntryList(&Subscription->SubscriptionListEntry);
    InterlockedExchange(&Subscription->Removed, TRUE);
    InterlockedDecrement(&Subscription->NameInstance->SubscriptionCount);
    if (ReapList != NULL)
        InsertTailList(ReapList, &Subscription->SubscriptionListEntry);
    else
        InitializeListHead(&Subscription->SubscriptionListEntry);
    ExpWnfDereferenceSubscription(Subscription);
}

static
VOID
ExpWnfAttachSubscription(PWNF_NAME_INSTANCE Instance,
                         PWNF_SUBSCRIPTION Subscription,
                         PWNF_STATE_NAME_INTERNAL InternalName)
{
    Subscription->NameInstance = Instance;
    Subscription->StateName.Value = InternalName->Value;
    Subscription->SubscriptionId = InterlockedIncrement64(&ExpWnfNextSubscriptionId);
    InterlockedIncrement(&Subscription->RefCount);

    ExpWnfLockExclusive(&Instance->SubscriptionListLock);
    InsertTailList(&Instance->SubscriptionListHead,
                   &Subscription->SubscriptionListEntry);
    InterlockedIncrement(&Instance->SubscriptionCount);
    ExpWnfUnlockExclusive(&Instance->SubscriptionListLock);
}

static
VOID
ExpWnfFreeNameInstance(PWNF_NAME_INSTANCE Instance)
{
    ASSERT(IsListEmpty(&Instance->SubscriptionListHead));

    if (Instance->DataBuffer != NULL)
        ExFreePoolWithTag(Instance->DataBuffer, TAG_WNF_DATA);

    if ((Instance->SecurityDescriptor != NULL) &&
        (Instance->SecurityDescriptor != ExpWnfDefaultSecurityDescriptor))
    {
        ExpWnfReleaseSecurityDescriptor(Instance->SecurityDescriptor);
    }

    ExFreePoolWithTag(Instance, TAG_WNF_INSTANCE);
}

static
VOID
ExpWnfDestroyNameInstance(PWNF_NAME_INSTANCE Instance)
{
    PWNF_SUBSCRIPTION Subscription;
    LIST_ENTRY ReapList;

    InitializeListHead(&ReapList);
    ExWaitForRundownProtectionRelease(&Instance->RunRef);

    ExpWnfLockExclusive(&Instance->SubscriptionListLock);
    while (!IsListEmpty(&Instance->SubscriptionListHead))
    {
        Subscription = CONTAINING_RECORD(Instance->SubscriptionListHead.Flink,
                                         WNF_SUBSCRIPTION,
                                         SubscriptionListEntry);
        KeSetEvent(&Subscription->NotificationEvent, IO_NO_INCREMENT, FALSE);
        ExpWnfDetachSubscription(Subscription,
                                 (Subscription->Callback == NULL) ? &ReapList : NULL);
    }
    ExpWnfUnlockExclusive(&Instance->SubscriptionListLock);

    ExpWnfReapSubscriptionList(&ReapList);
    ExpWnfFreeNameInstance(Instance);
}

static
NTSTATUS
ExpWnfQuerySnapshot(ULONG64 InternalName,
                    KPROCESSOR_MODE AccessMode,
                    BOOLEAN HasTypeId,
                    PCWNF_TYPE_ID TypeId,
                    PWNF_CHANGE_STAMP ChangeStamp,
                    PVOID *Data,
                    PULONG DataSize)
{
    PWNF_NAME_INSTANCE Instance;
    PVOID Snapshot = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    Instance = ExpWnfReferenceNameInstance(InternalName);
    if (Instance == NULL)
    {
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
    }
    else
    {
        Status = ExpWnfCheckAccess(Instance, WNF_QUERY_ACCESS, AccessMode);
        if (NT_SUCCESS(Status) && !ExpWnfMatchTypeId(Instance, HasTypeId, TypeId))
            Status = STATUS_INVALID_PARAMETER;

        if (NT_SUCCESS(Status))
        {
            ExpWnfLockShared(&Instance->StateDataLock);
            *ChangeStamp = Instance->ChangeStamp;
            *DataSize = Instance->DataSize;
            if (Instance->DataSize != 0)
            {
                Snapshot = ExAllocatePoolWithTag(PagedPool,
                                                 Instance->DataSize,
                                                 TAG_WNF_DATA);
                if (Snapshot != NULL)
                    RtlCopyMemory(Snapshot, Instance->DataBuffer, Instance->DataSize);
                else
                    Status = STATUS_INSUFFICIENT_RESOURCES;
            }
            ExpWnfUnlockShared(&Instance->StateDataLock);
        }

        ExpWnfDereferenceNameInstance(Instance);
    }

    *Data = Snapshot;
    return Status;
}

static
BOOLEAN
ExpWnfRemoveNameInstance(ULONG64 InternalName,
                         PWNF_NAME_INSTANCE Instance)
{
    PWNF_NAME_BUCKET Bucket = ExpWnfNameBucket(InternalName);
    BOOLEAN Removed = FALSE;

    ExpWnfLockBucketExclusive(Bucket);
    if (ExpWnfFindNameInstance(Bucket, InternalName) == Instance)
    {
        RemoveEntryList(&Instance->NameListEntry);
        Removed = TRUE;
    }
    ExpWnfUnlockBucketExclusive(Bucket);

    return Removed;
}

static
NTSTATUS
ExpWnfStoreStateData(PWNF_NAME_INSTANCE Instance,
                     PVOID *Data,
                     ULONG Length,
                     BOOLEAN HasTypeId,
                     PCWNF_TYPE_ID TypeId,
                     WNF_CHANGE_STAMP MatchingChangeStamp,
                     LOGICAL CheckStamp)
{
    PVOID OldData;

    if (Length > Instance->MaximumStateSize)
        return STATUS_BUFFER_OVERFLOW;

    if (!ExpWnfMatchTypeId(Instance, HasTypeId, TypeId))
        return STATUS_INVALID_PARAMETER;

    ExpWnfLockExclusive(&Instance->StateDataLock);
    if (CheckStamp && (Instance->ChangeStamp != MatchingChangeStamp))
    {
        ExpWnfUnlockExclusive(&Instance->StateDataLock);
        return STATUS_UNSUCCESSFUL;
    }

    OldData = Instance->DataBuffer;
    Instance->DataBuffer = *Data;
    Instance->DataSize = Length;
    Instance->ChangeStamp++;
    ExpWnfUnlockExclusive(&Instance->StateDataLock);

    *Data = OldData;
    ExpWnfSignalSubscriptions(Instance);
    return STATUS_SUCCESS;
}

static
WNF_CHANGE_STAMP
ExpWnfReadChangeStamp(PWNF_NAME_INSTANCE Instance)
{
    WNF_CHANGE_STAMP ChangeStamp;

    ExpWnfLockShared(&Instance->StateDataLock);
    ChangeStamp = Instance->ChangeStamp;
    ExpWnfUnlockShared(&Instance->StateDataLock);

    return ChangeStamp;
}

/* SYSTEM CALLS **************************************************************/

NTSTATUS
NTAPI
NtCreateWnfStateName(PWNF_STATE_NAME StateName,
                     WNF_STATE_NAME_LIFETIME NameLifetime,
                     WNF_DATA_SCOPE DataScope,
                     BOOLEAN PersistData,
                     PCWNF_TYPE_ID TypeId,
                     ULONG MaximumStateSize,
                     PSECURITY_DESCRIPTOR SecurityDescriptor)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    WNF_TYPE_ID CapturedTypeId;
    BOOLEAN HasTypeId;
    PSECURITY_DESCRIPTOR CapturedSd = NULL;
    PWNF_NAME_INSTANCE Instance;
    PWNF_NAME_INSTANCE Created;
    NTSTATUS Status;

    PAGED_CODE();

    if (!ExpWnfInitialized)
        return STATUS_UNSUCCESSFUL;

    if (StateName == NULL)
        return STATUS_INVALID_PARAMETER;

    if (SecurityDescriptor == NULL)
        return STATUS_ACCESS_VIOLATION;

    if ((NameLifetime != WnfPermanentStateName) &&
        (NameLifetime != WnfPersistentStateName) &&
        (NameLifetime != WnfTemporaryStateName))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (DataScope > WnfDataScopePhysicalMachine)
        return STATUS_INVALID_PARAMETER;

    if (MaximumStateSize > WNF_MAX_STATE_SIZE)
        return STATUS_INVALID_PARAMETER;

    if (PersistData && (NameLifetime == WnfTemporaryStateName))
        return STATUS_INVALID_PARAMETER;

    if ((NameLifetime != WnfTemporaryStateName) &&
        !SeSinglePrivilegeCheck(SeCreatePermanentPrivilege, PreviousMode))
    {
        return STATUS_PRIVILEGE_NOT_HELD;
    }

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWrite(StateName, sizeof(*StateName), sizeof(ULONG));
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Status = ExpWnfCaptureTypeId(TypeId, PreviousMode, &CapturedTypeId, &HasTypeId);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((NameLifetime == WnfTemporaryStateName) &&
        (PreviousMode != KernelMode) &&
        (ExpWnfCountProcessTemporaryNames(PsGetCurrentProcess()) >=
         WNF_MAX_TEMPORARY_NAMES_PER_PROCESS))
    {
        return STATUS_QUOTA_EXCEEDED;
    }

    if (SecurityDescriptor != NULL)
    {
        BOOLEAN DaclPresent;
        PACL Dacl;
        BOOLEAN DaclDefaulted;

        Status = SeCaptureSecurityDescriptor(SecurityDescriptor,
                                             PreviousMode,
                                             PagedPool,
                                             TRUE,
                                             &CapturedSd);
        if (!NT_SUCCESS(Status))
            return Status;

        /* Never let an owner-less descriptor reach the access check. */
        Status = ExpWnfCompleteCapturedSecurityDescriptor(&CapturedSd, PreviousMode);
        if (!NT_SUCCESS(Status))
        {
            ExpWnfReleaseSecurityDescriptor(CapturedSd);
            return Status;
        }

        /* Our SeAccessCheck consumes specific ACE masks: translate the
           caller's GENERIC_* masks once, at capture. */
        if ((CapturedSd != NULL) &&
            NT_SUCCESS(RtlGetDaclSecurityDescriptor(CapturedSd,
                                                    &DaclPresent,
                                                    &Dacl,
                                                    &DaclDefaulted)) &&
            DaclPresent && (Dacl != NULL) && RtlValidAcl(Dacl))
        {
            ExpWnfMapAclGenericAccess(Dacl);
        }
    }

    InternalName.Value = 0;
    InternalName.Version = WNF_STATE_NAME_VERSION;
    InternalName.NameLifetime = NameLifetime;
    InternalName.DataScope = DataScope;
    InternalName.PermanentData = PersistData ? 1 : 0;

    InternalName.Unique = InterlockedIncrement64(&ExpWnfNextUnique);
    Instance = ExpWnfAllocateNameInstance(&InternalName,
                                          HasTypeId ? &CapturedTypeId : NULL,
                                          MaximumStateSize,
                                          CapturedSd,
                                          (PreviousMode != KernelMode));
    if (Instance == NULL)
    {
        ExpWnfReleaseSecurityDescriptor(CapturedSd);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Created = ExpWnfInsertNameInstance(Instance);
    ASSERT(Created == Instance);
    ExpWnfDereferenceNameInstance(Created);

    _SEH2_TRY
    {
        ExpWnfEncodeToStateName(InternalName.Value, StateName);
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
        if (ExpWnfRemoveNameInstance(InternalName.Value, Instance))
            ExpWnfDestroyNameInstance(Instance);
        _SEH2_YIELD(return Status);
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtDeleteWnfStateName(PCWNF_STATE_NAME StateName)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    PWNF_NAME_INSTANCE Instance;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ExpWnfCaptureStateName(StateName, PreviousMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    Instance = ExpWnfReferenceNameInstance(InternalName.Value);
    if (Instance == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    Status = ExpWnfCheckAccess(Instance, DELETE, PreviousMode);
    if (NT_SUCCESS(Status) && !ExpWnfRemoveNameInstance(InternalName.Value, Instance))
        Status = STATUS_OBJECT_NAME_NOT_FOUND;

    ExpWnfDereferenceNameInstance(Instance);

    if (!NT_SUCCESS(Status))
        return Status;

    ExpWnfDestroyNameInstance(Instance);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtUpdateWnfStateData(PCWNF_STATE_NAME StateName,
                     const VOID *Buffer,
                     ULONG Length,
                     PCWNF_TYPE_ID TypeId,
                     const VOID *ExplicitScope,
                     WNF_CHANGE_STAMP MatchingChangeStamp,
                     LOGICAL CheckStamp)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    WNF_TYPE_ID CapturedTypeId;
    BOOLEAN HasTypeId;
    PWNF_NAME_INSTANCE Instance;
    PVOID NewData = NULL;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ExplicitScope);

    PAGED_CODE();

    Status = ExpWnfCaptureStateName(StateName, PreviousMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((Length != 0) && (Buffer == NULL))
        return STATUS_INVALID_PARAMETER;

    if (Length > WNF_MAX_STATE_SIZE)
        return STATUS_BUFFER_OVERFLOW;

    Status = ExpWnfCaptureTypeId(TypeId, PreviousMode, &CapturedTypeId, &HasTypeId);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Length != 0)
    {
        NewData = ExAllocatePoolWithTag(PagedPool, Length, TAG_WNF_DATA);
        if (NewData == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        _SEH2_TRY
        {
            if (PreviousMode != KernelMode)
                ProbeForRead(Buffer, Length, 1);
            RtlCopyMemory(NewData, Buffer, Length);
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            ExFreePoolWithTag(NewData, TAG_WNF_DATA);
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Status = ExpWnfReferenceOrCreateNameInstance(&InternalName,
                                                   PreviousMode,
                                                   &Instance);
    if (NT_SUCCESS(Status))
    {
        Status = ExpWnfCheckAccess(Instance, WNF_UPDATE_ACCESS, PreviousMode);
        if (NT_SUCCESS(Status))
        {
            Status = ExpWnfStoreStateData(Instance,
                                          &NewData,
                                          Length,
                                          HasTypeId,
                                          &CapturedTypeId,
                                          MatchingChangeStamp,
                                          CheckStamp);
        }
        ExpWnfDereferenceNameInstance(Instance);
    }

    if (NewData != NULL)
        ExFreePoolWithTag(NewData, TAG_WNF_DATA);

    return Status;
}

NTSTATUS
NTAPI
NtDeleteWnfStateData(PCWNF_STATE_NAME StateName,
                     const VOID *ExplicitScope)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    PWNF_NAME_INSTANCE Instance;
    PVOID OldData;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ExplicitScope);

    PAGED_CODE();

    Status = ExpWnfCaptureStateName(StateName, PreviousMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    Instance = ExpWnfReferenceNameInstance(InternalName.Value);
    if (Instance == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    Status = ExpWnfCheckAccess(Instance, WNF_UPDATE_ACCESS, PreviousMode);
    if (!NT_SUCCESS(Status))
    {
        ExpWnfDereferenceNameInstance(Instance);
        return Status;
    }

    ExpWnfLockExclusive(&Instance->StateDataLock);
    OldData = Instance->DataBuffer;
    Instance->DataBuffer = NULL;
    Instance->DataSize = 0;
    Instance->ChangeStamp = 0;
    ExpWnfUnlockExclusive(&Instance->StateDataLock);

    ExpWnfSignalSubscriptions(Instance);
    ExpWnfDereferenceNameInstance(Instance);

    if (OldData != NULL)
        ExFreePoolWithTag(OldData, TAG_WNF_DATA);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
NtQueryWnfStateData(PCWNF_STATE_NAME StateName,
                    PCWNF_TYPE_ID TypeId,
                    const VOID *ExplicitScope,
                    PWNF_CHANGE_STAMP ChangeStamp,
                    PVOID Buffer,
                    PULONG BufferSize)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    WNF_TYPE_ID CapturedTypeId;
    BOOLEAN HasTypeId;
    WNF_CHANGE_STAMP Stamp = 0;
    PVOID Snapshot = NULL;
    ULONG CallerSize = 0;
    ULONG Required = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ExplicitScope);

    PAGED_CODE();

    Status = ExpWnfCaptureStateName(StateName, PreviousMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((ChangeStamp == NULL) || (BufferSize == NULL))
        return STATUS_INVALID_PARAMETER;

    Status = ExpWnfCaptureTypeId(TypeId, PreviousMode, &CapturedTypeId, &HasTypeId);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        if (PreviousMode != KernelMode)
        {
            ProbeForWriteUlong((PULONG)ChangeStamp);
            ProbeForWriteUlong(BufferSize);
        }
        CallerSize = *BufferSize;
        if ((PreviousMode != KernelMode) && (Buffer != NULL) && (CallerSize != 0))
            ProbeForWrite(Buffer, CallerSize, 1);
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    if (Buffer == NULL)
        CallerSize = 0;

    Status = ExpWnfQuerySnapshot(InternalName.Value,
                                 PreviousMode,
                                 HasTypeId,
                                 &CapturedTypeId,
                                 &Stamp,
                                 &Snapshot,
                                 &Required);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *ChangeStamp = Stamp;
        *BufferSize = Required;
        if (CallerSize >= Required)
        {
            if (Required != 0)
                RtlCopyMemory(Buffer, Snapshot, Required);
            Status = STATUS_SUCCESS;
        }
        else
        {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (Snapshot != NULL)
        ExFreePoolWithTag(Snapshot, TAG_WNF_DATA);

    return Status;
}

NTSTATUS
NTAPI
NtQueryWnfStateNameInformation(PCWNF_STATE_NAME StateName,
                               ULONG NameInfoClass,
                               const VOID *ExplicitScope,
                               PVOID InfoBuffer,
                               ULONG InfoBufferSize)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    PWNF_NAME_INSTANCE Instance;
    ULONG Value = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(ExplicitScope);

    PAGED_CODE();

    Status = ExpWnfCaptureStateName(StateName, PreviousMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    if (NameInfoClass > WnfInfoIsQuiescent)
        return STATUS_INVALID_INFO_CLASS;

    if ((InfoBuffer == NULL) || (InfoBufferSize != sizeof(ULONG)))
        return STATUS_INVALID_PARAMETER;

    if (PreviousMode != KernelMode)
    {
        _SEH2_TRY
        {
            ProbeForWriteUlong(InfoBuffer);
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    Instance = ExpWnfReferenceNameInstance(InternalName.Value);
    switch (NameInfoClass)
    {
        case WnfInfoStateNameExist:
            Value = (Instance != NULL);
            Status = STATUS_SUCCESS;
            break;

        case WnfInfoSubscribersPresent:
            if (Instance == NULL)
            {
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
            }
            else
            {
                Value = (InterlockedCompareExchange(&Instance->SubscriptionCount, 0, 0) != 0);
                Status = STATUS_SUCCESS;
            }
            break;

        case WnfInfoIsQuiescent:
            if (Instance == NULL)
            {
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
            }
            else
            {
                Value = (InterlockedCompareExchange(&Instance->DeliveriesInFlight, 0, 0) == 0);
                Status = STATUS_SUCCESS;
            }
            break;
    }

    if (Instance != NULL)
        ExpWnfDereferenceNameInstance(Instance);

    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        *(PULONG)InfoBuffer = Value;
    }
    _SEH2_EXCEPT(ExSystemExceptionFilter())
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

NTSTATUS
NTAPI
NtSubscribeWnfStateChange(PCWNF_STATE_NAME StateName,
                          WNF_CHANGE_STAMP ChangeStamp,
                          ULONG EventMask,
                          PULONG64 SubscriptionId)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    PWNF_NAME_INSTANCE Instance;
    PWNF_SUBSCRIPTION Subscription;
    LIST_ENTRY ReapList;
    ULONG64 Id;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ExpWnfCaptureStateName(StateName, PreviousMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    if ((PreviousMode != KernelMode) && (SubscriptionId != NULL))
    {
        _SEH2_TRY
        {
            ProbeForWrite(SubscriptionId, sizeof(*SubscriptionId), sizeof(ULONG64));
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            _SEH2_YIELD(return _SEH2_GetExceptionCode());
        }
        _SEH2_END;
    }

    if ((PreviousMode != KernelMode) &&
        (ExpWnfCountProcessSubscriptions(PsGetCurrentProcess()) >=
         WNF_MAX_SUBSCRIPTIONS_PER_PROCESS))
    {
        return STATUS_QUOTA_EXCEEDED;
    }

    Subscription = ExpWnfAllocateSubscription(ChangeStamp, EventMask, NULL, NULL);
    if (Subscription == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Call-local reference: a concurrent unsubscribe must not free the
       subscription while this call is initialising it. */
    InterlockedIncrement(&Subscription->RefCount);

    Subscription->Process = PsGetCurrentProcess();
    Subscription->Thread = PsGetCurrentThread();

    Status = ExpWnfReferenceOrCreateNameInstance(&InternalName,
                                                   PreviousMode,
                                                   &Instance);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Subscription, TAG_WNF_SUB);
        return Status;
    }

    Status = ExpWnfCheckAccess(Instance, WNF_QUERY_ACCESS, PreviousMode);
    if (!NT_SUCCESS(Status))
    {
        ExpWnfDereferenceNameInstance(Instance);
        ExFreePoolWithTag(Subscription, TAG_WNF_SUB);
        return Status;
    }

    ExpWnfAttachSubscription(Instance, Subscription, &InternalName);
    Id = Subscription->SubscriptionId;

    if (ExpWnfReadChangeStamp(Instance) > ChangeStamp)
        KeSetEvent(&Subscription->NotificationEvent, IO_NO_INCREMENT, FALSE);

    if (SubscriptionId != NULL)
    {
        _SEH2_TRY
        {
            *SubscriptionId = Id;
        }
        _SEH2_EXCEPT(ExSystemExceptionFilter())
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    if (!NT_SUCCESS(Status))
    {
        InitializeListHead(&ReapList);
        ExpWnfLockExclusive(&Instance->SubscriptionListLock);
        if (!InterlockedCompareExchange(&Subscription->Removed, 0, 0))
        {
            ExpWnfDetachSubscription(Subscription, &ReapList);
            ExpWnfUnlockExclusive(&Instance->SubscriptionListLock);
            ExpWnfDereferenceSubscription(Subscription);
            ExpWnfReapSubscriptionList(&ReapList);
        }
        else
        {
            ExpWnfUnlockExclusive(&Instance->SubscriptionListLock);
            /* The other thread owns the reap and the free; only drop
               the call-local reference so its rundown wait completes. */
            ExpWnfDereferenceSubscription(Subscription);
        }
    }
    else
    {
        ExpWnfDereferenceSubscription(Subscription);
    }

    ExpWnfDereferenceNameInstance(Instance);
    return Status;
}

NTSTATUS
NTAPI
NtUnsubscribeWnfStateChange(PCWNF_STATE_NAME StateName)
{
    KPROCESSOR_MODE PreviousMode = ExGetPreviousMode();
    WNF_STATE_NAME_INTERNAL InternalName;
    PWNF_NAME_INSTANCE Instance;
    PWNF_SUBSCRIPTION Subscription;
    PEPROCESS Process = PsGetCurrentProcess();
    LIST_ENTRY ReapList;
    PLIST_ENTRY Entry, NextEntry;
    BOOLEAN Found = FALSE;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ExpWnfCaptureStateName(StateName, PreviousMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeListHead(&ReapList);

    Instance = ExpWnfReferenceNameInstance(InternalName.Value);
    if (Instance == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    ExpWnfLockExclusive(&Instance->SubscriptionListLock);
    for (Entry = Instance->SubscriptionListHead.Flink;
         Entry != &Instance->SubscriptionListHead;
         Entry = NextEntry)
    {
        NextEntry = Entry->Flink;
        Subscription = CONTAINING_RECORD(Entry,
                                         WNF_SUBSCRIPTION,
                                         SubscriptionListEntry);
        if ((Subscription->Process == Process) &&
            (Subscription->Callback == NULL))
        {
            ExpWnfDetachSubscription(Subscription, &ReapList);
            Found = TRUE;
        }
    }
    ExpWnfUnlockExclusive(&Instance->SubscriptionListLock);
    ExpWnfDereferenceNameInstance(Instance);

    ExpWnfReapSubscriptionList(&ReapList);

    return Found ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

/* KERNEL EXPORTS ************************************************************/

NTSTATUS
NTAPI
ExQueryWnfStateData(PCWNF_STATE_NAME StateName,
                    PWNF_CHANGE_STAMP ChangeStamp,
                    PVOID Buffer,
                    PULONG BufferSize)
{
    WNF_STATE_NAME_INTERNAL InternalName;
    WNF_CHANGE_STAMP Stamp = 0;
    PVOID Snapshot = NULL;
    ULONG Required = 0;
    NTSTATUS Status;

    PAGED_CODE();

    if ((ChangeStamp == NULL) || (BufferSize == NULL))
        return STATUS_INVALID_PARAMETER;

    Status = ExpWnfCaptureStateName(StateName, KernelMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ExpWnfQuerySnapshot(InternalName.Value,
                                 KernelMode,
                                 FALSE,
                                 NULL,
                                 &Stamp,
                                 &Snapshot,
                                 &Required);
    if (!NT_SUCCESS(Status))
        return Status;

    *ChangeStamp = Stamp;
    if (Required == 0)
    {
        Status = STATUS_SUCCESS;
    }
    else if ((Buffer != NULL) && (*BufferSize >= Required))
    {
        RtlCopyMemory(Buffer, Snapshot, Required);
        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_BUFFER_TOO_SMALL;
    }
    *BufferSize = Required;

    if (Snapshot != NULL)
        ExFreePoolWithTag(Snapshot, TAG_WNF_DATA);

    return Status;
}

NTSTATUS
NTAPI
ExSubscribeWnfStateChange(PVOID *SubscriptionHandle,
                          PCWNF_STATE_NAME StateName,
                          WNF_CHANGE_STAMP ChangeStamp,
                          ULONG Flags,
                          PWNF_KERNEL_CALLBACK Callback,
                          PVOID CallbackContext)
{
    WNF_STATE_NAME_INTERNAL InternalName;
    PWNF_NAME_INSTANCE Instance;
    PWNF_SUBSCRIPTION Subscription;
    NTSTATUS Status;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Flags);

    if ((SubscriptionHandle == NULL) || (Callback == NULL))
        return STATUS_INVALID_PARAMETER;

    Status = ExpWnfCaptureStateName(StateName, KernelMode, &InternalName);
    if (!NT_SUCCESS(Status))
        return Status;

    Subscription = ExpWnfAllocateSubscription(ChangeStamp,
                                              0,
                                              Callback,
                                              CallbackContext);
    if (Subscription == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ExpWnfReferenceOrCreateNameInstance(&InternalName,
                                                   KernelMode,
                                                   &Instance);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Subscription, TAG_WNF_SUB);
        return Status;
    }

    ExpWnfAttachSubscription(Instance, Subscription, &InternalName);

    if (ExpWnfReadChangeStamp(Instance) > ChangeStamp)
    {
        ExpWnfLockShared(&Instance->SubscriptionListLock);
        if (!InterlockedCompareExchange(&Subscription->Removed, 0, 0))
            ExpWnfSignalSubscription(Subscription);
        ExpWnfUnlockShared(&Instance->SubscriptionListLock);
    }

    ExpWnfDereferenceNameInstance(Instance);
    *SubscriptionHandle = Subscription;
    return STATUS_SUCCESS;
}

VOID
NTAPI
ExUnsubscribeWnfStateChange(PVOID SubscriptionHandle)
{
    PWNF_SUBSCRIPTION Subscription = SubscriptionHandle;
    PWNF_NAME_INSTANCE Instance;

    PAGED_CODE();

    if ((Subscription == NULL) || !ExpWnfInitialized)
        return;

    if (!InterlockedCompareExchange(&Subscription->Removed, 0, 0))
    {
        Instance = ExpWnfReferenceNameInstance(Subscription->StateName.Value);
        if (Instance != NULL)
        {
            ExpWnfLockExclusive(&Instance->SubscriptionListLock);
            if ((Subscription->NameInstance == Instance) &&
                !InterlockedCompareExchange(&Subscription->Removed, 0, 0))
            {
                ExpWnfDetachSubscription(Subscription, NULL);
            }
            ExpWnfUnlockExclusive(&Instance->SubscriptionListLock);
            ExpWnfDereferenceNameInstance(Instance);
        }
    }

    ExpWnfReapSubscription(Subscription);
}

