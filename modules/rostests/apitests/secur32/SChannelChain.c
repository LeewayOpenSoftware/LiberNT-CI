/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     A TLS handshake must validate when the server sends a chain
 *              whose intermediate exists only in the handshake (no store,
 *              no AIA). Windows passes the peer certificates to the chain
 *              engine as hAdditionalStore; schannel must do the same.
 * COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
 */

#define WIN32_NO_STATUS

#include <apitest.h>
#include <windef.h>
#include <wincrypt.h>
#include <sspi.h>
#include <schannel.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <psa/crypto.h>

#define TEST_ROOT_NAME   L"CN=Orosi Schannel Chain Test Root"
#define TEST_INTER_NAME  L"CN=Orosi Schannel Chain Test Intermediate"
#define TEST_LEAF_NAME   L"CN=localhost"

#define FILETIME_DAY (24ULL * 60 * 60 * 10000000)

#define IO_BUF_SIZE 65536
#define PUMP_MAX_ROUNDS 128

#include "leafkey.inc"
#include "leafpub.inc"

struct byte_queue
{
    BYTE data[IO_BUF_SIZE];
    SIZE_T start;
    SIZE_T len;
};

struct handshake_pump
{
    CredHandle cred;
    CtxtHandle ctx;
    BOOL ctx_ready;
    BOOL incomplete;
    SEC_WCHAR target[32];
    struct byte_queue *to_server;   /* client tokens waiting for the server */
    struct byte_queue *from_server; /* server tokens waiting for the client */
    BYTE in_buf[IO_BUF_SIZE];       /* server bytes not yet consumed by ISC */
    SIZE_T in_len;
    SECURITY_STATUS status;
    BOOL done;
    INT rounds;
};

struct io_bridge
{
    struct byte_queue *to_server;
    struct byte_queue *from_server;
    struct handshake_pump *pump;
};

static void
queue_reset(
    _Out_ struct byte_queue *q)
{
    q->start = 0;
    q->len = 0;
}

static BOOL
queue_push(
    _Inout_ struct byte_queue *q,
    _In_reads_bytes_(len) const void *buf,
    _In_ SIZE_T len)
{
    if (q->len + len > sizeof(q->data))
        return FALSE;
    if (q->start + q->len + len > sizeof(q->data))
    {
        memmove(q->data, q->data + q->start, q->len);
        q->start = 0;
    }
    memcpy(q->data + q->start + q->len, buf, len);
    q->len += len;
    return TRUE;
}

static SIZE_T
queue_pop(
    _Inout_ struct byte_queue *q,
    _Out_writes_bytes_to_(len, return) void *buf,
    _In_ SIZE_T len)
{
    SIZE_T take = q->len < len ? q->len : len;

    memcpy(buf, q->data + q->start, take);
    q->start += take;
    q->len -= take;
    if (q->len == 0)
        q->start = 0;
    return take;
}

static FILETIME
file_time_from_now(
    _In_ LONGLONG days)
{
    FILETIME now;
    ULARGE_INTEGER value;

    GetSystemTimeAsFileTime(&now);
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    value.QuadPart += (ULONGLONG)days * FILETIME_DAY;
    now.dwLowDateTime = value.LowPart;
    now.dwHighDateTime = value.HighPart;
    return now;
}

/* Sign a certificate for Subject with the AT_SIGNATURE key of sign_prov.
 * Basic constraints always; the leaf adds server-auth EKU and a localhost
 * SAN. When forced_pub is given it supplies the subject public half and
 * subject_prov is ignored (the leaf uses the embedded test key, whose
 * private half never needs exporting). Returns an independent context. */
