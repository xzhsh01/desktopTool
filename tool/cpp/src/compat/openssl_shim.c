/*
 * OpenSSL ABI compatibility shim for libssh (built against OpenSSL 1.1 headers).
 *
 * libssh.a was compiled against OpenSSL 1.1 and references:
 *   - EVP_PKEY_base_id / EVP_PKEY_size / EVP_PKEY_bits
 *     In OpenSSL 1.1 these are real exported functions.
 *     In OpenSSL 3.x they became macros around EVP_PKEY_get_base_id etc.,
 *     and libcrypto no longer exports the 1.1 symbol names.
 *   - FIPS_mode
 *     Removed in OpenSSL 3.x (FIPS is now a provider).
 *
 * Strategy:
 *   - Detect OpenSSL version via OPENSSL_VERSION_NUMBER macro.
 *   - On OpenSSL >= 3.0: provide 1.1-compatible wrapper functions that
 *     delegate to the OpenSSL 3 API, and a stub FIPS_mode returning 0.
 *   - On OpenSSL < 3.0 (1.1.x): provide nothing here; the real symbols
 *     from libcrypto are linked directly. The file compiles to an empty TU.
 */
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L

#undef EVP_PKEY_base_id
#undef EVP_PKEY_size
#undef EVP_PKEY_bits

int EVP_PKEY_base_id(const EVP_PKEY *pkey)
{
    return EVP_PKEY_get_base_id(pkey);
}

int EVP_PKEY_size(const EVP_PKEY *pkey)
{
    return EVP_PKEY_get_size(pkey);
}

int EVP_PKEY_bits(const EVP_PKEY *pkey)
{
    return EVP_PKEY_get_bits(pkey);
}

int FIPS_mode(void)
{
    /* OpenSSL 3 default: no FIPS provider loaded -> not in FIPS mode. */
    return 0;
}

#endif /* OPENSSL_VERSION_NUMBER >= 3.0 */