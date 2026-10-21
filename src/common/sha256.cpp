#include "common/sha256.h"

#include <Windows.h>
#include <bcrypt.h>
#include <vector>

bool Sha256File(const std::wstring& path, std::string& hex) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD received = 0;
    std::vector<BYTE> object;
    std::vector<BYTE> digest;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto cleanup;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize), &received, 0) < 0) goto cleanup;
    if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
            sizeof(hashSize), &received, 0) < 0) goto cleanup;
    if (objectSize == 0 || hashSize != 32) goto cleanup;

    object.resize(objectSize);
    digest.resize(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) goto cleanup;

    file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;

    {
        std::vector<BYTE> buffer(1u << 20);
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) goto cleanup;
            if (read == 0) break;
            if (BCryptHashData(hash, buffer.data(), read, 0) < 0) goto cleanup;
        }
    }

    if (BCryptFinishHash(hash, digest.data(), hashSize, 0) < 0) goto cleanup;

    static constexpr char digits[] = "0123456789abcdef";
    hex.resize(hashSize * 2);
    for (DWORD i = 0; i < hashSize; ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    ok = true;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}
