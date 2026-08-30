// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/moonlight/MoonlightPairingCrypto.h"

#include "Util/Hex.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstring>
#include <memory>

namespace dish::mooncrypto {
namespace {

using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using MdCtx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

X509Ptr certFromPem(const std::string& pem) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) { return {nullptr, X509_free}; }
    return {PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free};
}

PkeyPtr privateKeyFromPem(const std::string& pem) {
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) { return {nullptr, EVP_PKEY_free}; }
    return {PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free};
}

std::optional<Bytes> aesEcb(const std::array<std::uint8_t, kAesKeySize>& key,
                            const std::uint8_t* data, std::size_t len, bool encrypt) {
    if (data == nullptr || len == 0 || (len % kAesBlockSize) != 0) { return std::nullopt; }
    CipherCtx ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) { return std::nullopt; }
    const int initOk =
        encrypt ? EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr)
                : EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr);
    if (initOk != 1) { return std::nullopt; }
    if (EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1) { return std::nullopt; }

    Bytes out(len + kAesBlockSize);
    int outLen = 0;
    const int updateOk =
        encrypt ? EVP_EncryptUpdate(ctx.get(), out.data(), &outLen, data, static_cast<int>(len))
                : EVP_DecryptUpdate(ctx.get(), out.data(), &outLen, data, static_cast<int>(len));
    if (updateOk != 1) { return std::nullopt; }
    int finalLen = 0;
    const int finalOk = encrypt ? EVP_EncryptFinal_ex(ctx.get(), out.data() + outLen, &finalLen)
                                : EVP_DecryptFinal_ex(ctx.get(), out.data() + outLen, &finalLen);
    if (finalOk != 1) { return std::nullopt; }
    out.resize(static_cast<std::size_t>(outLen) + static_cast<std::size_t>(finalLen));
    return out;
}

} // namespace

std::array<std::uint8_t, kSha256Size> sha256(const std::uint8_t* data, std::size_t len) {
    std::array<std::uint8_t, kSha256Size> digest{};
    MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    unsigned int digestLen = 0;
    if (ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx.get(), data, len) == 1 &&
        EVP_DigestFinal_ex(ctx.get(), digest.data(), &digestLen) == 1) {
        return digest;
    }
    digest.fill(0);
    return digest;
}

bool randomBytes(std::uint8_t* out, std::size_t len) {
    return RAND_bytes(out, static_cast<int>(len)) == 1;
}

std::array<std::uint8_t, kAesKeySize>
derivePairingKey(const std::array<std::uint8_t, kPairingSaltSize>& salt, const std::string& pin) {
    Bytes material(salt.begin(), salt.end());
    material.insert(material.end(), pin.begin(), pin.end());
    const auto digest = sha256(material.data(), material.size());
    std::array<std::uint8_t, kAesKeySize> key{};
    std::memcpy(key.data(), digest.data(), kAesKeySize);
    return key;
}

std::optional<Bytes> aesEcbEncrypt(const std::array<std::uint8_t, kAesKeySize>& key,
                                   const std::uint8_t* data, std::size_t len) {
    return aesEcb(key, data, len, true);
}

std::optional<Bytes> aesEcbDecrypt(const std::array<std::uint8_t, kAesKeySize>& key,
                                   const std::uint8_t* data, std::size_t len) {
    return aesEcb(key, data, len, false);
}

std::optional<Bytes> rsaSignSha256(const std::string& privateKeyPem, const std::uint8_t* msg,
                                   std::size_t len) {
    PkeyPtr key = privateKeyFromPem(privateKeyPem);
    if (!key) { return std::nullopt; }
    MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestSignUpdate(ctx.get(), msg, len) != 1) {
        return std::nullopt;
    }
    std::size_t sigLen = 0;
    if (EVP_DigestSignFinal(ctx.get(), nullptr, &sigLen) != 1) { return std::nullopt; }
    Bytes sig(sigLen);
    if (EVP_DigestSignFinal(ctx.get(), sig.data(), &sigLen) != 1) { return std::nullopt; }
    sig.resize(sigLen);
    return sig;
}

