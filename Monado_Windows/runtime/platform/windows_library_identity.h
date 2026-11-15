// [XCDE] WINPARITY-004: local PE identity checks before executable mapping.
// The build receipt binds these sidecars to the input/source/compiler closure.
// SHA256 detects a changed/corrupt local companion; it is not a publisher signature.
#pragma once
#ifdef _WIN32
#include "windows_save_io.h"
#include <bcrypt.h>
#include <aclapi.h>

static bool wl_owned(HANDLE file) {
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    HANDLE token = NULL;
    DWORD size = 0;
    bool valid = false;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            &owner, NULL, NULL, NULL, &descriptor) != ERROR_SUCCESS) return false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        GetTokenInformation(token, TokenUser, NULL, 0, &size);
        TOKEN_USER *user = size && size <= 65536 ? (TOKEN_USER *)malloc(size) : NULL;
        if (user && GetTokenInformation(token, TokenUser, user, size, &size))
            valid = owner && EqualSid(owner, user->User.Sid);
        free(user);
        CloseHandle(token);
    }
    LocalFree(descriptor);
    return valid;
}

static bool wl_read_hex(const char *path, char result[65]) {
    WsGuard guard;
    if (ws_guard_open(&guard, path)) return false;
    HANDLE file = CreateFileW(guard.path, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    LARGE_INTEGER size;
    DWORD count = 0;
    char bytes[66] = {0};
    bool valid = file != INVALID_HANDLE_VALUE && ws_plain(file, false, true) && wl_owned(file) &&
        GetFileSizeEx(file, &size) && (size.QuadPart == 64 || size.QuadPart == 65) &&
        ReadFile(file, bytes, sizeof bytes, &count, NULL) && count == size.QuadPart;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    ws_guard_close(&guard);
    if (!valid || (count == 65 && bytes[64] != '\n')) return false;
    for (unsigned i = 0; i < 64; ++i)
        if (!((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f'))) return false;
    memcpy(result, bytes, 64);
    result[64] = 0;
    return true;
}

// Call with the ordinary file locked against write/delete, and retain that same
// handle until LoadLibraryExW has mapped the checked pathname. No reopen race.
static bool wl_hash_matches(HANDLE file, const char expected[65]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    LARGE_INTEGER size, zero = {0};
    unsigned char data[65536], digest[32];
    DWORD count;
    bool valid = false;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 2ll * 1024 * 1024 * 1024 ||
        !SetFilePointerEx(file, zero, NULL, FILE_BEGIN) ||
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) < 0) goto done;
    LONGLONG remaining = size.QuadPart;
    while (remaining) {
        DWORD wanted = remaining < (LONGLONG)sizeof data ? (DWORD)remaining : (DWORD)sizeof data;
        if (!ReadFile(file, data, wanted, &count, NULL) || count != wanted ||
            BCryptHashData(hash, data, count, 0) < 0) goto done;
        remaining -= count;
    }
    if (BCryptFinishHash(hash, digest, sizeof digest, 0) < 0) goto done;
    static const char hex[] = "0123456789abcdef";
    valid = true;
    for (unsigned i = 0; i < sizeof digest; ++i)
        if (expected[2 * i] != hex[digest[i] >> 4] || expected[2 * i + 1] != hex[digest[i] & 15]) valid = false;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return valid;
}
#endif
