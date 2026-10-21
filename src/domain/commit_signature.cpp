#include "domain/commit_signature.h"

#include "domain/text_catalog.h"

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

    std::u8string signature_badge_text(const signature_status status)
    {
        switch (status)
        {
        case signature_status::none:
            return tr(u8"signature.badge.none");
        case signature_status::good:
            return tr(u8"signature.badge.good");
        case signature_status::bad:
            return tr(u8"signature.badge.bad");
        case signature_status::unknown_key:
            return tr(u8"signature.badge.unknown_key");
        case signature_status::expired_signature:
            return tr(u8"signature.badge.expired_signature");
        case signature_status::expired_key:
            return tr(u8"signature.badge.expired_key");
        case signature_status::revoked_key:
            return tr(u8"signature.badge.revoked_key");
        case signature_status::cannot_check:
            break;
        }
        return tr(u8"signature.badge.cannot_check");
    }

    std::u8string signature_badge_label(const commit_signature& signature)
    {
        if (signature.note == signature_note::allowed_signers_missing)
            return tr(u8"signature.badge.allowed_signers_missing");
        return signature_badge_text(signature.status);
    }

    std::u8string signature_description(const commit_signature& signature)
    {
        std::u8string text {};
        switch (signature.status)
        {
        case signature_status::none:
            return text;
        case signature_status::good:
            text = tr(u8"signature.description.good");
            break;
        case signature_status::bad:
            text = tr(u8"signature.description.bad");
            break;
        case signature_status::unknown_key:
            text = tr(u8"signature.description.unknown_key");
            break;
        case signature_status::expired_signature:
            text = tr(u8"signature.description.expired_signature");
            break;
        case signature_status::expired_key:
            text = tr(u8"signature.description.expired_key");
            break;
        case signature_status::revoked_key:
            text = tr(u8"signature.description.revoked_key");
            break;
        case signature_status::cannot_check:
            text = tr(u8"signature.description.cannot_check");
            break;
        }
        if (signature.note == signature_note::allowed_signers_missing)
            text = tr(u8"signature.description.allowed_signers_missing");
        return text;
    }
} // namespace cyella
