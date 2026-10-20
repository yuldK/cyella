#pragma once

#include "application/process_cancellation.h"
#include "application/process_request.h"
#include "application/process_runner.h"
#include "domain/git_identity.h"

#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 저장소의 작성자 신원(`user.*`)을 읽고 `git config --local`로 쓰는 경로다.
    // operation 표(workspace)를 거치지 않고 worker에서 바로 부르는 작은 입출력이다. 읽기는 `--no-optional-locks`의 로컬 조회이고, 쓰기는
    // 저장소의 `.git/config`만 건드린다. **전역·시스템 설정은 쓰지 않는다** (`--local`을 명시한다).
    struct git_identity_options
    {
        // 전역 설정 파일의 경로다. `global` 범위의 값이 이 파일이 아닌 곳에서 왔으면 `includeIf`로 분류한다.
        identity_origin_hints hints {};
        // 이 요청에만 더하거나 바꾸는 환경 변수다. test가 `GIT_CONFIG_GLOBAL`·`GIT_CONFIG_NOSYSTEM`으로 사용자 설정과 떼어 놓는다.
        std::vector<process_environment_override> environment {};
    };

    struct git_identity_read
    {
        git_identity identity {};
        // git을 실행하지 못했거나 오류로 끝났다. 신원이 비어 있는 것(값 없음)은 실패가 아니다.
        bool ok { false };
        std::u8string error {};
    };

    struct git_identity_write
    {
        bool ok { false };
        std::u8string message {};
    };

    class git_identity_gateway
    {
    public:
        git_identity_gateway(process_runner& runner, std::u8string git_executable, git_identity_options options = {});

        [[nodiscard]] git_identity_read read(std::u8string_view repository_path, const process_cancellation_token& token) const;
        // `user.name`·`user.email`을 저장소의 로컬 설정에 쓴다. 이름과 이메일이 모두 비어 있지 않아야 한다.
        [[nodiscard]] git_identity_write apply(std::u8string_view repository_path, std::u8string_view user_name, std::u8string_view user_email, const process_cancellation_token& token) const;

    private:
        process_runner* runner_ { nullptr };
        std::u8string git_executable_ {};
        git_identity_options options_ {};
    };

    // 읽기 명령의 인자다 (test가 확인한다).
    [[nodiscard]] std::vector<std::u8string> make_identity_read_arguments();
    [[nodiscard]] std::vector<std::u8string> make_identity_write_arguments(std::u8string_view key, std::u8string_view value);
} // namespace cyella
