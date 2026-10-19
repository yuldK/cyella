#include "application/git_workspace.h"

#include "domain/path_syntax.h"

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>

namespace cyella {
    namespace {
        std::string to_bytes(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string setup_target_of(const git_operation& operation)
        {
            if (const auto* const clone { std::get_if<clone_request>(&operation) })
                return clone->target_path;
            if (const auto* const init { std::get_if<init_request>(&operation) })
                return init->target_path;
            if (const auto* const inspect { std::get_if<inspect_path_request>(&operation) })
                return inspect->path;
            return {};
        }

        // continue·skip·abort와 충돌 해결, 정책을 고른 체크아웃은 기다리던 결정에 대한 답이다.
        bool answers_pending_input(const git_operation& operation) noexcept
        {
            if (std::holds_alternative<sequence_control_request>(operation) || std::holds_alternative<resolve_file_request>(operation) || std::holds_alternative<mark_resolved_request>(operation))
                return true;
            if (const auto* const checkout { std::get_if<checkout_branch_request>(&operation) })
                return checkout->policy != checkout_policy::ask;
            if (const auto* const remote { std::get_if<checkout_remote_branch_request>(&operation) })
                return remote->policy != checkout_policy::ask;
            if (const auto* const detached { std::get_if<checkout_detached_request>(&operation) })
                return detached->policy != checkout_policy::ask;
            return false;
        }

        // 원격 태그 표시를 바꾸는 작업과, 끝난 뒤 다시 읽을 원격이다. 빈 목록은 모든 원격이다.
        bool remote_tag_targets_of(const git_operation& operation, std::vector<std::string>& remotes)
        {
            if (const auto* const fetch { std::get_if<fetch_request>(&operation) })
            {
                if (fetch->all == false && fetch->remote.empty() == false)
                    remotes.push_back(fetch->remote);
                return true;
            }
            if (const auto* const tag { std::get_if<push_tag_request>(&operation) })
            {
                remotes.push_back(tag->remote);
                return true;
            }
            if (const auto* const tags { std::get_if<push_tags_request>(&operation) })
            {
                remotes.push_back(tags->remote);
                return true;
            }
            if (const auto* const deleted { std::get_if<delete_remote_tag_request>(&operation) })
            {
                remotes.push_back(deleted->remote);
                return true;
            }
            if (const auto* const push { std::get_if<push_execute_request>(&operation) }; push != nullptr && push->follow_tags)
            {
                if (push->remote.empty() == false)
                    remotes.push_back(push->remote);
                return true;
            }
            if (std::holds_alternative<clone_request>(operation))
                return true;
            return false;
        }
    } // namespace

    git_workspace::git_workspace(git_operation_executor& executor, operation_dispatcher dispatch, operation_event_sink post, cancellation_registry* const registry, git_workspace_options options)
        : executor_ { executor }
        , dispatch_ { std::move(dispatch) }
        , post_ { std::move(post) }
        , registry_ { registry }
        , options_ { options }
    {}

    std::string git_workspace::open_repository(std::u8string path)
    {
        const std::string id { to_bytes(normalize_path_key(path)) };
        if (sessions_.contains(id))
            return id;
        sessions_.emplace(id, std::make_unique<repository_session>(id, std::move(path)));
        ++revision_;
        if (options_.refresh_on_open)
            static_cast<void>(submit(id, refresh_request {}, true));
        return id;
    }

    bool git_workspace::close_repository(const std::string& repository_id)
    {
        if (sessions_.erase(repository_id) == 0)
            return false;
        // 닫힌 탭의 작업은 결과를 받을 곳이 없다. 실행 중인 것은 취소하고 사건은 버린다.
        for (auto& [id, record] : operations_)
            if (record.status.repository_id == repository_id && record.status.finished() == false && record.cancellation != nullptr)
                record.cancellation->request_cancellation();
        last_write_.erase(repository_id);
        ++revision_;
        return true;
    }

    repository_session* git_workspace::find_session(const std::string& repository_id)
    {
        const auto found { sessions_.find(repository_id) };
        return found == sessions_.end() ? nullptr : found->second.get();
    }

    const repository_session* git_workspace::find_session(const std::string& repository_id) const
    {
        const auto found { sessions_.find(repository_id) };
        return found == sessions_.end() ? nullptr : found->second.get();
    }

    std::vector<std::string> git_workspace::repository_ids() const
    {
        std::vector<std::string> ids {};
        ids.reserve(sessions_.size());
        for (const auto& [id, session] : sessions_)
            ids.push_back(id);
        return ids;
    }

