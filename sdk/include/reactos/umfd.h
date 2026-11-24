/*
 * PROJECT:     LiberNT win32k user-mode font driver channel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Wire contract between win32k and fontdrvhost.exe (Orosi-private, not Windows')
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 */

#pragma once

/* Private NtGdiExtEscape codes, valid only with hdc == NULL and no driver name */
#define UMFD_ESCAPE_REGISTER    0x554D4601
#define UMFD_ESCAPE_SERVE       0x554D4602
#define UMFD_ESCAPE_UNREGISTER  0x554D4603
#define UMFD_IS_ESCAPE(e)       ((ULONG)(e) >= UMFD_ESCAPE_REGISTER && (ULONG)(e) <= UMFD_ESCAPE_UNREGISTER)

#define UMFD_MAGIC              0x44464D55 /* 'UMFD' */
#define UMFD_VERSION            1

/* Operations win32k asks the host for */
#define UMFD_OP_LOAD            1
#define UMFD_OP_UNLOAD          2
#define UMFD_OP_GLYPH_INDEX     3
#define UMFD_OP_GLYPH_BITMAP    4
#define UMFD_OP_GLYPH_OUTLINE   5
#define UMFD_OP_TABLE           6
#define UMFD_OP_KERN            7

/* Absolute ceilings; per-request caps derived from ppem and transform are lower */
#define UMFD_MAX_BITMAP_BYTES   (16 * 1024 * 1024)
#define UMFD_MAX_OUTLINE_POINTS 65536
#define UMFD_MAX_KERN_PAIRS     65536
#define UMFD_MAX_MESSAGE        (UMFD_MAX_BITMAP_BYTES + 64 * 1024)

/* Every request and reply starts with this header; the payload follows it */
typedef struct _UMFD_HEADER
{
    ULONG Magic;
    USHORT Version;
    USHORT Op;
    ULONG RequestId;    /* 0 in a SERVE call that carries no reply */
    ULONG FontId;
    LONG Status;        /* NTSTATUS of the operation, in replies */
    ULONG cbPayload;
} UMFD_HEADER, *PUMFD_HEADER;

/* UMFD_OP_LOAD: the font file as a read-only view win32k mapped into the host */
#define UMFD_LOAD_VALIDATE_ONLY 0x00000001
#define UMFD_MAX_FACES          4096

typedef struct _UMFD_LOAD_REQUEST
{
    ULONG64 ViewBase;
    ULONG64 ViewSize;
    LONG FaceIndex;     /* -1: every face in the file */
    ULONG Flags;
} UMFD_LOAD_REQUEST, *PUMFD_LOAD_REQUEST;

typedef struct _UMFD_LOAD_REPLY
{
    ULONG NumFaces;
    ULONG Reserved;
} UMFD_LOAD_REPLY, *PUMFD_LOAD_REPLY;

/* UMFD_ESCAPE_REGISTER input; the caller needs SeTcbPrivilege */
typedef struct _UMFD_REGISTER
{
    ULONG Magic;
    USHORT Version;
    USHORT Reserved;
    ULONG64 ProcessHandle;  /* the suspended host, in the caller's session */
} UMFD_REGISTER, *PUMFD_REGISTER;
