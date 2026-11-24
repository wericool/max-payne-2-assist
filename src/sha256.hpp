#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

inline std::string fileSha256(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    BCRYPT_HASH_HANDLE state = nullptr;
    bool ok = BCryptCreateHash(algorithm, &state, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<unsigned char, 65536> buffer{};
    while (ok && stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size()).gcount()) {
        ok = BCryptHashData(state, buffer.data(), static_cast<ULONG>(stream.gcount()), 0) >= 0;
    }
    std::array<unsigned char, 32> digest{};
    ok = ok && !stream.bad() && BCryptFinishHash(state, digest.data(), digest.size(), 0) >= 0;
    if (state) BCryptDestroyHash(state);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const auto value : digest) {
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return result;
}