    operation_id git_workspace::submit(const std::string& repository_id, git_operation operation, const bool automatic)
    {
        repository_session* const session { find_session(repository_id) };
        const operation_kind kind { operation_kind_of(operation) };
        if (session == nullptr || is_setup_operation(kind))
            return 0;
        // 읽는 중인 쪽을 또 읽으면 같은 위치의 묶음이 두 번 와서 둘째가 버려지고, 쪽이 끝났다는
        // 표시도 함께 버려져 읽는 중 상태가 풀리지 않는다. 이어 읽을 것이 없을 때도 내지 않는다.
        if (kind == operation_kind::load_more_log && (session->loaded() == false || session->log_page_loading() || session->history_complete()))
            return 0;

        operation_record record {};
        record.status.kind = kind;
        record.status.repository_id = repository_id;
        record.status.automatic = automatic;
        record.lane = operation_lane_of(kind);
        record.refreshes_remote_tags = remote_tag_targets_of(operation, record.remote_tag_targets);

        git_operation_job job {};
        job.repository_id = repository_id;
        job.repository_path = session->path();
        job.current_branch_hint = session->snapshot().summary.current_branch;

        const operation_id id { next_id_ };
        if (kind == operation_kind::refresh)
        {
            record.generation = session->begin_refresh();
        }
        else if (kind == operation_kind::load_more_log)
        {
            // 화면에 있는 generation을 이어 읽는다. 다음 refresh가 오면 이 결과는 버려진다.
            record.generation = session->displayed_generation();
            job.log_skip = session->loaded_commit_count();
            job.log_include_head = session->snapshot().summary.head_sha.empty() == false;
            session->begin_log_page();
        }
        else if (kind == operation_kind::show_commit)
        {
            session->expect_commit_detail(id);
        }
        else if (kind == operation_kind::diff_file)
        {
            session->expect_file_diff(id);
        }
        if (answers_pending_input(operation))
            session->set_awaiting_input(std::nullopt);

        job.generation = record.generation;
        job.operation = std::move(operation);

        std::string lane_key {};
        if (record.lane == operation_lane::write)
        {
            lane_key = repository_id;
            if (const auto previous { last_write_.find(repository_id) }; previous != last_write_.end())
            {
                const operation_status* const earlier { find_operation(previous->second) };
                if (earlier != nullptr && earlier->finished() == false)
                    record.status.queued_behind = previous->second;
            }
            last_write_[repository_id] = id;
        }
        return dispatch(std::move(record), std::move(job), std::move(lane_key));
    }

    operation_id git_workspace::submit_setup(git_operation operation)
    {
        const operation_kind kind { operation_kind_of(operation) };
        if (is_setup_operation(kind) == false)
            return 0;

        operation_record record {};
        record.status.kind = kind;
        record.status.target_path = setup_target_of(operation);
        record.lane = operation_lane_of(kind);
        record.refreshes_remote_tags = false;

        git_operation_job job {};
        job.operation = std::move(operation);
        // clone·init은 같은 대상 경로끼리만 줄을 세운다. 저장소 lane과 섞이지 않게 접두어를 붙인다.
        std::string lane_key {};
        if (record.lane == operation_lane::setup)
            lane_key = "setup:" + to_bytes(normalize_path_key(record.status.target_path));
        return dispatch(std::move(record), std::move(job), std::move(lane_key));
    }

    operation_id git_workspace::dispatch(operation_record record, git_operation_job job, std::string lane_key)
    {
        const operation_id id { next_id_++ };
        record.status.id = id;
        record.status.state = operation_state::queued;
        record.status.submitted_at = std::chrono::system_clock::now();
        record.cancellation = std::make_shared<process_cancellation_source>();
        if (registry_ != nullptr)
            registry_->add(*record.cancellation);
        job.id = id;

        operation_request request {};
        request.id = id;
        request.repository_key = std::move(lane_key);
        request.interactive = record.lane == operation_lane::read;
        request.job = [executor = &executor_, job = std::move(job), source = record.cancellation, post = post_](const operation_context& context) {
            // 사용자의 취소와 앱 종료를 한 token으로 합친다. 실행기는 token 하나만 runner에 넘긴다.
            process_cancellation_registration shutdown_link {};
            if (context.shutdown.cancellable())
                shutdown_link = context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); });

