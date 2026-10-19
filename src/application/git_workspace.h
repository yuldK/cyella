#pragma once

#include "application/cancellation_registry.h"
#include "application/git_operation.h"
#include "application/git_operation_executor.h"
#include "application/operation_events.h"
#include "application/operation_executor.h"
#include "application/process_cancellation.h"
#include "application/repository_session.h"
#include "domain/operation_status.h"

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
    };

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

        void apply(const operation_event& event);

        [[nodiscard]] const operation_status* find_operation(operation_id id) const;
        // 낸 순서의 operation id다. 작업 로그가 이 순서로 그린다.
        [[nodiscard]] const std::deque<operation_id>& operation_order() const noexcept
        {
            return order_;
        }

        [[nodiscard]] std::optional<path_inspected> inspection(const std::u8string& path) const;

        // 상태가 바뀔 때마다 오른다.
        [[nodiscard]] std::uint64_t revision() const noexcept
        {
            return revision_;
        }

    private:
        struct operation_record
        {
            operation_status status {};
            operation_lane lane { operation_lane::read };
            std::uint64_t generation { 0 };
            // fetch·push_tag처럼 원격 태그를 바꾸는 작업이 끝난 뒤 다시 읽을 원격이다.
            std::vector<std::string> remote_tag_targets {};
            bool refreshes_remote_tags { false };
            std::shared_ptr<process_cancellation_source> cancellation {};
        };

        operation_id dispatch(operation_record record, git_operation_job job, std::string lane_key);
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
        operation_id next_id_ { 1 };
        std::uint64_t revision_ { 0 };
    };
} // namespace cyella
