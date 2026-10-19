#include "infrastructure/git_error_classifier.h"

#include <array>
#include <cstddef>
#include <span>

namespace cyella {
    namespace {
        char ascii_lowercase(const char value) noexcept
        {
            if (value >= 'A' && value <= 'Z')
                return static_cast<char>(value - 'A' + 'a');
            return value;
        }

        bool is_ascii_digit(const char value) noexcept
        {
            return value >= '0' && value <= '9';
        }

        bool contains_ignoring_case(const std::string_view text, const std::string_view needle) noexcept
        {
            if (needle.empty() || text.size() < needle.size())
                return false;
            const std::size_t last { text.size() - needle.size() };
            for (std::size_t begin { 0 }; begin <= last; ++begin)
            {
                std::size_t index { 0 };
                while (index < needle.size() && ascii_lowercase(text[begin + index]) == ascii_lowercase(needle[index]))
                    ++index;
                if (index == needle.size())
                    return true;
            }
            return false;
        }

        bool contains_any(const std::string_view text, const std::span<const std::string_view> needles) noexcept
        {
            for (const std::string_view needle : needles)
                if (contains_ignoring_case(text, needle))
                    return true;
            return false;
        }

        // 번역된 문장 안에서도 숫자 인자는 그대로 남는다. 다만 브랜치 이름이나 경로에
        // 우연히 같은 숫자가 있을 수 있어, 앞뒤가 숫자가 아닌 독립 토큰이면서 HTTP 문맥일
        // 때만 인정한다.
        bool contains_http_status(const std::string_view text, const std::string_view status) noexcept
        {
            if (contains_ignoring_case(text, "http") == false)
                return false;

            std::size_t begin { text.find(status) };
            while (begin != std::string_view::npos)
            {
                const bool digit_before { begin > 0 && is_ascii_digit(text[begin - 1]) };
                const std::size_t after { begin + status.size() };
                const bool digit_after { after < text.size() && is_ascii_digit(text[after]) };
                if (digit_before == false && digit_after == false)
                    return true;
                begin = text.find(status, begin + 1);
            }
            return false;
        }

        git_failure_kind classify_text(const std::string_view text) noexcept
        {
            // OpenSSH가 만드는 문구라 번역되지 않는다. 호스트 키는 인증 문제와 다르게
            // 안내해야 해서 따로 가른다.
            if (contains_ignoring_case(text, "Host key verification failed"))
                return git_failure_kind::host_key_verification_failed;
            constexpr std::array publickey {
                std::string_view { "Permission denied (publickey" },
                std::string_view { "Permission denied, please try again" },
            };
            if (contains_any(text, publickey))
                return git_failure_kind::permission_denied_publickey;

            // libcurl과 OpenSSH가 만드는 문구다. 둘 다 번역 catalog가 없다.
            constexpr std::array resolve {
                std::string_view { "Could not resolve host" },
                std::string_view { "Could not resolve hostname" },
                std::string_view { "Could not resolve proxy" },
            };
            if (contains_any(text, resolve))
                return git_failure_kind::could_not_resolve_host;

            if (contains_http_status(text, "401") || contains_http_status(text, "403"))
                return git_failure_kind::authentication_required;
            // 서버가 돌려주는 문구이거나 HTTP 상태라 번역과 무관하다.
            if (contains_ignoring_case(text, "Repository not found") || contains_http_status(text, "404"))
                return git_failure_kind::repository_not_found;

            // push 결과 표의 표지다. `[rejected]`와 `[remote rejected]`는 번역되지 않고,
            // 뒤의 사유 문구만 번역된다. 영어 사유로 non-fast-forward를 더 좁힌다.
            if (contains_ignoring_case(text, "[rejected]") || contains_ignoring_case(text, "[remote rejected]"))
            {
                if (contains_ignoring_case(text, "non-fast-forward") || contains_ignoring_case(text, "fetch first"))
                    return git_failure_kind::push_rejected_non_fast_forward;
                return git_failure_kind::push_rejected;
            }

            // 잠금 파일 이름은 번역되지 않는다. 다른 `.lock`(HEAD.lock 등)도 같은 경우다.
            if (contains_ignoring_case(text, "index.lock") || (contains_ignoring_case(text, ".lock'") && contains_ignoring_case(text, "File exists")))
                return git_failure_kind::index_lock_exists;

            // 아래는 번역 대상이라 영어 출력에서만 맞는 보조 신호다.
            constexpr std::array overwritten {
                std::string_view { "would be overwritten by" },
                std::string_view { "Please commit your changes or stash them" },
            };
            if (contains_any(text, overwritten))
                return git_failure_kind::local_changes_would_be_overwritten;

            constexpr std::array conflict {
                std::string_view { "CONFLICT (" },
                std::string_view { "Automatic merge failed" },
                std::string_view { "could not apply" },
                std::string_view { "Resolve all conflicts manually" },
                std::string_view { "fix conflicts and" },
            };
            if (contains_any(text, conflict))
                return git_failure_kind::conflict;

            constexpr std::array translated_authentication {
                std::string_view { "could not read Username" },
                std::string_view { "could not read Password" },
                std::string_view { "terminal prompts disabled" },
                std::string_view { "Authentication failed" },
            };
            if (contains_any(text, translated_authentication))
                return git_failure_kind::authentication_required;

            constexpr std::array translated_not_found {
                std::string_view { "does not appear to be a git repository" },
                std::string_view { "not a git repository" },
            };
            if (contains_any(text, translated_not_found))
                return git_failure_kind::repository_not_found;
            return git_failure_kind::unclassified;
        }
    } // namespace

