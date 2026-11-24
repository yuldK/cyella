#include "platform/win32/win32_sha256.h"

#include <CommonCrypto/CommonDigest.h>

#include <array>

// `platform/win32/win32_sha256.h`의 macOS 구현이다 (CommonCrypto).
namespace cyella::win32 {
    std::string sha256_hex(const std::string_view bytes)
    {
        std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest {};
        // CC_SHA256은 길이를 CC_LONG(32비트)으로 받는다. 메일 주소 같은 짧은 입력만 쓰므로 넘치면 실패로 본다.
        if (bytes.size() > 0xFFFFFFFFu)
            return {};
        static_cast<void>(CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data()));
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
