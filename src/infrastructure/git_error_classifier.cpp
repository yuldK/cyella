#include "infrastructure/git_error_classifier.h"

#include "domain/text_catalog.h"

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

        // git이 `--status-fd=2`로 부르는 gpg의 `[GNUPG:] FAILURE sign <코드>`에서 코드의 아래 16비트(gpg-error 코드)를 읽는다.
        // 위 비트는 오류를 낸 부품(gpg 8, gpg-agent 4)이라 부품마다 달라진다.
        std::optional<std::uint32_t> read_gpg_failure_code(const std::string_view text) noexcept
        {
            constexpr std::string_view marker { "[GNUPG:] FAILURE sign " };
            const std::size_t begin { text.find(marker) };
            if (begin == std::string_view::npos)
                return std::nullopt;
            std::uint64_t value { 0 };
            std::size_t index { begin + marker.size() };
            std::size_t digits { 0 };
            while (index < text.size() && is_ascii_digit(text[index]) && digits < 12)
            {
                value = value * 10u + static_cast<std::uint64_t>(text[index] - '0');
                ++index;
                ++digits;
            }
            if (digits == 0)
                return std::nullopt;
            return static_cast<std::uint32_t>(value & 0xFFFFu);
        }

        // 커밋·태그 서명이 실패한 출력인가. 문구마다 서명과 무관한 곳에서도 나올 수 있어(ssh 키 passphrase 오류는 fetch에서도 나온다)
        // 서명 도구를 부르다 실패했다는 git의 문장과 함께 있을 때만 인정한다.
        git_failure_kind classify_signing_text(const std::string_view text) noexcept
        {
            const std::optional<std::uint32_t> gpg_code { read_gpg_failure_code(text) };
            // ssh 서명은 git이 gpg처럼 "failed to sign"이라고 말하지 않고 `fatal: failed to write commit object`로 끝난다. 그 문장만으로는
            // 디스크 오류와 구별되지 않으므로 서명 도구가 낸 오류가 함께 있을 때만 서명 실패로 본다.
            const bool tool_complained = contains_ignoring_case(text, "cannot spawn") || contains_ignoring_case(text, "Load key") || contains_ignoring_case(text, "ssh-keygen")
                || contains_ignoring_case(text, "passphrase") || contains_ignoring_case(text, "gpg");
            const bool git_says_signing_failed = contains_ignoring_case(text, "gpg failed to sign the data") || contains_ignoring_case(text, "unable to sign the tag")
                || contains_ignoring_case(text, "failed to sign") || contains_ignoring_case(text, "Couldn't load public key")
                || (contains_ignoring_case(text, "failed to write commit object") && tool_complained);
            if (gpg_code.has_value() == false && git_says_signing_failed == false)
                return git_failure_kind::unclassified;

            // gpg-error 코드: 11 BAD_PASSPHRASE, 17 NO_SECKEY, 85 NO_PINENTRY, 99 CANCELED.
            if (gpg_code == 17u || contains_ignoring_case(text, "No secret key") || contains_ignoring_case(text, "Couldn't load public key")
                || contains_ignoring_case(text, "user.signingkey needs to be") || contains_ignoring_case(text, "No public key"))
                return git_failure_kind::signing_key_missing;
            constexpr std::array passphrase {
                std::string_view { "No pinentry" },
                std::string_view { "Bad passphrase" },
                std::string_view { "Operation cancelled" },
                std::string_view { "incorrect passphrase" },
                std::string_view { "Inappropriate ioctl for device" },
                std::string_view { "can't get input" },
                std::string_view { "pinentry" },
            };
            if (gpg_code == 11u || gpg_code == 85u || gpg_code == 99u || contains_any(text, passphrase))
                return git_failure_kind::signing_passphrase_failed;
            // 서명 도구를 아예 실행하지 못한 경우 (`cannot spawn <program>`)도 여기다.
            return git_failure_kind::signing_failed;
        }

        git_failure_kind classify_text(const std::string_view text) noexcept
        {
            // 새 git은 `is already used by worktree at`, 옛 git은 `is already checked out at`이라고 한다. 경로와 이름만
            // 번역되지 않는 채 남으므로 영어 출력에서만 맞는 신호다.
            if (contains_ignoring_case(text, "is already used by worktree at") || contains_ignoring_case(text, "is already checked out at"))
                return git_failure_kind::branch_checked_out_elsewhere;
            // OpenSSH가 만드는 문구라 번역되지 않는다. 호스트 키는 인증 문제와 다르게
            // 안내해야 해서 따로 가른다. 바뀐 키도 끝에 `Host key verification failed`가
            // 붙으므로 먼저 본다.
            if (contains_ignoring_case(text, "REMOTE HOST IDENTIFICATION HAS CHANGED"))
                return git_failure_kind::host_key_changed;
            if (contains_ignoring_case(text, "Host key verification failed"))
                return git_failure_kind::host_key_verification_failed;
            constexpr std::array publickey {
                std::string_view { "Permission denied (publickey" },
                std::string_view { "Permission denied, please try again" },
            };
            if (contains_any(text, publickey))
                return git_failure_kind::permission_denied_publickey;
            if (const git_failure_kind signing { classify_signing_text(text) }; signing != git_failure_kind::unclassified)
                return signing;

            // libcurl과 OpenSSH가 만드는 문구다. 둘 다 번역 catalog가 없다.
            constexpr std::array resolve {
                std::string_view { "Could not resolve host" },
                std::string_view { "Could not resolve hostname" },
                std::string_view { "Could not resolve proxy" },
            };
            if (contains_any(text, resolve))
                return git_failure_kind::could_not_resolve_host;
            constexpr std::array connect {
                std::string_view { "Failed to connect to" },
                std::string_view { "Could not connect to server" },
                std::string_view { "Couldn't connect to server" },
                std::string_view { "Connection refused" },
                std::string_view { "Connection timed out" },
                std::string_view { "Network is unreachable" },
                std::string_view { "No route to host" },
            };
            if (contains_any(text, connect))
                return git_failure_kind::connection_failed;

            if (contains_http_status(text, "401") || contains_http_status(text, "403"))
                return git_failure_kind::authentication_required;
            // 서버가 돌려주는 문구이거나 HTTP 상태라 번역과 무관하다.
            if (contains_ignoring_case(text, "Repository not found") || contains_http_status(text, "404"))
                return git_failure_kind::repository_not_found;

            // push 결과 표의 표지다. `[rejected]`와 `[remote rejected]`는 번역되지 않고,
            // 뒤의 사유 문구만 번역된다. 영어 사유로 non-fast-forward를 더 좁힌다.
            if (contains_ignoring_case(text, "[rejected]") || contains_ignoring_case(text, "[remote rejected]"))
            {
                // UI가 조치를 다르게 보인다. fetch first는 Pull, non-fast-forward는 rebase나
                // 강제 push, stale info는 다시 가져온 뒤 강제 push다.
                if (contains_ignoring_case(text, "(fetch first)"))
                    return git_failure_kind::push_rejected_fetch_first;
                if (contains_ignoring_case(text, "(stale info)"))
                    return git_failure_kind::push_rejected_stale_info;
                if (contains_ignoring_case(text, "non-fast-forward"))
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

            if (contains_ignoring_case(text, "Not possible to fast-forward"))
                return git_failure_kind::not_possible_to_fast_forward;

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

    std::optional<known_hosts_location> find_offending_known_hosts_line(const std::string_view standard_error)
    {
        if (contains_ignoring_case(standard_error, "REMOTE HOST IDENTIFICATION HAS CHANGED") == false)
            return std::nullopt;

        // `Offending ECDSA key in /c/Users/me/.ssh/known_hosts:3`. 경로에 `:`가 들어갈 수
        // 있어(`C:/...`) 마지막 `:` 뒤의 숫자를 줄 번호로 읽는다.
        std::string_view rest { standard_error };
        while (rest.empty() == false)
        {
            const std::size_t end { rest.find('\n') };
            std::string_view line { rest.substr(0, end) };
            rest = end == std::string_view::npos ? std::string_view {} : rest.substr(end + 1);
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);

            const std::size_t offending { line.find("Offending ") };
            if (offending == std::string_view::npos)
                continue;
            const std::size_t in { line.find(" in ", offending) };
            if (in == std::string_view::npos)
                continue;
            const std::string_view location { line.substr(in + 4) };
            const std::size_t colon { location.rfind(':') };
            if (colon == std::string_view::npos || colon + 1 >= location.size())
                continue;

            std::uint32_t number { 0 };
            bool digits { true };
            for (const char value : location.substr(colon + 1))
            {
                if (is_ascii_digit(value) == false)
                {
                    digits = false;
                    break;
                }
                number = number * 10u + static_cast<std::uint32_t>(value - '0');
            }
            if (digits == false || number == 0)
                continue;
            return known_hosts_location { std::string { location.substr(0, colon) }, number };
        }
        return known_hosts_location {};
    }

    std::optional<std::string> find_checked_out_worktree_path(const std::string_view standard_error)
    {
        for (const std::string_view marker : { std::string_view { "is already used by worktree at '" }, std::string_view { "is already checked out at '" } })
        {
            const std::size_t begin { standard_error.find(marker) };
            if (begin == std::string_view::npos)
                continue;
            const std::string_view rest { standard_error.substr(begin + marker.size()) };
            // 경로에 따옴표가 들어갈 수 있어 줄 끝에 가장 가까운 따옴표가 닫는 쪽이다.
            const std::size_t line_end { rest.find_first_of("\r\n") };
            const std::string_view line { rest.substr(0, line_end) };
            const std::size_t close { line.rfind('\'') };
            if (close == std::string_view::npos || close == 0)
                continue;
            return std::string { line.substr(0, close) };
        }
        return std::nullopt;
    }

    bool signing_failure_is_from_gpg(const std::string_view standard_error) noexcept
    {
        return contains_ignoring_case(standard_error, "[GNUPG:]") || contains_ignoring_case(standard_error, "gpg:") || contains_ignoring_case(standard_error, "gpg failed to sign");
    }

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
        case git_failure_kind::push_rejected_fetch_first:
            return "push_rejected_fetch_first";
        case git_failure_kind::push_rejected_stale_info:
            return "push_rejected_stale_info";
        case git_failure_kind::host_key_changed:
            return "host_key_changed";
        case git_failure_kind::connection_failed:
            return "connection_failed";
        case git_failure_kind::not_possible_to_fast_forward:
            return "not_possible_to_fast_forward";
        case git_failure_kind::branch_checked_out_elsewhere:
            return "branch_checked_out_elsewhere";
        case git_failure_kind::signing_failed:
            return "signing_failed";
        case git_failure_kind::signing_key_missing:
            return "signing_key_missing";
        case git_failure_kind::signing_passphrase_failed:
            return "signing_passphrase_failed";
        }
        return "unclassified";
    }

    std::u8string git_failure_message(const git_failure_kind kind)
    {
        switch (kind)
        {
        case git_failure_kind::unclassified:
            return tr(u8"git.failure.command_failed");
        case git_failure_kind::authentication_required:
            return tr(u8"git.failure.authentication_required");
        case git_failure_kind::host_key_verification_failed:
            return tr(u8"git.failure.host_key_verification_failed");
        case git_failure_kind::could_not_resolve_host:
            return tr(u8"git.failure.could_not_resolve_host");
        case git_failure_kind::repository_not_found:
            return tr(u8"git.failure.repository_not_found");
        case git_failure_kind::permission_denied_publickey:
            return tr(u8"git.failure.publickey_denied");
        case git_failure_kind::push_rejected_non_fast_forward:
            return tr(u8"git.failure.push_rejected_non_fast_forward");
        case git_failure_kind::push_rejected:
            return tr(u8"git.failure.push_rejected");
        case git_failure_kind::conflict:
            return tr(u8"git.failure.conflict");
        case git_failure_kind::index_lock_exists:
            return tr(u8"git.failure.index_lock_exists");
        case git_failure_kind::local_changes_would_be_overwritten:
            return tr(u8"git.failure.local_changes_would_be_overwritten");
        case git_failure_kind::push_rejected_fetch_first:
            return tr(u8"git.failure.push_rejected_fetch_first");
        case git_failure_kind::push_rejected_stale_info:
            return tr(u8"git.failure.push_rejected_stale_info");
        case git_failure_kind::host_key_changed:
            return tr(u8"git.failure.host_key_changed");
        case git_failure_kind::connection_failed:
            return tr(u8"git.failure.connection_failed");
        case git_failure_kind::not_possible_to_fast_forward:
            return tr(u8"git.failure.not_possible_to_fast_forward");
        case git_failure_kind::branch_checked_out_elsewhere:
            return tr(u8"git.failure.branch_checked_out_elsewhere");
        case git_failure_kind::signing_failed:
            return tr(u8"signing.failed_check_settings");
        case git_failure_kind::signing_key_missing:
            return tr(u8"signing.key_missing");
        case git_failure_kind::signing_passphrase_failed:
            return tr(u8"signing.passphrase_failed");
        }
        return tr(u8"git.failure.command_failed");
    }
} // namespace cyella
