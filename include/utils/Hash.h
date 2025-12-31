//
// Created by kirill on 12/31/25.
//

#ifndef ARTICLE_HASH_H
#define ARTICLE_HASH_H

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/core_names.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace article::hash {
    static std::string b64_encode(const uint8_t *data, size_t len) {
        std::string out;
        out.resize(4 * ((len + 2) / 3));
        int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                reinterpret_cast<const unsigned char *>(data),
                                static_cast<int>(len));
        if (n < 0) throw std::runtime_error("EVP_EncodeBlock failed");
        out.resize(static_cast<size_t>(n));
        return out;
    }

    static std::vector<uint8_t> b64_decode(std::string_view s) {
        std::vector<uint8_t> out(3 * (s.size() / 4) + 3);
        int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                reinterpret_cast<const unsigned char *>(s.data()),
                                static_cast<int>(s.size()));
        if (n < 0) throw std::runtime_error("EVP_DecodeBlock failed");

        size_t pad = 0;
        if (!s.empty() && s.back() == '=') pad++;
        if (s.size() > 1 && s[s.size() - 2] == '=') pad++;

        size_t real = static_cast<size_t>(n);
        if (pad) real -= pad;
        out.resize(real);
        return out;
    }

    static std::vector<std::string_view> split_sv(std::string_view s, char d) {
        std::vector<std::string_view> parts;
        size_t start = 0;
        while (start <= s.size()) {
            size_t pos = s.find(d, start);
            if (pos == std::string_view::npos) {
                parts.push_back(s.substr(start));
                break;
            }
            parts.push_back(s.substr(start, pos - start));
            start = pos + 1;
        }
        return parts;
    }

    struct KdfDeleter {
        void operator()(EVP_KDF *p) const noexcept { EVP_KDF_free(p); }
    };

    struct KdfCtxDeleter {
        void operator()(EVP_KDF_CTX *p) const noexcept { EVP_KDF_CTX_free(p); }
    };

    static std::vector<uint8_t> pbkdf2_hmac_sha256(std::string_view password,
                                                   const uint8_t *salt, size_t salt_len,
                                                   uint32_t iterations,
                                                   size_t dk_len) {
        std::unique_ptr<EVP_KDF, KdfDeleter> kdf(EVP_KDF_fetch(nullptr, "PBKDF2", nullptr));
        if (!kdf) throw std::runtime_error("EVP_KDF_fetch(PBKDF2) failed");

        std::unique_ptr<EVP_KDF_CTX, KdfCtxDeleter> ctx(EVP_KDF_CTX_new(kdf.get()));
        if (!ctx) throw std::runtime_error("EVP_KDF_CTX_new failed");

        char digest_name[] = "SHA256";
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                              const_cast<char *>(password.data()),
                                              password.size()),
            OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                              const_cast<uint8_t *>(salt),
                                              salt_len),
            OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
            OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                             digest_name,
                                             0),
            OSSL_PARAM_construct_end()
        };

        std::vector<uint8_t> out(dk_len);
        if (EVP_KDF_derive(ctx.get(), out.data(), out.size(), params) != 1)
            throw std::runtime_error("EVP_KDF_derive failed");

        return out;
    }

    // pbkdf2$sha256$iters$salt_b64$dk_b64
    static std::string hash_password(std::string_view password,
                                     uint32_t iterations = 200'000,
                                     size_t salt_len = 16,
                                     size_t dk_len = 32) {
        std::vector<uint8_t> salt(salt_len);
        if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
            throw std::runtime_error("RAND_bytes failed");

        auto dk = pbkdf2_hmac_sha256(password, salt.data(), salt.size(), iterations, dk_len);

        return "pbkdf2$sha256$" + std::to_string(iterations) + "$" +
               b64_encode(salt.data(), salt.size()) + "$" +
               b64_encode(dk.data(), dk.size());
    }

    static bool verify_password(std::string_view password, std::string_view stored) {
        auto parts = split_sv(stored, '$');
        if (parts.size() != 5) return false;
        if (parts[0] != "pbkdf2") return false;
        if (parts[1] != "sha256") return false;

        uint32_t iterations = 0;
        try { iterations = static_cast<uint32_t>(std::stoul(std::string(parts[2]))); } catch (...) { return false; }
        if (iterations == 0) return false;

        std::vector<uint8_t> salt, expected;
        try {
            salt = b64_decode(parts[3]);
            expected = b64_decode(parts[4]);
        } catch (...) {
            return false;
        }

        auto dk = pbkdf2_hmac_sha256(password, salt.data(), salt.size(), iterations, expected.size());
        if (dk.size() != expected.size()) return false;

        return CRYPTO_memcmp(dk.data(), expected.data(), dk.size()) == 0;
    }
}

#endif //ARTICLE_HASH_H
