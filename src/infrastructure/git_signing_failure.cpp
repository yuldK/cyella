#include "infrastructure/git_signing_failure.h"

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
                return u8"gpg가 passphrase를 받지 못했습니다 — gpg-agent 또는 pinentry 설정을 확인하세요.";
            return u8"ssh 서명 키의 passphrase를 받지 못했거나 맞지 않습니다. ssh-agent에 키를 등록하거나 다시 시도하세요.";
        case failure_kind::signing_key_missing:
            if (gpg)
                return u8"gpg에서 서명할 비밀 키를 찾지 못했습니다. 환경 설정의 서명에서 키를 고르세요.";
            return u8"ssh 서명 키를 읽지 못했습니다. 환경 설정의 서명에서 공개 키(.pub) 파일을 고르세요.";
        case failure_kind::signing_failed:
            if (contains(standard_error, "cannot spawn"))
                return u8"서명 프로그램을 실행하지 못했습니다. 환경 설정의 서명에서 gpg.program 또는 gpg.ssh.program을 확인하세요.";
            return u8"서명하지 못했습니다. 환경 설정의 서명에서 서명 프로그램(gpg·ssh-keygen)과 키를 확인하세요.";
        default:
            break;
        }
        return {};
    }
} // namespace cyella