            git_operation_job running { job };
            running.cancellation = source->token();
            if (running.cancellation.cancelled())
            {
                operation_event event {};
                event.id = running.id;
                event.repository_id = running.repository_id;
                event.payload = operation_finished { operation_state::cancelled, std::nullopt, std::nullopt, {} };
                post(std::move(event));
                return;
            }
            post(operation_event { running.id, running.repository_id, operation_started {} });
            executor->execute(running, post);
        };

        operations_.emplace(id, std::move(record));
        order_.push_back(id);
        ++revision_;

        if (dispatch_ == nullptr || dispatch_(std::move(request)) == false)
        {
            operation_record& stored { operations_.at(id) };
            operation_finished rejected {};
            rejected.state = operation_state::failed;
            rejected.failure = operation_failure { failure_kind::other, u8"작업을 줄에 세우지 못했습니다(종료 중).", {} };
            finish(stored, rejected);
        }
        return id;
    }

    bool git_workspace::cancel(const operation_id id)
    {
        const auto found { operations_.find(id) };
        if (found == operations_.end())
            return false;
        operation_record& record { found->second };
        if (record.status.finished() || record.status.cancellable == false)
            return false;
        record.status.cancel_requested = true;
        if (record.cancellation != nullptr)
            record.cancellation->request_cancellation();
        // 아직 시작하지 않은 작업은 바로 끝난 것으로 보인다. job은 곧 취소된 token을 보고 빠진다.
        if (record.status.state == operation_state::queued)
        {
            operation_finished cancelled {};
            cancelled.state = operation_state::cancelled;
            finish(record, cancelled);
        }
        ++revision_;
        return true;
    }

    void git_workspace::apply(const operation_event& event)
    {
        const auto found { operations_.find(event.id) };
        if (found == operations_.end())
            return;
        operation_record& record { found->second };
        // 끝난 operation의 사건은 늦게 온 것이다(대기 중 취소 뒤의 finished 등).
        if (record.status.finished())
            return;
        repository_session* const session { find_session(event.repository_id) };

        std::visit(
            [this, &record, session, &event](const auto& payload) {
                using payload_type = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<payload_type, operation_started>)
                {
                    if (record.status.state == operation_state::queued)
                    {
                        record.status.state = operation_state::running;
                        record.status.started_at = std::chrono::system_clock::now();
                    }
                }
                else if constexpr (std::is_same_v<payload_type, operation_progressed>)
                {
                    record.status.progress = payload.progress;
                }
                else if constexpr (std::is_same_v<payload_type, operation_command_finished>)
                {
                    record.status.commands.push_back(payload.command);
                }
                else if constexpr (std::is_same_v<payload_type, path_inspected>)
                {
                    inspections_[payload.path] = payload;
                }
                else if constexpr (std::is_same_v<payload_type, commit_detail_loaded> || std::is_same_v<payload_type, file_diff_loaded>)
                {
                    if (session != nullptr)
                        session->apply(event.id, payload);
                }
                else if constexpr (std::is_same_v<payload_type, operation_finished>)
                {
                    finish(record, payload);
                }
                else
                {
                    if (session != nullptr)
                        session->apply(payload);
                }
            },
            event.payload);
        ++revision_;
    }

    void git_workspace::finish(operation_record& record, const operation_finished& finished)
    {
        const bool was_started { record.status.state == operation_state::running };
        record.status.state = finished.state;
        record.status.failure = finished.failure;
        record.status.needs_input = finished.needs_input;
        record.status.result_summary = finished.summary;
        record.status.finished_at = std::chrono::system_clock::now();
        // 충돌 대기는 abort로만 빠져나간다. 끝난 작업도 취소할 것이 없다.
        record.status.cancellable = false;

        const std::string& repository_id { record.status.repository_id };
        if (const auto last { last_write_.find(repository_id) }; last != last_write_.end() && last->second == record.status.id)
            last_write_.erase(last);

        repository_session* const session { find_session(repository_id) };
        if (session != nullptr)
        {
            if (finished.state == operation_state::needs_input && finished.needs_input.has_value())
                session->set_awaiting_input(record.status.id);
            // 실패한 log 쪽을 계속 기다리지 않게 한다.
            if ((record.status.kind == operation_kind::refresh || record.status.kind == operation_kind::load_more_log) && finished.state != operation_state::succeeded)
                session->abandon_log_page(record.generation);
        }

        // 실행되지 않은 쓰기는 저장소를 바꾸지 않았다. 시작한 쓰기는 실패·취소여도 일부가 바뀌었을 수 있다.
        if (was_started || finished.state != operation_state::cancelled)
            follow_up(record);
        trim_history();
    }

    void git_workspace::follow_up(const operation_record& record)
    {
        if (record.lane != operation_lane::write)
            return;
        const std::string repository_id { record.status.repository_id };
        if (find_session(repository_id) == nullptr)
            return;
        // 쓰기 lane이 끝난 뒤에 내므로 읽기 lane의 refresh가 쓰기와 겹치지 않는다.
        static_cast<void>(submit(repository_id, refresh_request {}, true));
        if (record.refreshes_remote_tags && record.status.state == operation_state::succeeded)
            static_cast<void>(submit(repository_id, refresh_remote_tags_request { record.remote_tag_targets }, true));
    }

    void git_workspace::trim_history()
    {
        std::size_t finished_count { 0 };
        for (const operation_id id : order_)
            if (const operation_status* const status { find_operation(id) }; status != nullptr && status->finished())
                ++finished_count;
        while (finished_count > options_.finished_history_limit && order_.empty() == false)
        {
            // 가장 오래된 끝난 작업부터 지운다. 기다리는 결정(needs_input)은 남긴다.
            const auto removable = [this](const operation_id id) {
                const operation_status* const status { find_operation(id) };
                return status != nullptr && status->finished() && status->state != operation_state::needs_input;
            };
            const auto oldest { std::find_if(order_.begin(), order_.end(), removable) };
            if (oldest == order_.end())
                break;
            operations_.erase(*oldest);
            order_.erase(oldest);
            --finished_count;
        }
    }

    const operation_status* git_workspace::find_operation(const operation_id id) const
    {
        const auto found { operations_.find(id) };
        return found == operations_.end() ? nullptr : &found->second.status;
    }

    std::optional<path_inspected> git_workspace::inspection(const std::u8string& path) const
    {
        const auto found { inspections_.find(path) };
        if (found == inspections_.end())
            return std::nullopt;
        return found->second;
    }
} // namespace cyella
