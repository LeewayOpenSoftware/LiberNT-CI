/*
 * PROJECT:     LiberNT Update - libservicing
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     ENFORCE verification of a package: manifest, payload hashes, signed catalog,
 *              signer pinned to the development certificate
 * COPYRIGHT:   Copyright 2026 Leeway Open Software
 *
 * There is no audit mode here (owner condition 2). Order of checks, each one rejecting
 * before anything is written to the target:
 *   1. manifest.json parses and obeys the schema and the path rules;
 *   2. every payload exists inside the package and its SHA-256 matches the manifest;
 *   3. catalog.cat exists and WinVerifyTrust accepts its signature;
 *   4. the catalog members cover manifest.json and every payload (by SHA-256);
 *   5. the signer certificate hash equals manifest.certificateSha256 AND the certificate
 *      installed in the target's trust\ directory;
 *   6. manifest.sig (RSA-SHA256, PKCS#1 v1.5, made at packaging time with the development key)
 *      verifies over manifest.json with the signer's public key. The boot applier checks the same
 *      signature without WinVerifyTrust (S1-CONTRACTS.md plan.signature).
 *
 * The development certificate is self-signed, so WinVerifyTrust reports an untrusted root
 * (CERT_E_UNTRUSTEDROOT / CERT_E_CHAINING) after it has checked the signature itself. That
 * result is accepted only because step 5 pins the exact certificate; any other failure
 * (bad digest, bad signature, expired, revoked-by-policy, unknown form) is a rejection.
 */

#include "libservicing.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <wincrypt.h>

#ifndef CERT_E_UNTRUSTEDROOT
#define CERT_E_UNTRUSTEDROOT ((LONG)0x800B0109L)
#endif
#ifndef CERT_E_CHAINING
#define CERT_E_CHAINING ((LONG)0x800B010AL)
#endif
#ifndef TRUST_E_NOSIGNATURE
#define TRUST_E_NOSIGNATURE ((LONG)0x800B0100L)
#endif

DWORD LnCertFileSha256(const WCHAR *cerPath, char out[65])
{
    BYTE *data;
    size_t len;
    DWORD err = LnReadFileAll(cerPath, &data, &len);

    if (err)
        return err;
    if (len < 16 || data[0] != 0x30)                    /* a DER certificate is an ASN.1 SEQUENCE */
        err = LN_E_CERT_MISMATCH;
    else
        LnSha256Hex(data, len, out);
    free(data);
    return err;
}

/* Returns the WinVerifyTrust status and, if a signer exists, the SHA-256 of its certificate. */
static LONG TrustCatalogFile(const WCHAR *catPath, char signerHash[65], BYTE **certDer, DWORD *certLen)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO fi;
    WINTRUST_DATA wd;
    LONG status;

    signerHash[0] = 0;
    *certDer = NULL;
    *certLen = 0;
    memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = catPath;
    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL;

    status = WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &wd);
    if (wd.hWVTStateData)
    {
        CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);

        if (pd)
        {
            CRYPT_PROVIDER_SGNR *sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0);
            CRYPT_PROVIDER_CERT *pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : NULL;

            if (pc && pc->pCert && pc->pCert->pbCertEncoded)
            {
                LnSha256Hex(pc->pCert->pbCertEncoded, pc->pCert->cbCertEncoded, signerHash);
                *certDer = malloc(pc->pCert->cbCertEncoded);
                if (*certDer)
                {
                    memcpy(*certDer, pc->pCert->pbCertEncoded, pc->pCert->cbCertEncoded);
                    *certLen = pc->pCert->cbCertEncoded;
                }
            }
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(INVALID_HANDLE_VALUE, &action, &wd);
    return status;
}

