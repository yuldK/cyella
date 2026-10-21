#include "presentation/signing_view.h"

#include "domain/text_catalog.h"

namespace cyella {
    signature_tone signature_tone_of(const signature_status status) noexcept
    {
        switch (status)
        {
        case signature_status::good:
            return signature_tone::good;
        case signature_status::bad:
        case signature_status::revoked_key:
            return signature_tone::bad;
        case signature_status::unknown_key:
        case signature_status::expired_signature:
        case signature_status::expired_key:
        case signature_status::cannot_check:
            return signature_tone::warning;
        case signature_status::none:
            break;
        }
        return signature_tone::neutral;
    }

    signature_view make_signature_view(const commit_signature* const signature)
    {
        signature_view view {};
        if (signature == nullptr)
        {
            view.badge = tr(u8"signing_page.signature_checking");
            return view;
        }
        view.known = true;
        view.status = signature->status;
        view.tone = signature_tone_of(signature->status);
        view.badge = signature_badge_label(*signature);
        view.description = signature_description(*signature);
        view.signer = signature->signer;
        view.key = signature->key;
        return view;
    }

    std::u8string signing_setup_hint(const signing_config& config, const bool signing_enabled)
    {
        if (signing_enabled == false)
            return {};
        // ssh 서명은 키 파일(.pub)을 git이 스스로 찾지 못한다. gpg는 설정이 없으면 커밋터 이메일의 키를 쓴다.
        if (config.format() == signing_format::ssh && config.has_signing_key() == false)
            return tr(u8"signing_page.ssh_signing_key_missing");
        return {};
    }
} // namespace cyella
