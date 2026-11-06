#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

inline void RequireParity(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
inline std::string Sha256(const std::vector<unsigned char>& bytes) {
    struct Algorithm { BCRYPT_ALG_HANDLE value = nullptr; ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); } } algorithm;
    struct Hash { BCRYPT_HASH_HANDLE value = nullptr; ~Hash() { if (value) BCryptDestroyHash(value); } } hash;
    auto check = [](NTSTATUS status) { RequireParity(status >= 0, "SHA256 operation failed"); };
    check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    check(BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0));
    RequireParity(bytes.size() <= (std::numeric_limits<ULONG>::max)(), "hash input too large");
    check(BCryptHashData(hash.value, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0));
    std::array<unsigned char, 32> digest{};
    check(BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0));
    std::string result; result.reserve(64);
    for (const auto byte : digest) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}
inline std::vector<unsigned char> LoadPinned(const std::filesystem::path& path, size_t size, const char* hash) {
    RequireParity(std::filesystem::file_size(path) == size, "pinned file size mismatch");
    std::ifstream stream(path, std::ios::binary);
    std::vector<unsigned char> bytes(size);
    RequireParity(bool(stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))), "pinned file read failed");
    RequireParity(stream.peek() == std::char_traits<char>::eof(), "pinned file grew");
    RequireParity(Sha256(bytes) == hash, "pinned file SHA256 mismatch");
    return bytes; // exact verified snapshot is used, never reopen for model execution
}
struct ParityMetrics {
    double maxAbsolute = 0, maxScaled = 0;
    size_t violations = 0, nonfinite = 0;
    bool Passed() const { return violations == 0 && nonfinite == 0; }
};
inline ParityMetrics CompareParity(const std::vector<float>& actual, const std::vector<float>& expected) {
    RequireParity(!actual.empty() && actual.size() == expected.size(), "parity size mismatch");
    ParityMetrics result;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!std::isfinite(actual[i]) || !std::isfinite(expected[i])) { ++result.nonfinite; continue; }
        const double absolute = std::abs(static_cast<double>(actual[i]) - expected[i]);
        const double scaled = absolute / (1.0 + std::abs(static_cast<double>(expected[i])));
        result.maxAbsolute = (std::max)(result.maxAbsolute, absolute);
        result.maxScaled = (std::max)(result.maxScaled, scaled);
        if (scaled > 1e-4) ++result.violations;
    }
    return result;
}