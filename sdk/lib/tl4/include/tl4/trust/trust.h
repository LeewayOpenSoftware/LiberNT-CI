/*
 * PROJECT:     Theme Loader 4 - trust decisions
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Pure functions behind the I1 trust gate: the privileged-process classifier (THEME-TRUST-POLICY
 *              T-DEF-3/4) and the user-writable test on a security descriptor (T-DEF-1/2, T-DEF-5)
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * docs/design/I1-INTEGRATION.md sections 5.2 and 5.5. The integration (LiberNT uxtheme trust.c) gathers the facts
 * from the token, the desktop and the file handles; these functions only decide, so the decisions are unit-tested and
 * fuzzed on the host. No OS call, no allocation, no global state (R-GEN-1 style). Every input is treated as unchecked:
 * a security descriptor is parsed with bounded reads, and anything malformed or unknown decides for the safe side
 * (writable, privileged), as T-FAIL-3 requires.
 */
#ifndef TL4_TRUST_TRUST_H
#define TL4_TRUST_TRUST_H

#include <stddef.h>
#include <stdint.h>

/* ---- the user-writable test (T-DEF-1, T-DEF-2) ------------------------------------------------------------------ */

enum tl4_trust_object {
    TL4_TRUST_FILE = 0,           /* the style file itself (T-DEF-1, file rights) */
    TL4_TRUST_DIR = 1,            /* a directory from the file up to and including the store root (T-DEF-1) */
    TL4_TRUST_DIR_ABOVE_ROOT = 2, /* a directory above the store root, up to the volume root (T-DEF-2, narrow set) */
    TL4_TRUST_KEY = 3             /* a registry key (T-DEF-1, policy v6: key rights; notify is not a write right) */
};

/* Flags for tl4_trust_sd_writable. */
#define TL4_TRUST_COMPONENT_OWNER_TRUSTED 0x1u /* T-DEF-5 was shown on this system: the component owner SID is trusted */

/* Why an object counts as user-writable; TL4_TRUST_SAFE when it does not. */
typedef enum tl4_trust_reason {
    TL4_TRUST_SAFE = 0,
    TL4_TRUST_W_UNREADABLE,      /* no descriptor, or not a well-formed self-relative descriptor */
    TL4_TRUST_W_OWNER,           /* missing owner, or the owner is not a trusted principal */
    TL4_TRUST_W_NO_DACL,         /* no DACL or a NULL DACL */
    TL4_TRUST_W_ACE_UNKNOWN,     /* an ACE type this test does not know in the DACL (counted as a grant) */
    TL4_TRUST_W_GRANT            /* an effective allow ACE gives a write-class right to an ordinary principal */
} tl4_trust_reason;

/* Every result other than TL4_TRUST_SAFE means user-writable: the reasons exist for logging, never for a decision.
 * A descriptor larger than TL4_TRUST_MAX_SD is TL4_TRUST_W_UNREADABLE. */
#define TL4_TRUST_MAX_SD (128u * 1024u)

/* Decide whether a security descriptor makes the object user-writable. `sd`/`size` is a self-relative descriptor as
 * GetSecurityInfo / NtQuerySecurityObject return it (NULL = could not be read). Returns TL4_TRUST_SAFE only when the
 * owner is trusted, a DACL is present and not NULL, every ACE is well-formed and of a known type, and no effective
 * (not inherit-only) allow ACE gives an ordinary principal a write-class right for `object`. Deny ACEs are ignored
 * (T-DEF-1). Trusted principals: SYSTEM, Administrators, and the component owner when the flag says so (T-DEF-5). */
tl4_trust_reason tl4_trust_sd_writable(const uint8_t *sd, size_t size, enum tl4_trust_object object, unsigned flags);

/* The write-class access mask for `object` (exported for the tests and the documentation of the integration). The
 * low 16 bits are object-specific: a key's KEY_NOTIFY (0x10) is the same bit as a file's FILE_WRITE_EA, so a key
 * checked with the file or directory mask would count KEY_READ as a write grant (I1a-T review, MUST 1). */
uint32_t tl4_trust_write_mask(enum tl4_trust_object object);

/* ---- the privileged-process classifier (T-DEF-3, T-DEF-4) ------------------------------------------------------ */

#define TL4_TRUST_MAX_GROUPS     256u
#define TL4_TRUST_MAX_PRIVILEGES 64u
#define TL4_TRUST_MAX_NAME       64u   /* desktop / window station name, UTF-16 code units */