static PCCERT_CONTEXT
make_cert(
    _In_ HCRYPTPROV sign_prov,
    _In_ HCRYPTPROV subject_prov,
    _In_ PCWSTR subject,
    _In_ PCWSTR issuer,
    _In_ BYTE serial,
    _In_ BOOL is_ca,
    _In_ BOOL add_server_auth,
    _In_opt_ PCERT_PUBLIC_KEY_INFO forced_pub)
{
    BYTE subject_buf[256], issuer_buf[256], bc_buf[32], pub_buf[1024];
    BYTE eku_buf[64], san_buf[256], cert_buf[2048];
    DWORD subject_size = sizeof(subject_buf), issuer_size = sizeof(issuer_buf);
    DWORD bc_size = sizeof(bc_buf), pub_size = sizeof(pub_buf);
    DWORD eku_size = sizeof(eku_buf), san_size = sizeof(san_buf);
    DWORD cert_size = sizeof(cert_buf);
    CERT_BASIC_CONSTRAINTS2_INFO bc;
    CERT_EXTENSION exts[3];
    CERT_ALT_NAME_INFO san;
    CERT_ALT_NAME_ENTRY san_entry;
    CERT_ENHKEY_USAGE eku;
    LPSTR usage;
    CERT_INFO info;
    BYTE serial_bytes[1];

    if (!CertStrToNameW(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, NULL,
                        subject_buf, &subject_size, NULL) ||
        !CertStrToNameW(X509_ASN_ENCODING, issuer, CERT_X500_NAME_STR, NULL,
                        issuer_buf, &issuer_size, NULL))
    {
        trace("name encoding failed: %lu\n", GetLastError());
        return NULL;
    }
    if (forced_pub)
    {
        pub_size = 0;
    }
    else if (!CryptExportPublicKeyInfo(subject_prov, AT_SIGNATURE, X509_ASN_ENCODING,
                                       (PCERT_PUBLIC_KEY_INFO)pub_buf, &pub_size))
    {
        trace("key encoding failed: %lu\n", GetLastError());
        return NULL;
    }

    bc.fCA = is_ca;
    bc.fPathLenConstraint = FALSE;
    bc.dwPathLenConstraint = 0;
    if (!CryptEncodeObject(X509_ASN_ENCODING, X509_BASIC_CONSTRAINTS2, &bc,
                           bc_buf, &bc_size))
        return NULL;

    exts[0].pszObjId = (LPSTR)szOID_BASIC_CONSTRAINTS2;
    exts[0].fCritical = TRUE;
    exts[0].Value.cbData = bc_size;
    exts[0].Value.pbData = bc_buf;

    memset(&info, 0, sizeof(info));
    info.cExtension = 1;
    info.rgExtension = exts;

    if (add_server_auth)
    {
        usage = (LPSTR)szOID_PKIX_KP_SERVER_AUTH;
        eku.cUsageIdentifier = 1;
        eku.rgpszUsageIdentifier = &usage;
        if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku,
                               eku_buf, &eku_size))
            return NULL;
        exts[1].pszObjId = (LPSTR)szOID_ENHANCED_KEY_USAGE;
        exts[1].fCritical = FALSE;
        exts[1].Value.cbData = eku_size;
        exts[1].Value.pbData = eku_buf;

        san_entry.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
        san_entry.pwszDNSName = (LPWSTR)L"localhost";
        san.cAltEntry = 1;
        san.rgAltEntry = &san_entry;
        if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ALTERNATE_NAME, &san,
                               san_buf, &san_size))
            return NULL;
        exts[2].pszObjId = (LPSTR)szOID_SUBJECT_ALT_NAME2;
        exts[2].fCritical = FALSE;
        exts[2].Value.cbData = san_size;
        exts[2].Value.pbData = san_buf;

        info.cExtension = 3;
    }

    serial_bytes[0] = serial;
    info.dwVersion = CERT_V3;
    info.SerialNumber.cbData = sizeof(serial_bytes);
    info.SerialNumber.pbData = serial_bytes;
    info.SignatureAlgorithm.pszObjId = (LPSTR)szOID_RSA_SHA256RSA;
    info.Issuer.cbData = issuer_size;
    info.Issuer.pbData = issuer_buf;
    info.NotBefore = file_time_from_now(-1);
    info.NotAfter = file_time_from_now(30);
    info.Subject.cbData = subject_size;
    info.Subject.pbData = subject_buf;
    if (forced_pub)
    {
        info.SubjectPublicKeyInfo.Algorithm.pszObjId = (LPSTR)szOID_RSA_RSA;
        info.SubjectPublicKeyInfo.Algorithm.Parameters.cbData = 2;
        info.SubjectPublicKeyInfo.Algorithm.Parameters.pbData = (BYTE *)"\x05\x00";
        info.SubjectPublicKeyInfo.PublicKey.cbData = sizeof(LeafPublicKey);
        info.SubjectPublicKeyInfo.PublicKey.pbData = (BYTE *)LeafPublicKey;
        info.SubjectPublicKeyInfo.PublicKey.cUnusedBits = 0;
    }
    else
    {
        info.SubjectPublicKeyInfo = *(PCERT_PUBLIC_KEY_INFO)pub_buf;
    }

    if (!CryptSignAndEncodeCertificate(sign_prov, AT_SIGNATURE, X509_ASN_ENCODING,
                                       X509_CERT_TO_BE_SIGNED, &info,
                                       &info.SignatureAlgorithm, NULL,
                                       cert_buf, &cert_size))
    {
        trace("signing %S failed: %lu\n", subject, GetLastError());
        return NULL;
    }
    return CertCreateCertificateContext(X509_ASN_ENCODING, cert_buf, cert_size);
}

