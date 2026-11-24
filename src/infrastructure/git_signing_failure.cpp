#include "infrastructure/git_signing_failure.h"

#include "domain/text_catalog.h"
#include "infrastructure/git_error_classifier.h"

namespace cyella {
    namespace {
        bool contains(const std::string_view text, const std::string_view needle) noexcept
        {
            return text.find(needle) != std::string_view::npos;
        }
    } // namespace

    std::u8string signing_failure_summary(const failure_kind kind, const std::string_view standard_error)
    {
        const bool gpg { signing_failure_is_from_gpg(standard_error) };
        switch (kind)
        {
        case failure_kind::signing_passphrase_failed:
            if (gpg)
                return tr(u8"signing.gpg_passphrase_failed");
            return tr(u8"signing.ssh_passphrase_failed");
        case failure_kind::signing_key_missing:
            if (gpg)
                return tr(u8"signing.gpg_secret_key_missing");
            return tr(u8"signing.ssh_key_unreadable");
        case failure_kind::signing_failed:
            // 서명 프로그램을 띄우지 못했다. Git for Windows는 `cannot spawn`, POSIX의 git은 `cannot exec`로 알린다.
            if (contains(standard_error, "cannot spawn") || contains(standard_error, "cannot exec"))
                return tr(u8"signing.program_start_failed");
            return tr(u8"signing.failed_check_settings");
        default:
            break;
        }
        return {};
    }
} // namespace cyella
