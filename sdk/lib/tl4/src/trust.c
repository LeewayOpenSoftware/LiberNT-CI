/*
 * PROJECT:     Theme Loader 4 - trust decisions
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Privileged-process classifier (T-DEF-3/4) and user-writable test (T-DEF-1/2/5), pure functions
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */
#include <tl4/trust/trust.h>

/* ---- bounded little-endian reads -------------------------------------------------------------------------------- */

static int
rd16(const uint8_t *p, size_t size, size_t off, uint32_t *v)
{
    if (off > size || size - off < 2)
        return 0;
    *v = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8);
    return 1;
}

static int
rd32(const uint8_t *p, size_t size, size_t off, uint32_t *v)
{
    if (off > size || size - off < 4)
        return 0;
    *v = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
    return 1;
}

/* ---- SIDs ------------------------------------------------------------------------------------------------------- */

#define SID_MAX_SUB 15u

size_t
tl4_trust_sid_length(const uint8_t *sid, size_t size)
{
    size_t len;

    if (!sid || size < 8 || sid[0] != 1 || sid[1] > SID_MAX_SUB)
        return 0;
    len = 8u + 4u * (size_t)sid[1];
    return len <= size ? len : 0;
}

/* S-1-5-<sub...>: NT authority SIDs only, which is every SID this policy names. */
static int
sid_is_nt(const uint8_t *sid, size_t len, const uint32_t *sub, uint32_t nsub)
{
    uint32_t i, v;

    if (len != 8u + 4u * nsub || sid[1] != nsub)
        return 0;
    if (sid[2] || sid[3] || sid[4] || sid[5] || sid[6] || sid[7] != 5)
        return 0;
    for (i = 0; i < nsub; i++)
    {
        if (!rd32(sid, len, 8u + 4u * i, &v) || v != sub[i])
            return 0;
    }
    return 1;
}

static const uint32_t SubSystem[] = { 18 };
static const uint32_t SubLocalService[] = { 19 };
static const uint32_t SubNetworkService[] = { 20 };
static const uint32_t SubAdmins[] = { 32, 544 };
/* NT SERVICE\TrustedInstaller, the owner the LiberNT image sets on the system tree (boot/bootdata/filesecurity.txt) */
static const uint32_t SubComponentOwner[] = { 80, 956008885u, 3418522649u, 1831038044u, 1853292631u, 2271478464u };

static int
sid_trusted(const uint8_t *sid, size_t len, unsigned flags)
{
    return sid_is_nt(sid, len, SubSystem, 1) || sid_is_nt(sid, len, SubAdmins, 2) ||
           ((flags & TL4_TRUST_COMPONENT_OWNER_TRUSTED) && sid_is_nt(sid, len, SubComponentOwner, 6));
}

/* ---- the user-writable test ------------------------------------------------------------------------------------- */

#define FILE_WRITE_DATA_       0x00000002u  /* = FILE_ADD_FILE on a directory */
#define FILE_APPEND_DATA_      0x00000004u  /* = FILE_ADD_SUBDIRECTORY on a directory */
#define FILE_WRITE_EA_         0x00000010u
#define FILE_DELETE_CHILD_     0x00000040u
#define FILE_WRITE_ATTRIBUTES_ 0x00000100u
#define KEY_SET_VALUE_         0x00000002u
#define KEY_CREATE_SUB_KEY_    0x00000004u
#define KEY_CREATE_LINK_       0x00000020u  /* KEY_NOTIFY 0x10, KEY_ENUMERATE_SUB_KEYS 0x8, KEY_QUERY_VALUE 0x1 and the
                                             * KEY_WOW64_* flags 0x100/0x200 are not write rights (T-DEF-1) */
#define DELETE_                0x00010000u
#define WRITE_DAC_             0x00040000u
#define WRITE_OWNER_           0x00080000u
#define MAXIMUM_ALLOWED_       0x02000000u
#define GENERIC_ALL_           0x10000000u
#define GENERIC_WRITE_         0x40000000u

uint32_t
tl4_trust_write_mask(enum tl4_trust_object object)
{
    uint32_t file = FILE_WRITE_DATA_ | FILE_APPEND_DATA_ | FILE_WRITE_EA_ | FILE_WRITE_ATTRIBUTES_ | DELETE_ |
                    WRITE_DAC_ | WRITE_OWNER_ | MAXIMUM_ALLOWED_ | GENERIC_ALL_ | GENERIC_WRITE_;

    switch (object)
    {
        case TL4_TRUST_FILE:
            return file;
        case TL4_TRUST_DIR:
            return file | FILE_DELETE_CHILD_;
        case TL4_TRUST_DIR_ABOVE_ROOT:
            /* T-DEF-2: the rights that let a principal replace or re-point the path below */
            return DELETE_ | WRITE_DAC_ | WRITE_OWNER_ | FILE_DELETE_CHILD_ | MAXIMUM_ALLOWED_ | GENERIC_ALL_;
        case TL4_TRUST_KEY:
            /* T-DEF-1 (policy v6): the write rights of a key; KEY_READ (query, enumerate, notify) is not one */
            return KEY_SET_VALUE_ | KEY_CREATE_SUB_KEY_ | KEY_CREATE_LINK_ | DELETE_ | WRITE_DAC_ | WRITE_OWNER_ |
                   MAXIMUM_ALLOWED_ | GENERIC_ALL_ | GENERIC_WRITE_;
    }
    return 0xFFFFFFFFu;  /* unknown object kind: every right counts */
}