/* Feed the pending server tokens to InitializeSecurityContext and queue the
 * client output for the server. Stops when the client needs more input, and
 * records the terminal status. Mirrors the wininet manual handshake loop. */
static VOID
drive_client(
    _Inout_ struct handshake_pump *p)
{
    SecBuffer out_buf;
    SecBufferDesc out_desc = { SECBUFFER_VERSION, 1, &out_buf };
    SecBuffer in_bufs[2];
    SecBufferDesc in_desc = { SECBUFFER_VERSION, 2, in_bufs };
    ULONG attrs;
    SIZE_T fed;
    SIZE_T pulled;
    BOOL have_input;

    if (p->done)
        return;

    for (;;)
    {
        pulled = 0;
        while (p->from_server->len && p->in_len < sizeof(p->in_buf))
        {
            SIZE_T room = sizeof(p->in_buf) - p->in_len;
            SIZE_T take = queue_pop(p->from_server, p->in_buf + p->in_len, room);

            p->in_len += take;
            pulled += take;
        }
        have_input = p->in_len != 0;

        /* The first call emits the ClientHello with no server input yet. */
        if (!have_input && p->ctx_ready)
            return;
        if (p->incomplete && pulled == 0)
            return;

        in_bufs[0].BufferType = SECBUFFER_TOKEN;
        in_bufs[0].cbBuffer = (ULONG)p->in_len;
        in_bufs[0].pvBuffer = have_input ? p->in_buf : NULL;
        in_bufs[1].BufferType = SECBUFFER_EMPTY;
        in_bufs[1].cbBuffer = 0;
        in_bufs[1].pvBuffer = NULL;
        out_buf.BufferType = SECBUFFER_TOKEN;
        out_buf.cbBuffer = 0;
        out_buf.pvBuffer = NULL;
        fed = p->in_len;

        p->status = InitializeSecurityContextW(
            &p->cred, p->ctx_ready ? &p->ctx : NULL, p->target,
            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY |
            ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT,
            0, 0, p->ctx_ready ? &in_desc : NULL, 0,
            &p->ctx, &out_desc, &attrs, NULL);
        p->ctx_ready = TRUE;
        p->incomplete = FALSE;
        p->in_len = 0;
        trace("ISC fed=%lu -> 0x%lx\n", (ULONG)fed, p->status);

        if (out_buf.cbBuffer)
        {
            if (!queue_push(p->to_server, out_buf.pvBuffer, out_buf.cbBuffer))
                trace("client token overflowed the test queue\n");
            FreeContextBuffer(out_buf.pvBuffer);
        }

        if (p->status == SEC_E_OK)
        {
            p->done = TRUE;
            return;
        }
        if (p->status == SEC_I_CONTINUE_NEEDED)
        {
            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer)
            {
                memmove(p->in_buf, p->in_buf + fed - in_bufs[1].cbBuffer,
                        in_bufs[1].cbBuffer);
                p->in_len = in_bufs[1].cbBuffer;
            }
            continue;
        }
        if (p->status == SEC_E_INCOMPLETE_MESSAGE)
        {
            p->incomplete = TRUE;
            p->in_len = fed;
            continue;
        }

        p->done = TRUE;
        return;
    }
}

/* Feed the client any server flight still queued once the server side has
 * finished: in TLS 1.2 the server's CCS+Finished arrives after the client
 * sent its own, and only consuming it lets ISC return SEC_E_OK. In TLS 1.3
 * the client is already done after sending its Finished. */
static VOID
finish_client(
    _Inout_ struct handshake_pump *p)
{
    INT rounds = 0;

    while (!p->done && p->from_server->len && rounds++ < PUMP_MAX_ROUNDS)
    {
        p->rounds++;
        drive_client(p);
    }
}

static int
bio_send(
    _In_ void *ctx,
    _In_ const unsigned char *buf,
    _In_ size_t len)
{
    struct io_bridge *io = ctx;

    if (!queue_push(io->from_server, buf, len))
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)len;
}

