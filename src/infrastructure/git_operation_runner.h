#pragma once

#include "application/git_operation_executor.h"
#include "application/process_runner.h"
#include "application/workspace_files.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cyella {
    struct git_operation_runner_options
    {
        // refresh가 한 번에 읽는 log 쪽 크기다. 다음 쪽은 `load_more_log`가 읽는다.
        std::size_t log_page_size { 10000 };
        // log 스트림을 logic thread로 넘기는 묶음 크기와 최대 간격이다. 묶음이 작으면 relay
        // 메시지가 늘고, 크면 첫 화면이 늦게 뜬다.
        std::size_t log_batch_size { 2000 };
        std::chrono::milliseconds log_flush_interval { 50 };
        // 진행률 사건의 최소 간격이다. git은 초당 수십 줄을 내므로 그대로 보내면 frame만 늘린다.
        std::chrono::milliseconds progress_interval { 50 };
        // askpass 연결이 채운다. 비어 있으면 askpass 변수를 지워 인증이 즉시 실패한다.
        std::u8string askpass_helper {};
    };

    // `git_operation_executor`의 git CLI 구현이다. 한 instance를 모든 worker가 함께 쓴다.
    // 실행별 상태는 `execute` 안에만 있고, 공유 상태는 저장소별 실행 중 git 수뿐이다.
    class git_operation_runner final : public git_operation_executor
    {
    public:
        git_operation_runner(process_runner& runner, std::u8string git_executable, workspace_files& files, git_operation_runner_options options = {});

        void execute(const git_operation_job& job, const operation_event_sink& emit) noexcept override;

        // 이 저장소 경로에서 지금 실행 중인 git 프로세스 수다. `remove_stale_index_lock`이 잠금이
        // 정말 버려진 것인지 판단할 때 쓴다. 경로는 대소문자와 구분자를 무시하고 비교한다.
        [[nodiscard]] std::size_t active_process_count(std::u8string_view repository_path) const;

        // 아래는 실행 문맥(git_operation_context)이 쓰는 내부 접근자다.
        [[nodiscard]] process_runner& runner() const noexcept
        {
            return runner_;
        }

        [[nodiscard]] const std::u8string& git_executable() const noexcept
        {
            return git_executable_;
        }

        [[nodiscard]] workspace_files& files() const noexcept
        {
            return files_;
        }

        [[nodiscard]] const git_operation_runner_options& options() const noexcept
        {
            return options_;
        }

        // 프로세스 하나의 실행 구간을 센다. 소멸할 때 수를 줄인다.
        class activity_scope
        {
        public:
            activity_scope(const git_operation_runner& owner, std::u8string_view repository_path);
            activity_scope(const activity_scope&) = delete;
            activity_scope& operator=(const activity_scope&) = delete;
            ~activity_scope();

        private:
            const git_operation_runner& owner_;
            std::u8string key_ {};
        };

    private:
        process_runner& runner_;
        std::u8string git_executable_ {};
        workspace_files& files_;
        git_operation_runner_options options_ {};
        mutable std::mutex activity_mutex_ {};
        mutable std::unordered_map<std::u8string, std::size_t> active_processes_ {};
    };
} // namespace cyella
