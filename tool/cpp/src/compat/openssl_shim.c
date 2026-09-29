/* OpenSSL 1.1 -> 3.x ABI 兼容垫片
 *
 * third_party/libssh 预编译静态库 (libssh.a) 是按 OpenSSL 1.1 头文件编译的，
 * 其中引用了以下符号：
 *   - EVP_PKEY_base_id / EVP_PKEY_size / EVP_PKEY_bits
 *     OpenSSL 3 中已退化为宏（#define EVP_PKEY_base_id EVP_PKEY_get_base_id），
 *     libcrypto 不再导出同名符号；
 *   - FIPS_mode
 *     OpenSSL 3 中已彻底移除（FIPS 由 provider 机制接管）。
 *
 * 此处补齐这些符号，语义与 1.1 完全一致：
 *   FIPS_mode() 返回 0 表示未处于 FIPS 模式（OpenSSL 3 默认无 FIPS provider）。
 */
#include <openssl/evp.h>

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
    return 0;
}