static int
bio_recv(
    _In_ void *ctx,
    _In_ unsigned char *buf,
    _In_ size_t len)
{
    struct io_bridge *io = ctx;
    struct handshake_pump *p = io->pump;
    SIZE_T take;

    while (io->to_server->len == 0 && !p->done && p->rounds < PUMP_MAX_ROUNDS)
    {
        p->rounds++;
        drive_client(p);
    }
    if (io->to_server->len == 0)
    {
        /* All data flows through this callback and the pump; running dry
         * means the handshake stalled, and a silent WANT_READ loop would
         * hang the test. Fail loudly instead. */
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    take = queue_pop(io->to_server, buf, len);
    return (int)take;
}

/* Import the test leaf key for the mbedTLS server from the embedded
 * PRIVATEKEYBLOB. CAPI stores the big integers little-endian; mbedTLS
 * wants big-endian, so every field is reversed. pk must be initialized by
 * the caller and is left valid on success only. */
static int
import_server_key(
    _Inout_ mbedtls_pk_context *pk)
{
    static const BYTE pub_exp_65537[] = { 0x01, 0x00, 0x01 };
    BYTE be_mod[256], be_p[128], be_q[128], be_d[256];
    const BYTE *n, *p1, *p2, *d;
    const RSAPUBKEY *rsa_pub;
    DWORD n_len = 2048 / 8, p_len = 2048 / 16, i;

    rsa_pub = (const RSAPUBKEY *)(LeafKeyBlob + sizeof(BLOBHEADER));
    if (((const BLOBHEADER *)LeafKeyBlob)->bType != PRIVATEKEYBLOB ||
        rsa_pub->magic != 0x32415352 /* RSA2 */ ||
        rsa_pub->bitlen != 2048 || rsa_pub->pubexp != 65537)
        return -1;

    n = (const BYTE *)(rsa_pub + 1);
    p1 = n + n_len;
    p2 = p1 + p_len;
    /* skip dp, dq, iq */
    d = p2 + p_len + 3 * p_len;

    for (i = 0; i < n_len; i++) be_mod[i] = n[n_len - 1 - i];
    for (i = 0; i < p_len; i++) be_p[i] = p1[p_len - 1 - i];
    for (i = 0; i < p_len; i++) be_q[i] = p2[p_len - 1 - i];
    for (i = 0; i < n_len; i++) be_d[i] = d[n_len - 1 - i];

    if (mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
    {
        trace("pk_setup failed");
        return -1;
    }
    if (mbedtls_rsa_import_raw(mbedtls_pk_rsa(*pk),
                               be_mod, sizeof(be_mod),
                               be_p, sizeof(be_p),
                               be_q, sizeof(be_q),
                               be_d, sizeof(be_d),
                               pub_exp_65537, sizeof(pub_exp_65537)) != 0)
    {
        trace("rsa_import_raw failed");
        return -1;
    }
    if (mbedtls_rsa_complete(mbedtls_pk_rsa(*pk)) != 0)
    {
        trace("rsa_complete failed");
        return -1;
    }
    return 0;
}

static VOID
free_cert(
    _Inout_ PCCERT_CONTEXT *cert)
{
    if (*cert)
    {
        CertFreeCertificateContext(*cert);
        *cert = NULL;
    }
}

START_TEST(SChannelChain)
{
    HCRYPTPROV prov_root = 0, prov_inter = 0;
    HCRYPTKEY key_root = 0, key_inter = 0;
    PCCERT_CONTEXT root = NULL, inter = NULL, leaf = NULL;
    HCERTSTORE root_store = NULL;
    PCCERT_CONTEXT staged = NULL;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt chain;
    mbedtls_pk_context pk;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    struct handshake_pump pump;
    struct io_bridge io;
    struct byte_queue c2s, s2c;
    SECURITY_STATUS ss;
    PCCERT_CONTEXT remote = NULL;
    CERT_PUBLIC_KEY_INFO leaf_pub;
    INT ret;

    memset(&pump, 0, sizeof(pump));
    memset(&io, 0, sizeof(io));
    memset(&leaf_pub, 0, sizeof(leaf_pub));
    queue_reset(&c2s);
    queue_reset(&s2c);
    io.to_server = &c2s;
    io.from_server = &s2c;
    io.pump = &pump;
    pump.to_server = &c2s;
    pump.from_server = &s2c;
    memcpy(pump.target, L"localhost", sizeof(L"localhost"));
    SecInvalidateHandle(&pump.cred);
    SecInvalidateHandle(&pump.ctx);

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&chain);
    mbedtls_pk_init(&pk);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);

    if (!CryptAcquireContextW(&prov_root, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT) ||
        !CryptGenKey(prov_root, AT_SIGNATURE, (2048 << 16) | CRYPT_EXPORTABLE, &key_root) ||
        !CryptAcquireContextW(&prov_inter, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT) ||
        !CryptGenKey(prov_inter, AT_SIGNATURE, (2048 << 16) | CRYPT_EXPORTABLE, &key_inter))
    {
        skip("no RSA provider for the local chain: %lu\n", GetLastError());
        goto cleanup;
    }

    root = make_cert(prov_root, prov_root, TEST_ROOT_NAME, TEST_ROOT_NAME, 1, TRUE, FALSE, NULL);
    inter = make_cert(prov_root, prov_inter, TEST_INTER_NAME, TEST_ROOT_NAME, 2, TRUE, FALSE, NULL);
    leaf = make_cert(prov_inter, 0, TEST_LEAF_NAME, TEST_INTER_NAME, 3, FALSE, TRUE, &leaf_pub);
    ok(root && inter && leaf, "could not build the test certificates\n");
    if (!root || !inter || !leaf)
        goto cleanup;

    /* Trust only the root; the intermediate exists solely in the handshake.
     * The CurrentUser Root store is writable without elevation and the
     * default chain engine merges it with the machine Root store. */
    root_store = CertOpenSystemStoreW(0, L"ROOT");
    if (!root_store ||
        !CertAddCertificateContextToStore(root_store, root, CERT_STORE_ADD_ALWAYS, &staged))
    {
        skip("cannot stage the test root in the Root store: %lu\n", GetLastError());
        goto cleanup;
    }

    ss = AcquireCredentialsHandleW(NULL, (SEC_WCHAR *)SCHANNEL_NAME_W,
                                   SECPKG_CRED_OUTBOUND, NULL, NULL, NULL, NULL,
                                   &pump.cred, NULL);
    ok(ss == SEC_E_OK, "AcquireCredentialsHandle failed: 0x%lx\n", ss);
    if (ss != SEC_E_OK)
        goto cleanup;

    if (import_server_key(&pk) != 0)
    {
        skip("cannot import the leaf key\n");
        goto cleanup;
    }

    psa_crypto_init();
    ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    ok(ret == 0, "ctr_drbg_seed failed: %d\n", ret);
    if (ret != 0)
        goto cleanup;

    ret = mbedtls_x509_crt_parse_der(&chain, leaf->pbCertEncoded, leaf->cbCertEncoded);
    if (ret == 0)
        ret = mbedtls_x509_crt_parse_der(&chain, inter->pbCertEncoded, inter->cbCertEncoded);
    ok(ret == 0, "x509_crt_parse_der failed: %d\n", ret);
    if (ret != 0)
        goto cleanup;

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    ok(ret == 0, "ssl_config_defaults failed: %d\n", ret);
    if (ret != 0)
        goto cleanup;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    ret = mbedtls_ssl_conf_own_cert(&conf, &chain, &pk);
    ok(ret == 0, "ssl_conf_own_cert failed: %d\n", ret);
    if (ret != 0)
        goto cleanup;

    ret = mbedtls_ssl_setup(&ssl, &conf);
    ok(ret == 0, "ssl_setup failed: %d\n", ret);
    if (ret != 0)
        goto cleanup;
    mbedtls_ssl_set_bio(&ssl, &io, bio_send, bio_recv, NULL);

    /* Runs to completion; bio_recv drives the secur32 client between steps. */
    ret = mbedtls_ssl_handshake(&ssl);
    finish_client(&pump);

    ok(pump.done, "client handshake never finished (server ret %d)\n", ret);
    ok(pump.status == SEC_E_OK,
       "client status 0x%lx, expected SEC_E_OK; the peer chain was not used\n",
       pump.status);
    ok(ret == 0, "server handshake failed: %d\n", ret);
    if (pump.status == SEC_E_OK)
    {
        ss = QueryContextAttributesW(&pump.ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote);
        ok(ss == SEC_E_OK, "QueryContextAttributes(REMOTE_CERT) failed: 0x%lx\n", ss);
        if (ss == SEC_E_OK)
        {
            ok(remote->cbCertEncoded == leaf->cbCertEncoded &&
               !memcmp(remote->pbCertEncoded, leaf->pbCertEncoded, leaf->cbCertEncoded),
               "remote certificate is not the leaf the server sent\n");
            CertFreeCertificateContext(remote);
        }
    }

cleanup:
    if (pump.ctx_ready)
        DeleteSecurityContext(&pump.ctx);
    if (SecIsValidHandle(&pump.cred))
        FreeCredentialHandle(&pump.cred);
    if (staged)
    {
        CertDeleteCertificateFromStore(staged);
        staged = NULL;
    }
    if (root_store)
        CertCloseStore(root_store, 0);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&chain);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    free_cert(&leaf);
    free_cert(&inter);
    free_cert(&root);
    if (key_inter)
        CryptDestroyKey(key_inter);
    if (key_root)
        CryptDestroyKey(key_root);
    if (prov_inter)
        CryptReleaseContext(prov_inter, 0);
    if (prov_root)
        CryptReleaseContext(prov_root, 0);
}