bool rsaVerifySha256(const std::string& certPem, const std::uint8_t* msg, std::size_t len,
                     const std::uint8_t* sig, std::size_t sigLen) {
    X509Ptr cert = certFromPem(certPem);
    if (!cert) { return false; }
    PkeyPtr key(X509_get_pubkey(cert.get()), EVP_PKEY_free);
    if (!key) { return false; }
    MdCtx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestVerifyUpdate(ctx.get(), msg, len) != 1) {
        return false;
    }
    return EVP_DigestVerifyFinal(ctx.get(), sig, sigLen) == 1;
}

std::optional<ClientIdentity> generateClientIdentity() {
    // 2048-bit RSA, the size every Moonlight host expects (the pairing
    // signature length is pinned to 256 bytes).
    PkeyCtx keyCtx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!keyCtx || EVP_PKEY_keygen_init(keyCtx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(keyCtx.get(), 2048) <= 0) {
        return std::nullopt;
    }
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(keyCtx.get(), &rawKey) <= 0) { return std::nullopt; }
    PkeyPtr key(rawKey, EVP_PKEY_free);

    X509Ptr cert(X509_new(), X509_free);
    if (!cert) { return std::nullopt; }
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
    X509_set_version(cert.get(), 2);
    // Valid for 20 years; hosts deliberately tolerate clock skew on either end.
    constexpr long kValidSeconds = 630720000L;
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), kValidSeconds);
    X509_set_pubkey(cert.get(), key.get());

    X509_NAME* name = X509_get_subject_name(cert.get());
    const auto addEntry = [name](const char* field, const char* value) {
        X509_NAME_add_entry_by_txt(name, field, MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(value), -1, -1, 0);
    };
    addEntry("O", "TinkerNorth");
    addEntry("CN", "Dish Client");
    X509_set_issuer_name(cert.get(), name);

    if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) { return std::nullopt; }

    const auto pemOf = [](auto writeFn) -> std::optional<std::string> {
        BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
        if (!bio || writeFn(bio.get()) != 1) { return std::nullopt; }
        BUF_MEM* mem = nullptr;
        BIO_get_mem_ptr(bio.get(), &mem);
        if (mem == nullptr || mem->data == nullptr) { return std::nullopt; }
        return std::string(mem->data, mem->length);
    };

    const auto certPem = pemOf([&cert](BIO* bio) { return PEM_write_bio_X509(bio, cert.get()); });
    const auto keyPem = pemOf([&key](BIO* bio) {
        return PEM_write_bio_PrivateKey(bio, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    });
    if (!certPem || !keyPem) { return std::nullopt; }
    return ClientIdentity{*certPem, *keyPem};
}

std::optional<Bytes> certSignature(const std::string& certPem) {
    X509Ptr cert = certFromPem(certPem);
    if (!cert) { return std::nullopt; }
    const ASN1_BIT_STRING* sig = nullptr;
    X509_get0_signature(&sig, nullptr, cert.get());
    if (sig == nullptr || sig->data == nullptr || sig->length <= 0) { return std::nullopt; }
    return Bytes(sig->data, sig->data + sig->length);
}

std::optional<std::string> certFingerprintHex(const std::string& certPem) {
    X509Ptr cert = certFromPem(certPem);
    if (!cert) { return std::nullopt; }
    unsigned char* der = nullptr;
    const int derLen = i2d_X509(cert.get(), &der);
    if (derLen <= 0 || der == nullptr) { return std::nullopt; }
    const auto digest = sha256(der, static_cast<std::size_t>(derLen));
    OPENSSL_free(der);
    return util::toHex(digest.data(), digest.size());
}

bool isValidCertPem(const std::string& pem) { return static_cast<bool>(certFromPem(pem)); }

} // namespace dish::mooncrypto