#define SE_DACL_PRESENT_   0x0004u
#define SE_SELF_RELATIVE_  0x8000u
#define INHERIT_ONLY_ACE_  0x08u

enum { ACE_ALLOWED = 0, ACE_DENIED = 1, ACE_DENIED_OBJECT = 6, ACE_DENIED_CALLBACK = 10,
       ACE_DENIED_CALLBACK_OBJECT = 12 };

tl4_trust_reason
tl4_trust_sd_writable(const uint8_t *sd, size_t size, enum tl4_trust_object object, unsigned flags)
{
    uint32_t control, owner_off, dacl_off, acl_size, ace_count, i;
    size_t owner_len, pos, acl_end;
    uint32_t mask = tl4_trust_write_mask(object);
    tl4_trust_reason result = TL4_TRUST_SAFE;

    if (!sd || size < 20 || size > TL4_TRUST_MAX_SD || sd[0] != 1)
        return TL4_TRUST_W_UNREADABLE;
    if (!rd16(sd, size, 2, &control) || !(control & SE_SELF_RELATIVE_) || !rd32(sd, size, 4, &owner_off) ||
        !rd32(sd, size, 16, &dacl_off))
        return TL4_TRUST_W_UNREADABLE;

    /* owner */
    if (owner_off == 0)
        return TL4_TRUST_W_OWNER;
    if (owner_off >= size || !(owner_len = tl4_trust_sid_length(sd + owner_off, size - owner_off)))
        return TL4_TRUST_W_UNREADABLE;
    if (!sid_trusted(sd + owner_off, owner_len, flags))
        result = TL4_TRUST_W_OWNER;

    /* DACL: present and not NULL */
    if (!(control & SE_DACL_PRESENT_) || dacl_off == 0)
        return result != TL4_TRUST_SAFE ? result : TL4_TRUST_W_NO_DACL;
    if (dacl_off >= size || size - dacl_off < 8 || sd[dacl_off] < 2 || sd[dacl_off] > 4)
        return TL4_TRUST_W_UNREADABLE;
    if (!rd16(sd, size, dacl_off + 2, &acl_size) || !rd16(sd, size, dacl_off + 4, &ace_count) || acl_size < 8 ||
        acl_size > size - dacl_off)
        return TL4_TRUST_W_UNREADABLE;
    acl_end = dacl_off + acl_size;

    /* every ACE is parsed, so a malformed list is reported as unreadable whatever came before */
    pos = dacl_off + 8;
    for (i = 0; i < ace_count; i++)
    {
        uint32_t ace_size, access;
        uint8_t type, ace_flags;
        size_t sid_len;

        if (pos > acl_end || acl_end - pos < 4 || !rd16(sd, acl_end, pos + 2, &ace_size) || ace_size < 4 ||
            (ace_size & 3u) || ace_size > acl_end - pos)
            return TL4_TRUST_W_UNREADABLE;
        type = sd[pos];
        ace_flags = sd[pos + 1];
        switch (type)
        {
            case ACE_ALLOWED:
            case ACE_DENIED:
                if (ace_size < 8 || !rd32(sd, acl_end, pos + 4, &access) ||
                    !(sid_len = tl4_trust_sid_length(sd + pos + 8, ace_size - 8u)))
                    return TL4_TRUST_W_UNREADABLE;
                if (type == ACE_ALLOWED && !(ace_flags & INHERIT_ONLY_ACE_) && (access & mask) &&
                    !sid_trusted(sd + pos + 8, sid_len, flags) && result == TL4_TRUST_SAFE)
                    result = TL4_TRUST_W_GRANT;
                break;
            case ACE_DENIED_OBJECT:
            case ACE_DENIED_CALLBACK:
            case ACE_DENIED_CALLBACK_OBJECT:
                break;                       /* deny entries never make an object safe, and never unsafe */
            default:
                if (!(ace_flags & INHERIT_ONLY_ACE_) && result == TL4_TRUST_SAFE)
                    result = TL4_TRUST_W_ACE_UNKNOWN;   /* allowed-object/callback, audit, label, ...: unknown */
                break;
        }
        pos += ace_size;
    }
    return result;
}

