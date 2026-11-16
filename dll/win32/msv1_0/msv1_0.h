/*
 * PROJECT:     Authentication Package DLL
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        dll/win32/msv1_0/msv1_0.h
 * PURPOSE:     Common header file
 * COPYRIGHT:   Copyright 2013 Eric Kohl
 */

#include <sam_c.h>

#define FIXUP_POINTER(Pointer, Offset) ((Pointer != NULL) ? ((PWSTR)((ULONG_PTR)Pointer + Offset)) : NULL)


NTSTATUS
NTAPI
SamIConnect(IN PSAMPR_SERVER_NAME ServerName,
            OUT SAMPR_HANDLE *ServerHandle,
            IN ACCESS_MASK DesiredAccess,
            IN BOOLEAN Trusted);

VOID
NTAPI
SamIFreeVoid(PVOID Ptr);

VOID
NTAPI
SamIFree_SAMPR_ULONG_ARRAY(PSAMPR_ULONG_ARRAY Ptr);

VOID
NTAPI
SamIFree_SAMPR_USER_INFO_BUFFER(PSAMPR_USER_INFO_BUFFER Ptr,
                                USER_INFORMATION_CLASS InformationClass);

typedef PVOID LSAPR_HANDLE;

typedef struct _LSAPR_POLICY_AUDIT_EVENTS_INFO
{
    BOOLEAN AuditingMode;
    DWORD *EventAuditingOptions;
    DWORD MaximumAuditEventCount;
} LSAPR_POLICY_AUDIT_EVENTS_INFO, *PLSAPR_POLICY_AUDIT_EVENTS_INFO;

typedef struct _LSAPR_POLICY_PRIMARY_DOM_INFO
{
    RPC_UNICODE_STRING Name;
    PRPC_SID Sid;
} LSAPR_POLICY_PRIMARY_DOM_INFO, *PLSAPR_POLICY_PRIMARY_DOM_INFO;

typedef struct _LSAPR_POLICY_ACCOUNT_DOM_INFO
{
    RPC_UNICODE_STRING DomainName;
    PRPC_SID Sid;
} LSAPR_POLICY_ACCOUNT_DOM_INFO, *PLSAPR_POLICY_ACCOUNT_DOM_INFO;

typedef struct _LSAPR_POLICY_PD_ACCOUNT_INFO
{
    RPC_UNICODE_STRING Name;
} LSAPR_POLICY_PD_ACCOUNT_INFO, *PLSAPR_POLICY_PD_ACCOUNT_INFO;

typedef struct _POLICY_LSA_REPLICA_SRCE_INFO
{
    RPC_UNICODE_STRING ReplicaSource;
    RPC_UNICODE_STRING ReplicaAccountName;
} POLICY_LSA_REPLICA_SRCE_INFO, *PPOLICY_LSA_REPLICA_SRCE_INFO;

typedef struct _LSAPR_POLICY_DNS_DOMAIN_INFO
{
    RPC_UNICODE_STRING Name;
    RPC_UNICODE_STRING DnsDomainName;
    RPC_UNICODE_STRING DnsForestName;
    GUID DomainGuid;
    PRPC_SID Sid;
} LSAPR_POLICY_DNS_DOMAIN_INFO, *PLSAPR_POLICY_DNS_DOMAIN_INFO;

typedef union _LSAPR_POLICY_INFORMATION
{
    POLICY_AUDIT_LOG_INFO PolicyAuditLogInfo;
    LSAPR_POLICY_AUDIT_EVENTS_INFO PolicyAuditEventsInfo;
    LSAPR_POLICY_PRIMARY_DOM_INFO PolicyPrimaryDomInfo;
    LSAPR_POLICY_PD_ACCOUNT_INFO PolicyPdAccountInfo;
    LSAPR_POLICY_ACCOUNT_DOM_INFO PolicyAccountDomainInfo;
    POLICY_LSA_SERVER_ROLE_INFO PolicyServerRoleInfo;
    POLICY_LSA_REPLICA_SRCE_INFO PolicyReplicaSourceInfo;
    POLICY_DEFAULT_QUOTA_INFO PolicyDefaultQuotaInfo;
    POLICY_MODIFICATION_INFO PolicyModificationInfo;
    POLICY_AUDIT_FULL_SET_INFO PolicyAuditFullSetInfo;
    POLICY_AUDIT_FULL_QUERY_INFO PolicyAuditFullQueryInfo;
    LSAPR_POLICY_DNS_DOMAIN_INFO PolicyDnsDomainInfo;
    LSAPR_POLICY_DNS_DOMAIN_INFO PolicyDnsDomainInfoInt;
    LSAPR_POLICY_ACCOUNT_DOM_INFO PolicyLocalAccountDomainInfo;
} LSAPR_POLICY_INFORMATION, *PLSAPR_POLICY_INFORMATION;

