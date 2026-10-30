#pragma once

#include "application/git_operation_executor.h"
#include "domain/operation_status.h"
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_error_classifier.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_status_parser.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// git_operation_runner의 구현 파일끼리만 쓰는 내부 헤더다. 다른 계층은 include하지 않는다.
namespace cyella {
    // operation 하나의 끝맺음이다. `operation_finished` 사건으로 바뀐다.
    struct operation_outcome
    {
        operation_state state { operation_state::succeeded };
        std::optional<operation_failure> failure {};
        std::optional<input_request> needs_input {};
        std::u8string summary {};

        [[nodiscard]] static operation_outcome succeeded(std::u8string summary = {});
        [[nodiscard]] static operation_outcome failed(failure_kind kind, std::u8string summary = {}, std::u8string detail = {});
        [[nodiscard]] static operation_outcome cancelled();
        [[nodiscard]] static operation_outcome needs(input_request request);
    };

    // git 명령 하나의 요청이다.
    struct git_command_spec
    {
        git_arguments arguments {};
        git_command_class command_class { git_command_class::local_query };
        std::string standard_input {};
        // 성공으로 볼 종료 코드다. `merge-base --is-ancestor`(1=아님)와 `diff --no-index`(1=다름)처럼
        // 0이 아닌 값이 정상 결과인 명령이 있다.
        std::vector<std::int32_t> accepted_exit_codes { 0 };
        // 비우면 저장소 경로다. clone·init·inspect_path가 바꾼다.
        std::u8string working_directory {};
        // 있으면 stdout 조각을 모으지 않고 바로 넘긴다(log 스트림).
        std::function<void(std::string_view)> on_standard_output {};
        // 이 명령에만 더하거나 바꿀 환경 변수다 (`git_request_options::environment`).
        std::vector<process_environment_override> environment {};
        // 진행 표시가 stderr가 아니라 stdout으로 나온다(git-lfs). 줄바꿈(CR·LF)으로 끊어 진행률로 보낸다.
        bool progress_on_standard_output { false };
    };

    struct git_command_outcome
    {
        process_result process {};
        // stdout 원문 byte다(raw 모드, 마스킹 없음). 해석기에만 넘기고 로그에는 남기지 않는다.
        std::string standard_output {};
        // 진행 표시를 뺀 stderr 줄이다(마스킹됨).
        std::vector<std::u8string> standard_error_lines {};
        bool accepted { false };

        [[nodiscard]] std::u8string standard_error_text() const;
        [[nodiscard]] std::string standard_error_bytes() const;
        [[nodiscard]] std::optional<std::int32_t> exit_code() const noexcept
        {
            return process.exit_code;
        }
    };

    // 한 job 동안 사는 실행 문맥이다. 명령을 실행하고, 명령마다 작업 로그 사건을 내보내고,
    // 진행률을 간격을 두고 보낸다. worker thread 하나에서만 쓴다.
    class git_operation_context
    {
    public:
        git_operation_context(const git_operation_runner& owner, const git_operation_job& job, const operation_event_sink& emit);

        [[nodiscard]] const git_operation_job& job() const noexcept
        {
            return job_;
        }

        [[nodiscard]] const git_operation_runner& owner() const noexcept
        {
            return owner_;
        }

        void emit(operation_event_payload payload) const;

        // 명령을 실행하고 작업 로그 사건을 하나 보낸다. 실패해도 결과를 돌려준다.
        [[nodiscard]] git_command_outcome run(git_command_spec spec);

        // 편의 판이다. 실행해 받아들일 수 없는 결과면 실패 끝맺음을 `stop`에 넣고 false다.
        [[nodiscard]] bool run_required(git_command_spec spec, git_command_outcome& outcome, std::optional<operation_outcome>& stop);

        // 실패한 명령을 끝맺음으로 바꾼다. 취소는 cancelled, 시간 초과는 timed_out이다.
        [[nodiscard]] operation_outcome failure_from(const git_command_outcome& outcome) const;

