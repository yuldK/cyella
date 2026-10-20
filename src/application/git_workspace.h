#pragma once

#include "application/askpass_service.h"
#include "application/cancellation_registry.h"
#include "application/git_operation.h"
#include "application/git_operation_executor.h"
#include "application/operation_events.h"
#include "application/operation_executor.h"
#include "application/process_cancellation.h"
#include "application/repository_session.h"
#include "domain/credential_prompt.h"
#include "domain/operation_status.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cyella {
    // job을 lane에 넣는 길이다. 실제로는 `task_scheduler::submit`이고, test는 직접 실행하는
    // 가짜를 꽂는다. 접수하지 못하면 false다.
    using operation_dispatcher = std::function<bool(operation_request)>;

    struct git_workspace_options
    {
        // 저장소를 열면 곧바로 refresh를 낸다.
        bool refresh_on_open { true };
        // 끝난 operation을 이만큼만 남긴다. 작업 로그가 더 오래된 것은 보지 않는다.
        std::size_t finished_history_limit { 256 };
        // 인증 도우미의 서버다. 있으면 operation이 취소되거나 끝날 때 그 operation의 대기 질문을
        // 풀어 준다. git을 죽여도 별도 프로세스인 도우미는 답을 기다리며 남기 때문이다. null이면
        // askpass 없이 도는 앱이다. workspace보다 오래 살아야 한다.
        askpass_service* askpass { nullptr };
    };

    // 비밀이 담긴 문자열을 지우고 비운다. 컴파일러가 마지막 쓰기라며 없애지 못하게 volatile로
    // 쓴다. 최선 노력이다 (이미 다른 곳에 복사된 값은 지울 수 없다).
    void wipe_secret_text(std::u8string& text) noexcept;

    // 열린 저장소 세션들과 operation 표의 소유자다. logic thread에서만 쓴다.
    //
    // 한 operation의 흐름:
    // 1. `submit`이 id를 붙이고 상태(queued)를 만든 뒤 job을 dispatcher에 넣는다. 쓰기는 저장소
    //    key의 직렬 lane, 읽기는 읽기 lane이다.
    // 2. worker가 job을 실행하며 사건을 `post`로 보낸다(relay를 거쳐 logic thread로 돌아온다).
    // 3. `apply`가 사건을 operation 상태와 세션에 반영한다. 표에 없는 id나 이미 끝난 operation의
    //    사건은 늦게 온 것이라 버린다. 세션은 generation으로 한 번 더 거른다.
    // 4. 쓰기가 끝나면 그 저장소에 refresh를, 원격 태그를 바꾸는 작업 뒤에는 ls-remote를 낸다.
    class git_workspace
    {
    public:
        git_workspace(git_operation_executor& executor, operation_dispatcher dispatch, operation_event_sink post, cancellation_registry* registry = nullptr, git_workspace_options options = {});

        // 같은 경로(대소문자·구분자 무시)는 같은 id를 돌려준다. 처음 열면 refresh를 낸다.
        std::string open_repository(std::u8string path);
        bool close_repository(const std::string& repository_id);
        [[nodiscard]] repository_session* find_session(const std::string& repository_id);
        [[nodiscard]] const repository_session* find_session(const std::string& repository_id) const;
        [[nodiscard]] std::vector<std::string> repository_ids() const;

        // 저장소 operation을 낸다. 저장소가 없거나 저장소 준비 작업이면 0이다. load_more_log는 첫 쪽 전,
        // 다른 쪽을 읽는 중, 이력 끝에 닿은 뒤에도 0이다.
        operation_id submit(const std::string& repository_id, git_operation operation, bool automatic = false);
        // clone·init·inspect_path를 낸다. 대상 경로로 묶이며 저장소 id가 없다.
        operation_id submit_setup(git_operation operation);
        // 대기·실행 중이면 취소를 걸고 true다. 충돌 대기(needs_input)는 취소할 수 없다.
        bool cancel(operation_id id);

        // 인증 대화 상자의 답이다. `prompt_id`는 `operation_status::needs_input->prompt_id`다. 지금 보이는
        // 질문에만 답할 수 있다. 이미 답했거나 닫혔거나, 질문이 없거나 끝난 operation이면 아무 일도
        // 하지 않고 false다. 답한 operation은 `running`으로 돌아간다 (다른 질문이 줄 서 있으면 그
        // 질문이 `needs_input`으로 보인다). 답은 읽은 뒤 지운다. 상태·명령 기록·로그에 남기지 않는다.
        bool answer_input(operation_id id, std::uint64_t prompt_id, std::u8string answer);
        // 질문을 답 없이 닫는다. 이 operation의 이후 질문도 모두 취소로 답하고(ssh는 passphrase를 세
        // 번까지 다시 묻는다), git이 인증 실패로 끝나면 실패 종류는 `auth_cancelled`다. 답과 같은
        // 규칙으로 false를 돌려준다.
        bool cancel_input(operation_id id, std::uint64_t prompt_id);

        void apply(const operation_event& event);

        [[nodiscard]] const operation_status* find_operation(operation_id id) const;
        // 그 operation을 낸 요청 값이다. 인증 질문의 "이번만 연결"로 같은 요청을 다시 내고, 체크아웃 정책을 골라 다시 내는
        // 데 쓴다. 표에 없으면 null이다.
        [[nodiscard]] const git_operation* request_of(operation_id id) const;
        // 낸 순서의 operation id다. 작업 로그가 이 순서로 그린다.
        [[nodiscard]] const std::deque<operation_id>& operation_order() const noexcept
        {
            return order_;
        }

        [[nodiscard]] std::optional<path_inspected> inspection(const std::u8string& path) const;
        // `ssh_connection_test` operation의 분류다. 아직 오지 않았으면 값이 없다.
        [[nodiscard]] std::optional<ssh_connection_classification> ssh_connection_result_of(operation_id id) const;
        // `load_rebase_plan` operation이 읽은 범위다 (인터랙티브 rebase 편집기). 아직 오지 않았으면 null이다.
        [[nodiscard]] const rebase_plan* rebase_plan_of(operation_id id) const;

        // 상태가 바뀔 때마다 오른다.
        [[nodiscard]] std::uint64_t revision() const noexcept
        {
            return revision_;
        }

    private:
        // 답을 기다리는 인증 질문 하나다. 도우미 하나가 묻고 있으므로 `reply`를 부를 때까지 git이 멈춰 있다.
        struct pending_prompt
        {
            std::uint64_t prompt_id { 0 };
            credential_prompt prompt {};
            askpass_reply_callback reply {};
        };

        struct operation_record
        {
            operation_status status {};
            operation_lane lane { operation_lane::read };
            std::uint64_t generation { 0 };
            // fetch·push_tag처럼 원격 태그를 바꾸는 작업이 끝난 뒤 다시 읽을 원격이다.
            std::vector<std::string> remote_tag_targets {};
            bool refreshes_remote_tags { false };
            std::shared_ptr<process_cancellation_source> cancellation {};
            // 낸 요청이다 (`request_of`).
            git_operation request {};
            // 답을 기다리는 질문이다. 맨 앞이 `status.needs_input`에 보이는 질문이다. 대개 하나뿐이다.
            std::deque<pending_prompt> prompts {};
            // 사용자가 질문을 답 없이 닫았다. job과 공유해 실행기가 `auth_cancelled`를 고른다.
            std::shared_ptr<std::atomic<bool>> prompt_cancelled {};
        };

        operation_id dispatch(operation_record record, git_operation_job job, std::string lane_key);
        void receive_prompt(operation_id id, const operation_input_requested& asked);
        // `status`를 맨 앞 질문에 맞춘다. 질문이 없으면 `running`으로 돌린다.
        void show_front_prompt(operation_record& record);
        // 대기 질문 전부를 취소로 답하고 서버에도 이 operation을 풀라고 알린다.
        void release_prompts(operation_record& record);
        void finish(operation_record& record, const operation_finished& finished);
        void follow_up(const operation_record& record);
        void trim_history();

        git_operation_executor& executor_;
        operation_dispatcher dispatch_ {};
        operation_event_sink post_ {};
        cancellation_registry* registry_ { nullptr };
        git_workspace_options options_ {};
        std::map<std::string, std::unique_ptr<repository_session>> sessions_ {};
        std::unordered_map<operation_id, operation_record> operations_ {};
        std::deque<operation_id> order_ {};
        // 저장소마다 마지막으로 낸 쓰기다. 다음 쓰기의 `queued_behind`가 된다.
        std::unordered_map<std::string, operation_id> last_write_ {};
        std::map<std::u8string, path_inspected> inspections_ {};
        std::map<operation_id, ssh_connection_classification> ssh_tests_ {};
        std::map<operation_id, rebase_plan> rebase_plans_ {};
        operation_id next_id_ { 1 };
        std::uint64_t revision_ { 0 };
    };
} // namespace cyella