/* Collects the SHA-256 digests of every catalog member. */
static DWORD ReadCatalogDigests(const WCHAR *catPath, unsigned char (**digests)[32], size_t *count)
{
    HANDLE cat;
    CRYPTCATMEMBER *mem = NULL;
    unsigned char (*list)[32] = NULL;
    size_t n = 0, cap = 0;
    WCHAR *copy = _wcsdup(catPath);

    *digests = NULL;
    *count = 0;
    if (!copy)
        return ERROR_NOT_ENOUGH_MEMORY;
    cat = CryptCATOpen(copy, CRYPTCAT_OPEN_EXISTING, 0, 0, 0);
    free(copy);
    if (cat == INVALID_HANDLE_VALUE || !cat)
        return LN_E_CATALOG_INVALID;

    while ((mem = CryptCATEnumerateMember(cat, mem)) != NULL)
    {
        if (!mem->pIndirectData || mem->pIndirectData->Digest.cbData != 32 ||
            !mem->pIndirectData->Digest.pbData)
            continue;                                   /* not a SHA-256 member: cannot cover anything */
        if (n == cap)
        {
            size_t ncap = cap ? cap * 2 : 64;
            unsigned char (*p)[32] = realloc(list, ncap * 32);

            if (!p)
            {
                free(list);
                CryptCATClose(cat);
                return ERROR_NOT_ENOUGH_MEMORY;
            }
            list = p;
            cap = ncap;
        }
        memcpy(list[n++], mem->pIndirectData->Digest.pbData, 32);
    }
    CryptCATClose(cat);
    *digests = list;
    *count = n;
    return ERROR_SUCCESS;
}

static BOOL Covered(unsigned char (*digests)[32], size_t count, const char *hex)
{
    char h[65];
    size_t i;

    for (i = 0; i < count; i++)
    {
        LnHexEncode(digests[i], 32, h);
        if (!strcmp(h, hex))
            return TRUE;
    }
    return FALSE;
}

/* RSA-SHA256 (PKCS#1 v1.5) signature of the manifest bytes by the signer's key.
 * CryptoAPI wants the signature in little-endian order; the packaging tool writes big-endian. */
static DWORD VerifyManifestSignature(const BYTE *certDer, DWORD certLen, const WCHAR *pkgDir)
{
    WCHAR *mp = LnJoin(pkgDir, L"manifest.json"), *sp = LnJoin(pkgDir, L"manifest.sig");
    BYTE *man = NULL, *sig = NULL;
    size_t mlen = 0, slen = 0, i;
    PCCERT_CONTEXT ctx = NULL;
    HCRYPTPROV prov = 0;
    HCRYPTKEY key = 0;
    HCRYPTHASH hash = 0;
    DWORD err = LN_E_CATALOG_INVALID;

    if (!mp || !sp)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto out;
    }
    if (!LnPathExists(sp))
    {
        err = LN_E_UNSIGNED;
        goto out;
    }
    if (LnReadFileAll(mp, &man, &mlen) || LnReadFileAll(sp, &sig, &slen) || slen < 64 || slen > 1024)
        goto out;
    ctx = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, certDer, certLen);
    if (!ctx)
        goto out;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto out;
    if (!CryptImportPublicKeyInfo(prov, X509_ASN_ENCODING, &ctx->pCertInfo->SubjectPublicKeyInfo, &key))
        goto out;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash) || !CryptHashData(hash, man, (DWORD)mlen, 0))
        goto out;
    for (i = 0; i < slen / 2; i++)
    {
        BYTE tmp = sig[i];

        sig[i] = sig[slen - 1 - i];
        sig[slen - 1 - i] = tmp;
    }
    if (CryptVerifySignatureW(hash, sig, (DWORD)slen, key, NULL, 0))
        err = ERROR_SUCCESS;
out:
    if (hash) CryptDestroyHash(hash);
    if (key) CryptDestroyKey(key);
    if (prov) CryptReleaseContext(prov, 0);
    if (ctx) CertFreeCertificateContext(ctx);
    free(man);
    free(sig);
    free(mp);
    free(sp);
    return err;
}

/* t == NULL: pkgDir is a package folder (payload\<path>). t != NULL: pkgDir is the copy held in the
 * store (packages\<id>\<ver>) and the payloads are the store objects, checked by hash. */
