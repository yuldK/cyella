#pragma once

#include "application/hosting_credentials.h"
#include "application/process_request.h"
#include "application/process_runner.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // `git credential` 표준 입력의 `key=value` 줄이다. 값에 줄바꿈·NUL이 있으면 다른 필드를 심을 수 있으므로 값이 없다.
    // 순서는 프로토콜, 호스트, 사용자 이름, 비밀번호다. 사용자 이름과 비밀번호는 비어 있으면 적지 않는다.
    [[nodiscard]] std::optional<std::string> encode_credential_input(const credential_query& query, std::string_view username = {}, std::string_view password = {});

    struct parsed_credential_output
    {
        std::string username {};
        secret_text password {};
        bool has_password { false };
    };

    // `git credential fill`의 표준 출력을 읽는다. `password`가 없으면 `has_password`가 거짓이다. 그 밖의 줄
    // (`oauth_refresh_token`, `password_expiry_utc` …)은 버린다 — 메모리에도 두지 않는다.
    [[nodiscard]] parsed_credential_output parse_credential_output(std::string_view output);

    struct git_credential_source_options
    {
        // `git.exe`의 절대 경로다.
        std::u8string git_executable {};
        // 도우미가 창을 띄울 시간을 넉넉히 준다 (GCM의 로그인 창). 취소로도 끝낼 수 있다.
        std::chrono::milliseconds timeout { std::chrono::minutes { 5 } };
        // 저장소 폴더를 모를 때 git을 실행할 절대 경로 폴더다. 비면 시스템 임시 폴더다.
        std::u8string default_working_directory {};
        // 요청에 더할 환경 변수다. 시험이 `GIT_CONFIG_GLOBAL`·`GIT_CONFIG_NOSYSTEM`으로 사용자의 helper를 끊는 데 쓴다.
        std::vector<process_environment_override> environment {};
    };

    // 사용자의 자격 증명 도우미에 `git credential fill`·`reject`로 묻는다. 사용자 환경의 askpass 변수는 지우고
    // `GIT_TERMINAL_PROMPT=0`을 걸어, 도우미가 없으면 터미널이나 cyella의 인증 대화 상자로 새지 않고 바로 실패한다.
    // 표준 출력은 가공 없이(`raw`) 받아 로그에 남기지 않는다. 토큰을 담은 버퍼는 쓰고 나면 지운다.
    class git_credential_source final : public credential_source
    {
    public:
        git_credential_source(process_runner& runner, git_credential_source_options options);

        [[nodiscard]] credential_fill_result fill(const credential_query& query, const process_cancellation_token& cancellation) override;
        void reject(const credential_query& query, std::string_view username, const secret_text& token, const process_cancellation_token& cancellation) override;

    private:
        [[nodiscard]] process_request make_request(const credential_query& query, std::string verb, std::string standard_input) const;

        process_runner* runner_ { nullptr };
        git_credential_source_options options_ {};
    };
} // namespace cyella
