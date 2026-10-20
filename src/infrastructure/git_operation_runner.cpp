#include "infrastructure/git_operation_runner.h"

#include "domain/path_syntax.h"
#include "infrastructure/git_extension_operations.h"
#include "infrastructure/git_operation_context.h"
#include "infrastructure/git_signing_operations.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace cyella {
    namespace {
        operation_outcome run_operation(git_operation_context& context)
        {
            const operation_kind kind { operation_kind_of(context.job().operation) };
            if (is_setup_operation(kind))
                return run_setup_operation(context);
            if (context.job().repository_path.empty())
                return operation_outcome::failed(failure_kind::invalid_request, u8"저장소 경로가 없습니다.");
            // submodule·worktree·Git LFS의 operation이다.
            if (std::optional<operation_outcome> extension { run_extension_operation(context) }; extension.has_value())
                return std::move(*extension);
            // 커밋 서명의 operation이다.
            if (std::optional<operation_outcome> signing { run_signing_operation(context) }; signing.has_value())
                return std::move(*signing);
            if (operation_lane_of(kind) == operation_lane::read)
                return run_read_operation(context);
            return run_write_operation(context);
        }

        std::u8string exception_text(const std::exception& error)
        {
            const std::string_view what { error.what() };
            return std::u8string { what.begin(), what.end() };
        }
    } // namespace

    git_operation_runner::git_operation_runner(process_runner& runner, std::u8string git_executable, workspace_files& files, git_operation_runner_options options)
        : runner_ { runner }
        , git_executable_ { std::move(git_executable) }
        , files_ { files }
        , options_ { std::move(options) }
    {}

    void git_operation_runner::execute(const git_operation_job& job, const operation_event_sink& emit) noexcept
    {
        operation_outcome outcome {};
        try
        {
            git_operation_context context { *this, job, emit };
            if (job.cancellation.cancelled())
                outcome = operation_outcome::cancelled();
            else
                outcome = run_operation(context);
        }
        catch (const std::invalid_argument& error)
        {
            // 조립기가 옵션처럼 보이는 이름·빈 경로를 거절했다. git은 실행되지 않았다.
            outcome = operation_outcome::failed(failure_kind::invalid_request, {}, exception_text(error));
        }
        catch (const std::exception& error)
        {
            outcome = operation_outcome::failed(failure_kind::other, u8"작업을 처리하다 내부 오류가 났습니다.", exception_text(error));
        }
        catch (...)
        {
            outcome = operation_outcome::failed(failure_kind::other, u8"작업을 처리하다 내부 오류가 났습니다.");
        }

        // 실행 중 취소가 걸렸으면 마지막 명령의 결과와 관계없이 취소로 끝낸다. 실패 배너를 띄우지 않기 위해서다.
        if (job.cancellation.cancelled() && outcome.state != operation_state::succeeded)
            outcome = operation_outcome::cancelled();

        // "이번만 연결"의 임시 known_hosts는 끝나는 순간 지운다. 마지막 사건보다 먼저 지워야 UI가
        // 끝난 것을 본 뒤에 파일이 남아 있지 않다.
        if (const temporary_ssh_trust* const trust { ssh_override_of(job.operation) }; trust != nullptr && trust->known_hosts_path.empty() == false)
            static_cast<void>(files_.remove_file(trust->known_hosts_path));

        if (emit == nullptr)
            return;
        try
        {
            operation_event event {};
            event.id = job.id;
            event.repository_id = job.repository_id;
            event.payload = operation_finished { outcome.state, std::move(outcome.failure), std::move(outcome.needs_input), std::move(outcome.summary) };
            emit(std::move(event));
        }
        catch (...)
        {
            // 마지막 사건조차 보내지 못하면 relay가 메모리를 얻지 못한 것이다. 더 할 수 있는 일이 없다.
        }
    }

    std::size_t git_operation_runner::active_process_count(const std::u8string_view repository_path) const
    {
        const std::u8string key { normalize_path_key(repository_path) };
        const std::lock_guard<std::mutex> lock { activity_mutex_ };
        const auto found { active_processes_.find(key) };
        return found == active_processes_.end() ? 0 : found->second;
    }

    git_operation_runner::activity_scope::activity_scope(const git_operation_runner& owner, const std::u8string_view repository_path)
        : owner_ { owner }
        , key_ { normalize_path_key(repository_path) }
    {
        const std::lock_guard<std::mutex> lock { owner_.activity_mutex_ };
        ++owner_.active_processes_[key_];
    }

    git_operation_runner::activity_scope::~activity_scope()
    {
        const std::lock_guard<std::mutex> lock { owner_.activity_mutex_ };
        const auto found { owner_.active_processes_.find(key_) };
        if (found == owner_.active_processes_.end())
            return;
        if (--found->second == 0)
            owner_.active_processes_.erase(found);
    }
} // namespace cyella