static DWORD VerifyCommon(const LN_TARGET *t, const WCHAR *pkgDir, const char *pinnedCertSha256, LN_MANIFEST *out)
{
    LN_MANIFEST m;
    WCHAR *manifestPath = NULL, *catPath = NULL, *payloadDir = NULL;
    unsigned char (*digests)[32] = NULL;
    size_t ndigests = 0, i;
    char signer[65];
    BYTE *certDer = NULL;
    DWORD certLen = 0;
    LONG trust;
    DWORD err;

    if (!pinnedCertSha256 || !LnIsSha256Hex(pinnedCertSha256))
        return LN_E_NO_TRUST_ANCHOR;

    manifestPath = LnJoin(pkgDir, L"manifest.json");
    catPath = LnJoin(pkgDir, L"catalog.cat");
    payloadDir = LnJoin(pkgDir, L"payload");
    if (!manifestPath || !catPath || !payloadDir)
    {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done_nomanifest;
    }

    /* 1. manifest */
    err = LnManifestLoad(manifestPath, &m);
    if (err)
        goto done_nomanifest;

    /* 2. payload hashes */
    for (i = 0; i < m.itemCount; i++)
    {
        WCHAR *p;
        char h[65];

        if (m.items[i].isDelete)
            continue;
        if (t)
        {
            err = LnStoreObjectPath(t, m.items[i].sha256, &p);
            if (err)
                goto done;
        }
        else
            p = LnJoin(payloadDir, m.items[i].path);
        if (!p)
        {
            err = ERROR_NOT_ENOUGH_MEMORY;
            goto done;
        }
        err = LnSha256FileHex(p, h);
        free(p);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        {
            err = LN_E_HASH_MISMATCH;                   /* declared but missing */
            goto done;
        }
        if (err)
            goto done;
        if (strcmp(h, m.items[i].sha256))
        {
            err = LN_E_HASH_MISMATCH;
            goto done;
        }
    }

    /* 3. signed catalog */
    if (!LnPathExists(catPath))
    {
        err = LN_E_UNSIGNED;
        goto done;
    }
    trust = TrustCatalogFile(catPath, signer, &certDer, &certLen);
    if (trust == TRUST_E_NOSIGNATURE)
    {
        err = LN_E_UNSIGNED;
        goto done;
    }
    if (trust != 0 && trust != CERT_E_UNTRUSTEDROOT)
    {
        err = LN_E_CATALOG_INVALID;
        goto done;
    }

    /* 4. members cover the manifest and every payload */
    err = ReadCatalogDigests(catPath, &digests, &ndigests);
    if (err)
        goto done;
    if (!Covered(digests, ndigests, m.manifestSha256))
    {
        err = LN_E_CATALOG_INVALID;
        goto done;
    }
    for (i = 0; i < m.itemCount; i++)
    {
        if (!m.items[i].isDelete && !Covered(digests, ndigests, m.items[i].sha256))
        {
            err = LN_E_CATALOG_INVALID;
            goto done;
        }
    }

    /* 5. signer pinned */
    if (!signer[0] || strcmp(signer, m.certificateSha256) || strcmp(signer, pinnedCertSha256))
    {
        err = LN_E_CERT_MISMATCH;
        goto done;
    }

    /* 6. the packaging signature of the manifest */
    err = VerifyManifestSignature(certDer, certLen, pkgDir);

done:
    if (!err && out)
        *out = m;                                       /* ownership moves to the caller */
    else
        LnManifestFree(&m);
done_nomanifest:
    free(certDer);
    free(digests);
    free(manifestPath);
    free(catPath);
    free(payloadDir);
    return err;
}

DWORD LnServicingVerifyPackage(const WCHAR *pkgDir, const char *pinnedCertSha256, LN_MANIFEST *out)
{
    return VerifyCommon(NULL, pkgDir, pinnedCertSha256, out);
}

DWORD LnServicingVerifyStored(const LN_TARGET *t, const char *packageId, const char *version,
                              const char *pinnedCertSha256, LN_MANIFEST *out)
{
    WCHAR *wid = LnUtf8ToWide(packageId), *wver = LnUtf8ToWide(version), *rel, *dir;
    LN_MANIFEST m, installed;
    DWORD err;

    if (!wid || !wver)
    {
        free(wid);
        free(wver);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    rel = malloc((wcslen(wid) + wcslen(wver) + 32) * sizeof(WCHAR));
    if (!rel)
    {
        free(wid);
        free(wver);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    wcscpy(rel, L"store\\packages\\");
    wcscat(rel, wid);
    wcscat(rel, L"\\");
    wcscat(rel, wver);
    free(wid);
    free(wver);
    dir = LnTargetPath(t, rel);
    free(rel);
    if (!dir)
        return ERROR_NOT_ENOUGH_MEMORY;
    err = VerifyCommon(t, dir, pinnedCertSha256, &m);
    free(dir);
    if (err)
        return err;
    /* the manifest the engine reads (manifests\) must be the very one that was verified (packages\) */
    err = LnStoreLoadManifest(t, packageId, version, &installed);
    if (!err)
    {
        if (strcmp(installed.manifestSha256, m.manifestSha256))
            err = LN_E_CATALOG_INVALID;
        LnManifestFree(&installed);
    }
    if (!err && out)
        *out = m;
    else
        LnManifestFree(&m);
    return err;
}
