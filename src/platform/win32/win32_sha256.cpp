#include "platform/win32/win32_sha256.h"

#include <windows.h>

#include <bcrypt.h>

#include <array>

namespace cyella::win32 {
    std::string sha256_hex(const std::string_view bytes)
    {
        std::array<unsigned char, 32> digest {};
        // BCryptHash는 길이를 ULONG으로 받는다. 메일 주소 같은 짧은 입력만 쓰므로 넘치면 실패로 본다.
        if (bytes.size() > 0xFFFFFFFFu)
            return {};
        auto* const input { reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())) };
        if (BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, input, static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()))) == false)
            return {};
        constexpr std::string_view hex { "0123456789abcdef" };
        std::string text {};
        text.reserve(digest.size() * 2);
        for (const unsigned char value : digest)
        {
            text.push_back(hex[value >> 4]);
            text.push_back(hex[value & 0x0F]);
        }
        return text;
    }
} // namespace cyella::win32