        // 저장소의 status를 읽는다. 실패하면 값이 없다.
        [[nodiscard]] std::optional<git_status_result> read_status();

        // `rev-parse --absolute-git-dir`. 실패하면 빈 문자열이다.
        [[nodiscard]] std::string read_git_dir();

        // git 디렉터리 안의 표지로 진행 중 작업을 판정하고 단계 파일을 읽는다.
        [[nodiscard]] in_progress_operation read_in_progress(std::string_view git_dir) const;

        // 현재 브랜치 이름이다. detached면 비어 있다.
        [[nodiscard]] std::string read_current_branch();

        // merge·rebase·cherry-pick·revert·pull처럼 충돌로 멈출 수 있는 명령의 끝맺음이다. 실패했고
        // 저장소가 진행 중 상태로 남았으며 충돌 파일이 있으면 `needs_input{conflict}`다. 문구가
        // 아니라 표지 파일과 status로 판정하므로 git의 메시지가 바뀌어도 맞다.
        [[nodiscard]] operation_outcome finish_sequence(const git_command_outcome& outcome, std::u8string success_summary);

        // 경로 목록을 받는 명령이다. 명령줄이 Windows 한도(32 KiB)를 넘을 수 있으면 stdin 판으로 바꾼다.
        [[nodiscard]] git_command_spec path_command(
            const std::vector<std::string>& paths, git_arguments (*inline_builder)(std::span<const std::string>), git_arguments (*stdin_builder)(), git_command_class command_class) const;

        // 사용자가 인증 대화 상자를 답 없이 닫았으면 인증 실패를 auth_cancelled로 바꾼다. `run`이
        // job의 `prompt_cancelled`를 보고 부른다.
        void note_authentication_cancelled() noexcept
        {
            authentication_cancelled_ = true;
        }

    private:
        friend class git_operation_sink;

        void report_progress(const std::u8string& line);

        const git_operation_runner& owner_;
        const git_operation_job& job_;
        const operation_event_sink& emit_;
        std::chrono::steady_clock::time_point last_progress_ {};
        std::u8string last_phase_ {};
        bool authentication_cancelled_ { false };
    };

    [[nodiscard]] operation_outcome run_read_operation(git_operation_context& context);
    [[nodiscard]] operation_outcome run_write_operation(git_operation_context& context);
    [[nodiscard]] operation_outcome run_setup_operation(git_operation_context& context);
    // 환경 설정의 SSH 도구다 (`ssh_operations.cpp`). `run_setup_operation`이 부른다.
    [[nodiscard]] operation_outcome run_ssh_keygen(git_operation_context& context, const ssh_keygen_request& request);
    [[nodiscard]] operation_outcome run_ssh_connection_test(git_operation_context& context, const ssh_connection_test_request& request);
    // 게시 상자의 "서버에 빈 저장소 만들기"다 (`ssh_operations.cpp`). 저장소의 쓰기 lane에서 `run_write_operation`이 부른다.
    [[nodiscard]] operation_outcome run_create_ssh_repository(git_operation_context& context, const create_ssh_repository_request& request);

    // git·ssh 실패 분류를 operation의 실패 종류로 옮긴다. ssh를 바로 부르는 도구도 git 명령과 같은 배너를 쓰게 한다.
    [[nodiscard]] failure_kind failure_kind_of(git_failure_kind kind) noexcept;

    // 문자열 계층을 잇는 도구다. git 출력은 UTF-8 byte(std::string)이고 프로세스 계약은 u8string이다.
    [[nodiscard]] std::string to_bytes(std::u8string_view text);
    [[nodiscard]] std::u8string to_utf8(std::string_view bytes);
    // git 디렉터리와 상대 경로를 잇는다. 구분자는 `/`다.
    [[nodiscard]] std::u8string join_path(std::u8string_view base, std::string_view relative);
    // 마지막 구성 요소를 뗀 부모 폴더다. 드라이브 루트(`C:/`)는 그대로 둔다.
    [[nodiscard]] std::u8string parent_directory(std::u8string_view path);
} // namespace cyella