VOID
NTAPI
LsaIFree_LSAPR_POLICY_INFORMATION(IN POLICY_INFORMATION_CLASS InformationClass,
                                  IN PLSAPR_POLICY_INFORMATION PolicyInformation);

NTSTATUS
WINAPI
LsaIOpenPolicyTrusted(OUT LSAPR_HANDLE *PolicyHandle);

NTSTATUS
WINAPI
LsarClose(IN OUT LSAPR_HANDLE *ObjectHandle);

NTSTATUS
WINAPI
LsarLookupPrivilegeValue(IN LSAPR_HANDLE PolicyHandle,
                         IN PRPC_UNICODE_STRING Name,
                         OUT PLUID Value);

NTSTATUS
WINAPI
LsarQueryInformationPolicy(IN LSAPR_HANDLE PolicyHandle,
                           IN POLICY_INFORMATION_CLASS InformationClass,
                           OUT PLSAPR_POLICY_INFORMATION *PolicyInformation);

NTSTATUS
WINAPI
SystemFunction007(PUNICODE_STRING string,
                  LPBYTE hash);

NTSTATUS
WINAPI
SystemFunction012(const BYTE *in,
                  const BYTE *key,
                  LPBYTE out);

struct ustring
{
    DWORD Length;
    DWORD MaximumLength;
    unsigned char *Buffer;
};

NTSTATUS
WINAPI
SystemFunction032(struct ustring *data,
                  const struct ustring *key);

typedef struct _MD5_CTX
{
    ULONG i[2];
    ULONG buf[4];
    UCHAR in[64];
    UCHAR digest[16];
} MD5_CTX, *PMD5_CTX;

VOID
WINAPI
MD5Init(PMD5_CTX Context);

VOID
WINAPI
MD5Update(PMD5_CTX Context,
          const UCHAR *Buffer,
          ULONG Length);

VOID
WINAPI
MD5Final(PMD5_CTX Context);

NTSTATUS
NTAPI
LsaApCallPackage(
    _In_ PLSA_CLIENT_REQUEST ClientRequest,
    _In_ PVOID ProtocolSubmitBuffer,
    _In_ PVOID ClientBufferBase,
    _In_ ULONG SubmitBufferLength,
    _Out_ PVOID *ProtocolReturnBuffer,
    _Out_ PULONG ReturnBufferLength,
    _Out_ PNTSTATUS ProtocolStatus);

NTSTATUS
NTAPI
LsaApCallPackagePassthrough(
    _In_ PLSA_CLIENT_REQUEST ClientRequest,
    _In_ PVOID ProtocolSubmitBuffer,
    _In_ PVOID ClientBufferBase,
    _In_ ULONG SubmitBufferLength,
    _Out_ PVOID *ProtocolReturnBuffer,
    _Out_ PULONG ReturnBufferLength,
    _Out_ PNTSTATUS ProtocolStatus);

NTSTATUS
NTAPI
LsaApCallPackageUntrusted(
    _In_ PLSA_CLIENT_REQUEST ClientRequest,
    _In_ PVOID ProtocolSubmitBuffer,
    _In_ PVOID ClientBufferBase,
    _In_ ULONG SubmitBufferLength,
    _Out_ PVOID *ProtocolReturnBuffer,
    _Out_ PULONG ReturnBufferLength,
    _Out_ PNTSTATUS ProtocolStatus);

VOID
NTAPI
LsaApLogonTerminated(
    _In_ PLUID LogonId);

NTSTATUS
NTAPI
LsaApLogonUserEx2(
    _In_ PLSA_CLIENT_REQUEST ClientRequest,
    _In_ SECURITY_LOGON_TYPE LogonType,
    _In_ PVOID ProtocolSubmitBuffer,
    _In_ PVOID ClientBufferBase,
    _In_ ULONG SubmitBufferSize,
    _Out_ PVOID *ProfileBuffer,
    _Out_ PULONG ProfileBufferSize,
    _Out_ PLUID LogonId,
    _Out_ PNTSTATUS SubStatus,
    _Out_ PLSA_TOKEN_INFORMATION_TYPE TokenInformationType,
    _Out_ PVOID *TokenInformation,
    _Out_ PUNICODE_STRING *AccountName,
    _Out_ PUNICODE_STRING *AuthenticatingAuthority,
    _Out_ PUNICODE_STRING *MachineName,
    _Out_ PSECPKG_PRIMARY_CRED PrimaryCredentials,
    _Out_ PSECPKG_SUPPLEMENTAL_CRED_ARRAY *SupplementalCredentials);

/* EOF */