    git_failure_kind classify_git_failure(const std::string_view standard_error, const std::string_view standard_output) noexcept
    {
        // stderr의 신호가 더 믿을 만하다. stdout은 충돌 문구처럼 git이 stdout으로
        // 내는 경우를 위한 보조다.
        const git_failure_kind from_error { classify_text(standard_error) };
        if (from_error != git_failure_kind::unclassified)
            return from_error;
        return classify_text(standard_output);
    }

    std::string_view git_failure_kind_name(const git_failure_kind kind) noexcept
    {
        switch (kind)
        {
        case git_failure_kind::unclassified:
            return "unclassified";
        case git_failure_kind::authentication_required:
            return "authentication_required";
        case git_failure_kind::host_key_verification_failed:
            return "host_key_verification_failed";
        case git_failure_kind::could_not_resolve_host:
            return "could_not_resolve_host";
        case git_failure_kind::repository_not_found:
            return "repository_not_found";
        case git_failure_kind::permission_denied_publickey:
            return "permission_denied_publickey";
        case git_failure_kind::push_rejected_non_fast_forward:
            return "push_rejected_non_fast_forward";
        case git_failure_kind::push_rejected:
            return "push_rejected";
        case git_failure_kind::conflict:
            return "conflict";
        case git_failure_kind::index_lock_exists:
            return "index_lock_exists";
        case git_failure_kind::local_changes_would_be_overwritten:
            return "local_changes_would_be_overwritten";
        }
        return "unclassified";
    }

    std::u8string_view git_failure_message(const git_failure_kind kind) noexcept
    {
        switch (kind)
        {
        case git_failure_kind::unclassified:
            return u8"명령이 실패했습니다.";
        case git_failure_kind::authentication_required:
            return u8"인증이 필요하거나 자격 증명이 거부되었습니다.";
        case git_failure_kind::host_key_verification_failed:
            return u8"SSH 호스트 키를 확인하지 못했습니다.";
        case git_failure_kind::could_not_resolve_host:
            return u8"원격 호스트 이름을 찾지 못했습니다.";
        case git_failure_kind::repository_not_found:
            return u8"저장소를 찾을 수 없습니다.";
        case git_failure_kind::permission_denied_publickey:
            return u8"SSH 키가 거부되었습니다.";
        case git_failure_kind::push_rejected_non_fast_forward:
            return u8"원격에 먼저 올라간 변경이 있어 push가 거절되었습니다. 먼저 가져오세요.";
        case git_failure_kind::push_rejected:
            return u8"원격이 push를 거절했습니다.";
        case git_failure_kind::conflict:
            return u8"충돌이 발생했습니다. 해결한 뒤 계속하세요.";
        case git_failure_kind::index_lock_exists:
            return u8"다른 git 작업이 저장소를 잠그고 있습니다.";
        case git_failure_kind::local_changes_would_be_overwritten:
            return u8"작업 트리의 변경이 덮어써질 수 있어 중단되었습니다.";
        }
        return u8"명령이 실패했습니다.";
    }
} // namespace cyella