typedef struct tl4_trust_sid_ref {
    const uint8_t *sid;          /* a binary SID; its own size is read from it, bounded by `size` */
    size_t size;
    uint32_t attributes;         /* SE_GROUP_* for groups; ignored for the user */
} tl4_trust_sid_ref;

/* SE_GROUP_* bits the classifier reads (winnt.h values). */
#define TL4_SE_GROUP_ENABLED            0x00000004u
#define TL4_SE_GROUP_USE_FOR_DENY_ONLY  0x00000010u

/* Well-known privilege LUIDs (LowPart, HighPart 0) of T-DEF-3. */
#define TL4_SE_CREATE_TOKEN       2u
#define TL4_SE_ASSIGN_PRIMARY     3u
#define TL4_SE_SECURITY           8u
#define TL4_SE_TAKE_OWNERSHIP     9u
#define TL4_SE_LOAD_DRIVER        10u
#define TL4_SE_BACKUP             17u
#define TL4_SE_RESTORE            18u
#define TL4_SE_DEBUG              20u
#define TL4_SE_TCB                7u
#define TL4_SE_IMPERSONATE        29u

/* Facts about the process, gathered from its primary token and its thread's desktop (I1 design 5.2). */
typedef struct tl4_trust_facts {
    int complete;                /* 0 when any query the integration needed failed (T-DEF-4: then privileged) */
    tl4_trust_sid_ref user;
    const tl4_trust_sid_ref *groups;
    uint32_t ngroups;            /* at most TL4_TRUST_MAX_GROUPS, else privileged */
    uint32_t elevation_type;     /* TokenElevationType: 1 default, 2 full, 3 limited; 0 = the class does not exist */
    int is_elevated;             /* TokenElevation.TokenIsElevated */
    uint32_t integrity_rid;      /* mandatory label RID; 0 = the system has no integrity levels */
    uint32_t session_id;
    int interactive_in_session0; /* system fact: interactive users run in session 0 (then the session clause is off) */
    int started_by_scm;          /* the parent is the service control manager (checked by the integration) */
    const uint64_t *privileges;  /* LUIDs present in the token, enabled or not */
    uint32_t nprivileges;        /* at most TL4_TRUST_MAX_PRIVILEGES, else privileged */
    int is_logon_process;
    const uint16_t *desktop;     /* the thread's desktop name, UTF-16, not terminated */
    uint32_t desktop_chars;
    const uint16_t *winstation;  /* its window station name */
    uint32_t winstation_chars;
} tl4_trust_facts;

/* Why a process is privileged; TL4_TRUST_ORDINARY when no clause holds. */
typedef enum tl4_trust_class {
    TL4_TRUST_ORDINARY = 0,
    TL4_TRUST_P_INCOMPLETE,      /* a query failed or an input is out of range (T-DEF-4, T-FAIL-3) */
    TL4_TRUST_P_SERVICE_USER,    /* SYSTEM, LocalService or NetworkService */
    TL4_TRUST_P_ADMINISTRATORS,  /* Administrators present and not deny-only, enabled or not (policy v5) */
    TL4_TRUST_P_ELEVATED,        /* elevation type full, or TokenIsElevated */
    TL4_TRUST_P_INTEGRITY,       /* integrity high or above */
    TL4_TRUST_P_SERVICE_START,   /* started by the service control manager */
    TL4_TRUST_P_SESSION0,        /* session 0 where interactive logons are kept out of it */
    TL4_TRUST_P_PRIVILEGE,       /* one of the ten privileges of T-DEF-3 */
    TL4_TRUST_P_LOGON            /* the logon process, or the logon / secure desktop */
} tl4_trust_class;

/* Classify the process from its facts. The first clause that holds is returned (the order above); any clause makes
 * the process privileged. NULL facts, a malformed SID, or counts over the limits give TL4_TRUST_P_INCOMPLETE. */
tl4_trust_class tl4_trust_classify(const tl4_trust_facts *facts);

/* The reason returned above is for logging only: a caller tests "!= TL4_TRUST_ORDINARY" and nothing else. It must
 * not be used for the T-PRIV-2 decision (a service user that also matches an earlier clause is reported under that
 * clause); this function makes that decision. It returns 1 when the process must read no user setting and use the
 * machine default style: the token user is SYSTEM, LocalService or NetworkService, or the service control manager
 * started the process. Incomplete or malformed facts give 1 as well (T-FAIL-3). */
int tl4_trust_uses_machine_default(const tl4_trust_facts *facts);

/* A binary SID's length when it is well-formed inside `size` (revision 1, at most 15 sub-authorities), else 0. */
size_t tl4_trust_sid_length(const uint8_t *sid, size_t size);

#endif