/* ---- the classifier --------------------------------------------------------------------------------------------- */

#define SECURITY_MANDATORY_HIGH_RID_ 0x3000u

static int
name_is(const uint16_t *name, uint32_t chars, const char *ascii)
{
    uint32_t i;

    for (i = 0; i < chars; i++)
    {
        uint16_t c = name[i];
        char a = ascii[i];

        if (!a)
            return 0;
        if (c >= 'A' && c <= 'Z')
            c = (uint16_t)(c - 'A' + 'a');
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (c != (uint16_t)(unsigned char)a)
            return 0;
    }
    return ascii[chars] == 0;
}

static int
privilege_listed(uint64_t luid)
{
    static const uint32_t listed[] = { TL4_SE_TCB, TL4_SE_DEBUG, TL4_SE_IMPERSONATE, TL4_SE_ASSIGN_PRIMARY,
                                       TL4_SE_CREATE_TOKEN, TL4_SE_LOAD_DRIVER, TL4_SE_BACKUP, TL4_SE_RESTORE,
                                       TL4_SE_TAKE_OWNERSHIP, TL4_SE_SECURITY };
    uint32_t i;

    if (luid >> 32)
        return 0;
    for (i = 0; i < sizeof(listed) / sizeof(listed[0]); i++)
    {
        if ((uint32_t)luid == listed[i])
            return 1;
    }
    return 0;
}

tl4_trust_class
tl4_trust_classify(const tl4_trust_facts *f)
{
    size_t user_len;
    uint32_t i;
    int admin = 0, priv = 0;

    /* T-DEF-4 / T-FAIL-3: anything missing or out of range is a privileged process */
    if (!f || !f->complete || f->ngroups > TL4_TRUST_MAX_GROUPS || f->nprivileges > TL4_TRUST_MAX_PRIVILEGES ||
        (f->ngroups && !f->groups) || (f->nprivileges && !f->privileges) || !f->desktop || !f->desktop_chars ||
        f->desktop_chars > TL4_TRUST_MAX_NAME || !f->winstation || !f->winstation_chars ||
        f->winstation_chars > TL4_TRUST_MAX_NAME || !(user_len = tl4_trust_sid_length(f->user.sid, f->user.size)))
        return TL4_TRUST_P_INCOMPLETE;
    for (i = 0; i < f->ngroups; i++)
    {
        size_t len = tl4_trust_sid_length(f->groups[i].sid, f->groups[i].size);

        if (!len)
            return TL4_TRUST_P_INCOMPLETE;
        /* policy v5: present and not deny-only, enabled or not (a process can enable a disabled group) */
        if (sid_is_nt(f->groups[i].sid, len, SubAdmins, 2) && !(f->groups[i].attributes & TL4_SE_GROUP_USE_FOR_DENY_ONLY))
            admin = 1;
    }
    for (i = 0; i < f->nprivileges; i++)
        priv |= privilege_listed(f->privileges[i]);

    if (sid_is_nt(f->user.sid, user_len, SubSystem, 1) || sid_is_nt(f->user.sid, user_len, SubLocalService, 1) ||
        sid_is_nt(f->user.sid, user_len, SubNetworkService, 1))
        return TL4_TRUST_P_SERVICE_USER;
    if (admin)
        return TL4_TRUST_P_ADMINISTRATORS;
    if (f->elevation_type == 2 || f->is_elevated)
        return TL4_TRUST_P_ELEVATED;
    if (f->integrity_rid >= SECURITY_MANDATORY_HIGH_RID_)
        return TL4_TRUST_P_INTEGRITY;
    if (f->started_by_scm)
        return TL4_TRUST_P_SERVICE_START;
    if (f->session_id == 0 && !f->interactive_in_session0)
        return TL4_TRUST_P_SESSION0;
    if (priv)
        return TL4_TRUST_P_PRIVILEGE;
    if (f->is_logon_process || name_is(f->desktop, f->desktop_chars, "Winlogon") ||
        name_is(f->desktop, f->desktop_chars, "Screen-saver") || !name_is(f->winstation, f->winstation_chars, "WinSta0"))
        return TL4_TRUST_P_LOGON;
    return TL4_TRUST_ORDINARY;
}

int
tl4_trust_uses_machine_default(const tl4_trust_facts *f)
{
    size_t user_len;

    if (!f || !f->complete || !(user_len = tl4_trust_sid_length(f->user.sid, f->user.size)))
        return 1;
    return sid_is_nt(f->user.sid, user_len, SubSystem, 1) || sid_is_nt(f->user.sid, user_len, SubLocalService, 1) ||
           sid_is_nt(f->user.sid, user_len, SubNetworkService, 1) || f->started_by_scm != 0;
}
