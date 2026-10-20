#include "domain/commit_signature.h"

namespace cyella {
    signature_status signature_status_from_code(const char code) noexcept
    {
        switch (code)
        {
        case 'N':
            return signature_status::none;
        case 'G':
            return signature_status::good;
        case 'B':
            return signature_status::bad;
        case 'U':
            return signature_status::unknown_key;
        case 'X':
            return signature_status::expired_signature;
        case 'Y':
            return signature_status::expired_key;
        case 'R':
            return signature_status::revoked_key;
        case 'E':
            break;
        default:
            break;
        }
        return signature_status::cannot_check;
    }

    char signature_status_code(const signature_status status) noexcept
    {
        switch (status)
        {
        case signature_status::none:
            return 'N';
        case signature_status::good:
            return 'G';
        case signature_status::bad:
            return 'B';
        case signature_status::unknown_key:
            return 'U';
        case signature_status::expired_signature:
            return 'X';
        case signature_status::expired_key:
            return 'Y';
        case signature_status::revoked_key:
            return 'R';
        case signature_status::cannot_check:
            break;
        }
        return 'E';
    }

    std::string_view signature_status_name(const signature_status status) noexcept
    {
        switch (status)
        {
        case signature_status::none:
            return "none";
        case signature_status::good:
            return "good";
        case signature_status::bad:
            return "bad";
        case signature_status::unknown_key:
            return "unknown_key";
        case signature_status::expired_signature:
            return "expired_signature";
        case signature_status::expired_key:
            return "expired_key";
        case signature_status::revoked_key:
            return "revoked_key";
        case signature_status::cannot_check:
            break;
        }
        return "cannot_check";
    }

    bool is_signed(const signature_status status) noexcept
    {
        return status != signature_status::none;
    }

    std::u8string_view signature_badge_text(const signature_status status) noexcept
    {
        switch (status)
        {
        case signature_status::none:
            return u8"서명 없음";
        case signature_status::good:
            return u8"서명 확인됨";
        case signature_status::bad:
            return u8"서명이 올바르지 않음";
        case signature_status::unknown_key:
            return u8"알 수 없는 키";
        case signature_status::expired_signature:
            return u8"서명 만료";
        case signature_status::expired_key:
            return u8"키 만료";
        case signature_status::revoked_key:
            return u8"키 폐기됨";
        case signature_status::cannot_check:
            break;
        }
        return u8"확인할 수 없음";
    }

    std::u8string signature_badge_label(const commit_signature& signature)
    {
        if (signature.note == signature_note::allowed_signers_missing)
            return u8"확인할 수 없음 (allowed signers 없음)";
        return std::u8string { signature_badge_text(signature.status) };
    }

    std::u8string signature_description(const commit_signature& signature)
    {
        std::u8string text {};
        switch (signature.status)
        {
        case signature_status::none:
            return text;
        case signature_status::good:
            text = u8"올바른 서명입니다.";
            break;
        case signature_status::bad:
            text = u8"서명이 커밋 내용과 맞지 않습니다. 커밋이 바뀌었거나 서명이 깨졌습니다.";
            break;
        case signature_status::unknown_key:
            text = u8"서명은 올바르지만 서명한 키를 신뢰할 수 없습니다.";
            break;
        case signature_status::expired_signature:
            text = u8"서명의 유효 기간이 지났습니다.";
            break;
        case signature_status::expired_key:
            text = u8"서명한 키의 유효 기간이 지났습니다.";
            break;
        case signature_status::revoked_key:
            text = u8"서명한 키가 폐기되었습니다.";
            break;
        case signature_status::cannot_check:
            text = u8"서명을 확인하지 못했습니다. 서명한 사람의 공개 키가 없거나 서명 도구를 실행할 수 없습니다.";
            break;
        }
        if (signature.note == signature_note::allowed_signers_missing)
            text = u8"ssh 서명의 서명자를 대조할 allowed signers 파일(gpg.ssh.allowedSignersFile)이 없어 확인하지 못했습니다.";
        return text;
    }
} // namespace cyella
