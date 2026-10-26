#include "presentation/hosting_controller.h"

#include "domain/text_catalog.h"
#include "presentation/build_view_snapshot.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace cyella {
    namespace {
        // 마지막 CI 상태를 다시 읽기 전까지 믿는 시간(초)이다. 진행 중인 상태는 빨리 바뀌므로 짧다.
        constexpr std::int64_t pending_ci_ttl { 20 };
        constexpr std::int64_t final_ci_ttl { 300 };
        constexpr std::int64_t empty_ci_ttl { 60 };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::optional<std::uint64_t> parse_number(const std::string_view text)
        {
            if (text.empty())
                return std::nullopt;
            std::uint64_t value { 0 };
            const auto [end, error] { std::from_chars(text.data(), text.data() + text.size(), value) };
            if (error != std::errc {} || end != text.data() + text.size())
                return std::nullopt;
            return value;
        }

        std::u8string state_text(const pull_request_info& info)
        {
            switch (info.state)
            {
            case pull_request_state::open:
                return info.draft ? tr(u8"hosting.state_draft") : tr(u8"hosting.state_open");
            case pull_request_state::merged:
                return tr(u8"hosting.state_merged");
            case pull_request_state::closed:
                return tr(u8"hosting.state_closed");
            }
            return {};
        }

        std::u8string ci_text(const ci_state state)
        {
            switch (state)
            {
            case ci_state::success:
                return tr(u8"hosting.ci_success");
            case ci_state::failure:
                return tr(u8"hosting.ci_failure");
            case ci_state::pending:
                return tr(u8"hosting.ci_pending");
            case ci_state::error:
                return tr(u8"hosting.ci_error");
            case ci_state::none:
                break;
            }
            return tr(u8"hosting.ci_none");
        }

        bool web_url_allowed(const std::string_view url)
        {
            const bool scheme { url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0 };
            return scheme && std::ranges::none_of(url, [](const char value) { return static_cast<unsigned char>(value) < 0x20 || value == ' '; });
        }
    } // namespace

    hosting_controller::hosting_controller(dependencies dependencies)
        : dependencies_ { std::move(dependencies) }
    {
        if (dependencies_.now == nullptr)
            dependencies_.now = [] { return std::int64_t { 0 }; };
    }

    hosting_controller::host_session& hosting_controller::session_of(const std::string& host_key)
    {
        return hosts_[host_key];
    }

    const hosting_controller::host_session* hosting_controller::find_session(const std::string& host_key) const
    {
        const auto found { hosts_.find(host_key) };
        return found == hosts_.end() ? nullptr : &found->second;
    }

    hosting_controller::repository_state& hosting_controller::state_of(const std::string& repository_id)
    {
        return repositories_[repository_id];
    }

    const hosting_controller::repository_state* hosting_controller::find_state(const std::string& repository_id) const
    {
        const auto found { repositories_.find(repository_id) };
        return found == repositories_.end() ? nullptr : &found->second;
    }

    std::string hosting_controller::local_branch_for(const hosting_repository& repository, const std::uint64_t number)
    {
        return (repository.provider == hosting_provider::gitlab ? "mr/" : "pr/") + std::to_string(number);
    }

    std::string hosting_controller::refspec_for(const hosting_repository& repository, const std::uint64_t number)
    {
        const std::string id { std::to_string(number) };
        if (repository.provider == hosting_provider::gitlab)
            return "merge-requests/" + id + "/head:mr/" + id;
        return "pull/" + id + "/head:pr/" + id;
    }

    const pull_request_info* hosting_controller::find_pull_request(const repository_state& state, const std::uint64_t number) const
    {
        const auto found { std::ranges::find_if(*state.pulls, [number](const pull_request_info& info) { return info.number == number; }) };
        return found == state.pulls->end() ? nullptr : &*found;
    }

    std::optional<hosting_target> hosting_controller::supported_target(const std::string& repository_id, const bool announce) const
    {
        std::optional<hosting_target> target { dependencies_.target_of != nullptr ? dependencies_.target_of(repository_id) : std::nullopt };
        const auto tell = [this, announce](std::u8string text) {
            if (announce && dependencies_.notice != nullptr)
                dependencies_.notice(std::move(text), true);
        };
        if (target.has_value() == false || target->repository.has_value() == false)
        {
            tell(tr(u8"hosting.no_remote_notice"));
            return std::nullopt;
        }
        if (target->repository->supported() == false)
        {
            tell(tr(u8"hosting.unsupported_remote_notice"));
            return std::nullopt;
        }
        return target;
    }

    hosting_job hosting_controller::make_job(const hosting_target& target, std::variant<hosting_sign_in_job, hosting_list_job, hosting_create_job, hosting_ci_job, hosting_create_repository_job> work)
    {
        hosting_job job {};
        job.serial = next_serial_++;
        job.repository_id = target.repository_id;
        job.context.repository = *target.repository;
        job.context.working_directory = target.working_directory;
        job.work = std::move(work);
        return job;
    }

    // ------------------------------------------------------------------ 일감

    void hosting_controller::request_sign_in(const std::string& repository_id)
    {
        const std::optional<hosting_target> target { supported_target(repository_id, true) };
        if (target.has_value() == false || dependencies_.submit_job == nullptr)
            return;
        host_session& host { session_of(target->repository->host_key) };
        if (host.signing_in)
            return;
        host.signing_in = true;
        host.error_text.clear();
        hosting_job job { make_job(*target, hosting_sign_in_job {}) };
        pending_[job.serial] = { .what = pending_job::kind::sign_in, .repository_id = repository_id, .host_key = target->repository->host_key };
        state_of(repository_id).host_key = target->repository->host_key;
        dependencies_.submit_job(std::move(job));
    }

    void hosting_controller::request_list(const std::string& repository_id, const std::uint32_t page, const bool append)
    {
        const std::optional<hosting_target> target { supported_target(repository_id, false) };
        if (target.has_value() == false || dependencies_.submit_job == nullptr)
            return;
        const host_session* const host { find_session(target->repository->host_key) };
        if (host == nullptr || host->signed_in == false)
            return;
        repository_state& state { state_of(repository_id) };
        state.host_key = target->repository->host_key;
        state.loading = true;
        state.error_text.clear();
        hosting_job job { make_job(*target, hosting_list_job { pull_request_query { .include_closed = state.include_closed, .page = page } }) };
        state.list_serial = job.serial;
        pending_[job.serial] = { .what = pending_job::kind::list, .repository_id = repository_id, .host_key = target->repository->host_key, .page = page, .append = append };
        dependencies_.submit_job(std::move(job));
    }

    bool hosting_controller::is_stale_ci(const ci_entry& entry, const std::int64_t now) noexcept
    {
        if (entry.has_status == false)
            return true;
        std::int64_t ttl { final_ci_ttl };
        if (entry.status.state == ci_state::pending)
            ttl = pending_ci_ttl;
        else if (entry.status.state == ci_state::none)
            ttl = empty_ci_ttl;
        return now - entry.at > ttl;
    }

    void hosting_controller::request_ci(const std::string& repository_id, const std::string& sha, const bool force)
    {
        if (sha.empty() || sha == "WIP" || dependencies_.submit_job == nullptr)
            return;
        const std::optional<hosting_target> target { supported_target(repository_id, false) };
        if (target.has_value() == false)
            return;
        const host_session* const host { find_session(target->repository->host_key) };
        if (host == nullptr || host->signed_in == false)
            return;
        repository_state& state { state_of(repository_id) };
        state.host_key = target->repository->host_key;
        ci_entry& entry { state.ci[sha] };
        if (entry.loading)
            return;
        if (force == false && is_stale_ci(entry, dependencies_.now()) == false)
            return;
        entry.loading = true;
        hosting_job job { make_job(*target, hosting_ci_job { sha }) };
        pending_[job.serial] = { .what = pending_job::kind::ci, .repository_id = repository_id, .host_key = target->repository->host_key, .sha = sha };
        dependencies_.submit_job(std::move(job));
    }

    void hosting_controller::send_create(const std::string& repository_id, const create_pull_request_params& params)
    {
        const std::optional<hosting_target> target { supported_target(repository_id, true) };
        if (target.has_value() == false || dependencies_.submit_job == nullptr)
            return;
        hosting_job job { make_job(*target, hosting_create_job { params }) };
        pending_[job.serial] = { .what = pending_job::kind::create, .repository_id = repository_id, .host_key = target->repository->host_key };
        if (dependencies_.notice != nullptr)
            dependencies_.notice(tr(u8"hosting.creating", { { u8"kind", hosting_change_request_abbreviation(target->repository->provider) } }), false);
        dependencies_.submit_job(std::move(job));
    }

    // ------------------------------------------------------------------ 화면 메시지

    bool hosting_controller::handle(const luil::app_message& message)
    {
        if (const auto* const result { message.get<hosting_result_intent>() }; result != nullptr)
        {
            apply_result(*result);
            return true;
        }
        if (const auto* const intent { message.get<hosting_detail_action_intent>() }; intent != nullptr)
        {
            const std::string repository_id { dependencies_.selected_repository_id != nullptr ? dependencies_.selected_repository_id() : std::string {} };
            if (repository_id.empty())
                return true;
            switch (intent->action)
            {
            case hosting_detail_action::open_browser:
                open_in_browser(repository_id);
                break;
            case hosting_detail_action::checkout:
                checkout_pull_request(repository_id);
                break;
            case hosting_detail_action::close:
                clear_selection(repository_id);
                break;
            case hosting_detail_action::refresh_ci:
                if (const repository_state* const state { find_state(repository_id) }; state != nullptr && state->selected_copy.has_value())
                    request_ci(repository_id, state->selected_copy->head_sha, true);
                break;
            }
            return true;
        }
        if (message.get<hosting_sign_in_intent>() != nullptr)
        {
            if (dependencies_.selected_repository_id != nullptr)
                request_sign_in(dependencies_.selected_repository_id());
            return true;
        }
        if (const auto* const intent { message.get<hosting_sign_out_intent>() }; intent != nullptr)
        {
            sign_out(intent->host_key);
            return true;
        }
        return false;
    }

    void hosting_controller::activate_row(const std::string& repository_id, const std::string& path)
    {
        if (path == hosting_row_login)
        {
            request_sign_in(repository_id);
            return;
        }
        if (path == hosting_row_refresh)
        {
            request_list(repository_id, 1, false);
            return;
        }
        if (path == hosting_row_toggle_closed)
        {
            repository_state& state { state_of(repository_id) };
            state.include_closed = state.include_closed == false;
            request_list(repository_id, 1, false);
            return;
        }
        if (path == hosting_row_more)
        {
            const repository_state* const state { find_state(repository_id) };
            if (state != nullptr && state->next_page.has_value() && state->loading == false)
                request_list(repository_id, *state->next_page, true);
            return;
        }
        if (path == hosting_row_create)
            return;
        if (const std::optional<std::uint64_t> number { parse_number(path) }; number.has_value())
            select_pull_request(repository_id, *number);
    }

    void hosting_controller::select_pull_request(const std::string& repository_id, const std::uint64_t number)
    {
        repository_state& state { state_of(repository_id) };
        const pull_request_info* const found { find_pull_request(state, number) };
        if (found == nullptr)
            return;
        state.selected = number;
        state.selected_copy = *found;
        if (state.created.has_value() && state.created->number != number)
            state.created.reset();
        request_ci(repository_id, found->head_sha, false);
    }

    void hosting_controller::clear_selection(const std::string& repository_id)
    {
        const auto found { repositories_.find(repository_id) };
        if (found == repositories_.end())
            return;
        found->second.selected.reset();
        found->second.selected_copy.reset();
        found->second.created.reset();
    }

    void hosting_controller::on_repository_closed(const std::string& repository_id)
    {
        repositories_.erase(repository_id);
        std::erase_if(pending_, [&repository_id](const auto& entry) { return entry.second.repository_id == repository_id; });
        std::erase_if(pending_creates_, [&repository_id](const auto& entry) { return entry.second.repository_id == repository_id; });
        publishes_.erase(repository_id);
        creating_.erase(repository_id);
    }

    // ------------------------------------------------------------------ 게시

    void hosting_controller::request_publish(const std::string& repository_id, publish_repository_request request)
    {
        if (dependencies_.submit_job == nullptr || request.host.supported() == false)
            return;
        if (publishes_.contains(repository_id))
        {
            if (dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.publish.in_progress"), true);
            return;
        }
        const std::string host_key { request.host.host_key };
        publishes_[repository_id] = std::move(request);
        if (const host_session* const host { find_session(host_key) }; host != nullptr && host->signed_in)
        {
            send_create_repository(repository_id);
            return;
        }
        host_session& host { session_of(host_key) };
        host.signing_in = true;
        host.error_text.clear();
        hosting_job job { make_job(publish_target(repository_id), hosting_sign_in_job {}) };
        pending_[job.serial] = { .what = pending_job::kind::sign_in, .repository_id = repository_id, .host_key = host_key };
        dependencies_.submit_job(std::move(job));
    }

    bool hosting_controller::publishing(const std::string& repository_id) const
    {
        return publishes_.contains(repository_id);
    }

    hosting_target hosting_controller::publish_target(const std::string& repository_id) const
    {
        hosting_target target {};
        if (dependencies_.target_of != nullptr)
            if (std::optional<hosting_target> found { dependencies_.target_of(repository_id) }; found.has_value())
                target = std::move(*found);
        target.repository_id = repository_id;
        if (const auto found { publishes_.find(repository_id) }; found != publishes_.end())
            target.repository = found->second.host;
        return target;
    }

    void hosting_controller::send_create_repository(const std::string& repository_id)
    {
        const auto found { publishes_.find(repository_id) };
        // 같은 호스트의 로그인이 둘 겹쳐 끝나도(사이드바 로그인과 게시) 저장소는 한 번만 만든다.
        if (found == publishes_.end() || dependencies_.submit_job == nullptr || creating_.insert(repository_id).second == false)
            return;
        const publish_repository_request& publish { found->second };
        if (dependencies_.notice != nullptr)
            dependencies_.notice(tr(u8"hosting.publish.creating", { { u8"host", publish.host.host_key }, { u8"name", publish.params.name } }), false);
        hosting_job job { make_job(publish_target(repository_id), hosting_create_repository_job { publish.params }) };
        pending_[job.serial] = { .what = pending_job::kind::create_repository, .repository_id = repository_id, .host_key = publish.host.host_key };
        dependencies_.submit_job(std::move(job));
    }

    void hosting_controller::apply_create_repository(const hosting_result_intent& result, const pending_job& job)
    {
        const auto found { publishes_.find(job.repository_id) };
        if (found == publishes_.end())
            return;
        const publish_repository_request publish { std::move(found->second) };
        publishes_.erase(found);
        creating_.erase(job.repository_id);
        const auto* const payload { std::get_if<hosting_result<created_repository>>(&result.payload) };
        if (payload == nullptr)
            return;
        if (payload->ok() == false)
        {
            if (dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.publish.failed", { { u8"host", publish.host.host_key }, { u8"error", describe_hosting_error(payload->error, result.at) } }), true);
            return;
        }
        const created_repository& created { *payload->value };
        // 고른 쪽 주소가 비었으면(서버가 ssh를 끈 경우 등) 다른 쪽을 쓴다.
        const bool ssh { publish.use_ssh ? created.ssh_url.empty() == false : created.https_url.empty() };
        std::vector<git_operation> operations {};
        operations.push_back(add_remote_request { .name = publish.remote_name, .url = ssh ? created.ssh_url : created.https_url });
        if (publish.push_branch.empty() == false)
        {
            push_execute_request push {};
            push.remote = publish.remote_name;
            push.branch = publish.push_branch;
            push.remote_branch = publish.push_branch;
            push.set_upstream = true;
            operations.push_back(std::move(push));
        }
        if (dependencies_.notice != nullptr && publish.push_branch.empty())
            dependencies_.notice(tr(u8"hosting.publish.created_remote_only", { { u8"host", publish.host.host_key }, { u8"name", created.full_name }, { u8"url", created.web_url } }), false);
        else if (dependencies_.notice != nullptr)
            dependencies_.notice(tr(u8"hosting.publish.created", { { u8"host", publish.host.host_key }, { u8"name", created.full_name }, { u8"url", created.web_url } }), false);
        if (dependencies_.submit_git != nullptr)
            static_cast<void>(dependencies_.submit_git(job.repository_id, std::move(operations)));
    }

    void hosting_controller::on_commit_selected(const std::string& repository_id, const std::string& sha)
    {
        request_ci(repository_id, sha, false);
    }

    void hosting_controller::open_in_browser(const std::string& repository_id)
    {
        const repository_state* const state { find_state(repository_id) };
        if (state == nullptr || state->selected_copy.has_value() == false || dependencies_.open_url == nullptr)
            return;
        const std::string& url { state->selected_copy->web_url };
        // 서버가 준 주소라 http(s)만 연다 (file:·실행 파일 경로·스크립트 주소를 ShellExecute에 넘기지 않는다).
        if (web_url_allowed(url) == false)
        {
            if (dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.url_not_openable"), true);
            return;
        }
        dependencies_.open_url(url);
    }

    void hosting_controller::checkout_pull_request(const std::string& repository_id)
    {
        const repository_state* const state { find_state(repository_id) };
        if (state == nullptr || state->selected_copy.has_value() == false || dependencies_.submit_git == nullptr)
            return;
        const std::optional<hosting_target> target { supported_target(repository_id, true) };
        if (target.has_value() == false)
            return;
        const std::uint64_t number { state->selected_copy->number };
        const std::string local { local_branch_for(*target->repository, number) };
        if (target->current_branch == local)
        {
            if (dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.already_on_branch", { { u8"branch", local } }), false);
            return;
        }
        if (target->repository->remote_name.empty())
        {
            if (dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.no_remote_name"), true);
            return;
        }
        fetch_request fetch {};
        fetch.remote = target->repository->remote_name;
        fetch.prune = false;
        fetch.refspec = refspec_for(*target->repository, number);
        std::vector<git_operation> operations {};
        operations.emplace_back(std::move(fetch));
        operations.emplace_back(checkout_branch_request { .name = local });
        if (dependencies_.submit_git(repository_id, std::move(operations)) == 0 && dependencies_.notice != nullptr)
            dependencies_.notice(tr(u8"hosting.checkout_not_queued"), true);
    }

    // ------------------------------------------------------------------ 만들기

    std::u8string hosting_controller::create_blocked_reason(const hosting_target& target) const
    {
        const host_session* const host { target.repository.has_value() ? find_session(target.repository->host_key) : nullptr };
        if (target.detached || target.current_branch.empty())
            return tr(u8"hosting.create_blocked_detached");
        if (host == nullptr || host->signed_in == false)
            return tr(u8"hosting.create_blocked_signed_out");
        return {};
    }

    std::optional<create_pull_request_defaults> hosting_controller::create_defaults(const std::string& repository_id) const
    {
        const std::optional<hosting_target> target { supported_target(repository_id, false) };
        if (target.has_value() == false)
            return std::nullopt;
        create_pull_request_defaults defaults {};
        defaults.source_branch = target->current_branch;
        defaults.push_remote = target->repository->remote_name;
        defaults.needs_push = target->has_upstream == false || target->ahead > 0;
        defaults.title = target->head_subject.empty() ? target->current_branch : target->head_subject;
        const std::string prefix { target->repository->remote_name + "/" };
        std::vector<std::string> remote_branches { target->remote_branches };
        if (remote_branches.empty() && dependencies_.remote_branches_of != nullptr)
            remote_branches = dependencies_.remote_branches_of(repository_id);
        const auto remote_has = [&](const std::string_view branch) { return std::ranges::find(remote_branches, prefix + std::string { branch }) != remote_branches.end(); };
        for (const char* const candidate : { "main", "master", "develop", "trunk" })
            if (remote_has(candidate) && target->current_branch != candidate)
            {
                defaults.target_branch = candidate;
                break;
            }
        if (defaults.target_branch.empty())
            for (const std::string& name : remote_branches)
                if (name.rfind(prefix, 0) == 0 && name.substr(prefix.size()) != target->current_branch && name.find("HEAD") == std::string::npos)
                {
                    defaults.target_branch = name.substr(prefix.size());
                    break;
                }
        defaults.blocked_reason = create_blocked_reason(*target);
        return defaults;
    }

    void hosting_controller::submit_create(const std::string& repository_id, create_pull_request_params params, const bool push_first)
    {
        const std::optional<hosting_target> target { supported_target(repository_id, true) };
        if (target.has_value() == false)
            return;
        if (push_first == false)
        {
            send_create(repository_id, params);
            return;
        }
        if (dependencies_.submit_git == nullptr)
            return;
        push_execute_request push {};
        push.remote = target->repository->remote_name;
        push.branch = target->current_branch;
        push.remote_branch = target->current_branch;
        push.set_upstream = true;
        std::vector<git_operation> operations {};
        operations.emplace_back(std::move(push));
        const operation_id id { dependencies_.submit_git(repository_id, std::move(operations)) };
        if (id == 0)
        {
            if (dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.push_not_queued"), true);
            return;
        }
        pending_creates_[id] = { repository_id, std::move(params) };
        if (dependencies_.notice != nullptr)
            dependencies_.notice(tr(u8"hosting.push_then_create", { { u8"kind", hosting_change_request_abbreviation(target->repository->provider) } }), false);
    }

    void hosting_controller::after_events()
    {
        if (dependencies_.find_operation == nullptr)
            return;
        for (auto entry { pending_creates_.begin() }; entry != pending_creates_.end();)
        {
            const operation_status* const status { dependencies_.find_operation(entry->first) };
            if (status == nullptr)
            {
                entry = pending_creates_.erase(entry);
                continue;
            }
            if (status->finished() == false)
            {
                ++entry;
                continue;
            }
            const pending_create create { std::move(entry->second) };
            entry = pending_creates_.erase(entry);
            if (status->state == operation_state::succeeded)
                send_create(create.repository_id, create.params);
            else if (status->state == operation_state::failed && dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.push_failed_no_create"), true);
        }
    }

    // ------------------------------------------------------------------ 결과

    void hosting_controller::update_host(const hosting_result_intent& result)
    {
        host_session& host { session_of(result.host_key) };
        const bool was_signed_in { host.signed_in };
        host.signed_in = result.signed_in;
        host.user = result.user;
        if (result.rate_limit.remaining.has_value() || result.rate_limit.reset_at.has_value())
            host.rate_limit = result.rate_limit;
        if (was_signed_in && result.signed_in == false)
        {
            // 토큰을 잃었다: 이 호스트의 비공개 데이터를 화면에 남기지 않는다.
            for (auto& [repository_id, state] : repositories_)
                if (state.host_key == result.host_key)
                {
                    state.pulls = std::make_shared<const std::vector<pull_request_info>>();
                    state.loaded = false;
                    state.next_page.reset();
                    state.ci.clear();
                    state.selected.reset();
                    state.selected_copy.reset();
                }
        }
    }

    void hosting_controller::apply_result(const hosting_result_intent& result)
    {
        const auto found { pending_.find(result.serial) };
        if (found == pending_.end())
            return;
        const pending_job job { found->second };
        pending_.erase(found);
        update_host(result);
        switch (job.what)
        {
        case pending_job::kind::sign_in:
            apply_sign_in(result, job);
            break;
        case pending_job::kind::list:
            apply_list(result, job);
            break;
        case pending_job::kind::create:
            apply_create(result, job);
            break;
        case pending_job::kind::ci:
            apply_ci(result, job);
            break;
        case pending_job::kind::create_repository:
            apply_create_repository(result, job);
            break;
        }
    }

    void hosting_controller::apply_sign_in(const hosting_result_intent& result, const pending_job& job)
    {
        host_session& host { session_of(job.host_key) };
        host.signing_in = false;
        const auto* const payload { std::get_if<hosting_result<std::string>>(&result.payload) };
        if (payload == nullptr)
            return;
        // 게시가 이 로그인을 기다리고 있었는가.
        const auto publish { publishes_.find(job.repository_id) };
        const bool for_publish { publish != publishes_.end() && publish->second.host.host_key == job.host_key };
        if (payload->ok() == false)
        {
            host.error_text = describe_hosting_error(payload->error, result.at);
            if (dependencies_.notice != nullptr)
                dependencies_.notice(to_u8(host.error_text), true);
            // 겹친 다른 로그인이 이미 저장소 만들기를 보냈으면 그 결과가 게시를 끝낸다.
            if (for_publish && creating_.contains(job.repository_id) == false)
                publishes_.erase(publish);
            return;
        }
        host.error_text.clear();
        if (dependencies_.notice != nullptr)
            dependencies_.notice(tr(u8"hosting.signed_in", { { u8"host", job.host_key }, { u8"user", *payload->value } }), false);
        if (for_publish)
        {
            send_create_repository(job.repository_id);
            return;
        }
        request_list(job.repository_id, 1, false);
        if (const std::optional<hosting_target> target { dependencies_.target_of != nullptr ? dependencies_.target_of(job.repository_id) : std::nullopt }; target.has_value())
            request_ci(job.repository_id, target->selected_sha, false);
    }

    void hosting_controller::apply_list(const hosting_result_intent& result, const pending_job& job)
    {
        const auto state_found { repositories_.find(job.repository_id) };
        if (state_found == repositories_.end())
            return;
        repository_state& state { state_found->second };
        // 더 나중에 낸 목록 요청이 있으면 이 결과는 낡았다.
        if (state.list_serial != result.serial)
            return;
        state.loading = false;
        const auto* const payload { std::get_if<hosting_result<pull_request_page>>(&result.payload) };
        if (payload == nullptr)
            return;
        if (payload->ok() == false)
        {
            state.error_text = describe_hosting_error(payload->error, result.at);
            return;
        }
        state.error_text.clear();
        state.loaded = true;
        state.next_page = payload->value->next_page;
        std::vector<pull_request_info> items {};
        if (job.append)
            items = *state.pulls;
        for (const pull_request_info& info : payload->value->items)
        {
            const auto same = [&info](const pull_request_info& other) { return other.number == info.number; };
            if (const auto existing { std::ranges::find_if(items, same) }; existing != items.end())
                *existing = info;
            else
                items.push_back(info);
        }
        state.pulls = std::make_shared<const std::vector<pull_request_info>>(std::move(items));
        // 열어 둔 PR이 목록에서 바뀌었으면 최신 모습으로 갱신한다.
        if (state.selected.has_value())
            if (const pull_request_info* const current { find_pull_request(state, *state.selected) }; current != nullptr)
                state.selected_copy = *current;
    }

    void hosting_controller::apply_create(const hosting_result_intent& result, const pending_job& job)
    {
        const auto* const payload { std::get_if<hosting_result<created_pull_request>>(&result.payload) };
        if (payload == nullptr)
            return;
        const std::optional<hosting_target> target { dependencies_.target_of != nullptr ? dependencies_.target_of(job.repository_id) : std::nullopt };
        std::string abbreviation = "PR";
        if (target.has_value() && target->repository.has_value())
            abbreviation = std::string { hosting_change_request_abbreviation(target->repository->provider) };
        if (payload->ok() == false)
        {
            if (dependencies_.notice != nullptr)
                dependencies_.notice(tr(u8"hosting.create_failed", { { u8"kind", abbreviation }, { u8"error", describe_hosting_error(payload->error, result.at) } }), true);
            return;
        }
        const created_pull_request& created { *payload->value };
        repository_state& state { state_of(job.repository_id) };
        pull_request_info info {};
        info.number = created.number;
        info.title = created.title;
        info.web_url = created.web_url;
        info.head_sha = created.head_sha;
        info.state = pull_request_state::open;
        state.selected = created.number;
        state.selected_copy = std::move(info);
        state.created = created_info { created.number, created.web_url };
        if (dependencies_.notice != nullptr)
            dependencies_.notice(tr(u8"hosting.created", { { u8"kind", abbreviation }, { u8"number", created.number }, { u8"url", created.web_url } }), false);
        request_list(job.repository_id, 1, false);
        request_ci(job.repository_id, state.selected_copy->head_sha, false);
    }

    void hosting_controller::apply_ci(const hosting_result_intent& result, const pending_job& job)
    {
        const auto state_found { repositories_.find(job.repository_id) };
        if (state_found == repositories_.end())
            return;
        ci_entry& entry { state_found->second.ci[job.sha] };
        entry.loading = false;
        const auto* const payload { std::get_if<hosting_result<ci_status_info>>(&result.payload) };
        if (payload == nullptr)
            return;
        entry.at = result.at;
        if (payload->ok() == false)
        {
            entry.error_text = describe_hosting_error(payload->error, result.at);
            return;
        }
        entry.error_text.clear();
        entry.has_status = true;
        entry.status = *payload->value;
    }

    void hosting_controller::sign_out(const std::string& host_key)
    {
        if (dependencies_.sign_out != nullptr)
            dependencies_.sign_out(hosting_sign_out_request { host_key });
        host_session& host { session_of(host_key) };
        host.signed_in = false;
        host.signing_in = false;
        host.user.clear();
        host.error_text.clear();
        for (auto& [repository_id, state] : repositories_)
            if (state.host_key == host_key)
            {
                state.pulls = std::make_shared<const std::vector<pull_request_info>>();
                state.loaded = false;
                state.loading = false;
                state.next_page.reset();
                state.ci.clear();
                state.selected.reset();
                state.selected_copy.reset();
                state.created.reset();
                state.error_text.clear();
            }
        std::erase_if(pending_, [&host_key](const auto& entry) { return entry.second.host_key == host_key; });
    }

    // ------------------------------------------------------------------ 화면 값

    std::u8string hosting_controller::rate_limit_text(const rate_limit_info& info) const
    {
        if (info.remaining.has_value() == false)
            return {};
        std::string calls { std::to_string(*info.remaining) };
        if (info.limit.has_value())
            calls += "/" + std::to_string(*info.limit);
        std::u8string text { tr(u8"hosting.rate_limit_remaining", { { u8"calls", calls } }) };
        if (info.reset_at.has_value() && *info.remaining == 0)
            text += tr(u8"hosting.rate_limit_reset_suffix", { { u8"time", describe_remaining_time(*info.reset_at - dependencies_.now()) } });
        return text;
    }

    ci_mark_view hosting_controller::mark_of(const hosting_repository& repository, const ci_entry* const entry) const
    {
        ci_mark_view mark {};
        mark.source = to_u8(std::string { hosting_provider_display_name(repository.provider) });
        if (entry == nullptr)
        {
            mark.text = tr(u8"hosting.ci_not_loaded");
            return mark;
        }
        mark.loading = entry->loading;
        if (entry->has_status)
        {
            mark.state = entry->status.state;
            mark.text = ci_text(entry->status.state);
            mark.web_url = entry->status.web_url;
            for (const ci_check_info& check : entry->status.checks)
                mark.checks.push_back({ to_u8(check.name), check.state, check.url });
            if (entry->loading)
                mark.text += tr(u8"hosting.ci_refreshing_suffix");
            return mark;
        }
        if (entry->loading)
        {
            mark.text = tr(u8"hosting.ci_checking");
            return mark;
        }
        mark.text = tr(u8"hosting.ci_load_failed");
        mark.error = to_u8(entry->error_text);
        return mark;
    }

    std::optional<ci_mark_view> hosting_controller::ci_mark(const std::string& repository_id, const std::string& sha) const
    {
        if (sha.empty() || sha == "WIP")
            return std::nullopt;
        const std::optional<hosting_target> target { supported_target(repository_id, false) };
        if (target.has_value() == false)
            return std::nullopt;
        const host_session* const host { find_session(target->repository->host_key) };
        if (host == nullptr || host->signed_in == false)
            return std::nullopt;
        const repository_state* const state { find_state(repository_id) };
        const ci_entry* entry { nullptr };
        if (state != nullptr)
            if (const auto found { state->ci.find(sha) }; found != state->ci.end())
                entry = &found->second;
        return mark_of(*target->repository, entry);
    }

    bool hosting_controller::has_pull_request_detail(const std::string& repository_id) const
    {
        const repository_state* const state { find_state(repository_id) };
        return state != nullptr && state->selected_copy.has_value();
    }

    hosting_repository_view hosting_controller::view_of(const std::string& repository_id) const
    {
        hosting_repository_view view {};
        const std::optional<hosting_target> target { dependencies_.target_of != nullptr ? dependencies_.target_of(repository_id) : std::nullopt };
        if (target.has_value() == false || target->repository.has_value() == false)
            return view;
        const hosting_repository& repository { *target->repository };
        view.provider = repository.provider;
        view.host_key = repository.host_key;
        view.provider_name = to_u8(std::string { hosting_provider_display_name(repository.provider) });
        view.abbreviation = to_u8(std::string { hosting_change_request_abbreviation(repository.provider) });
        if (repository.supported() == false)
        {
            view.support = hosting_support::unsupported;
            return view;
        }
        view.support = hosting_support::supported;
        const host_session* const host { find_session(repository.host_key) };
        view.signed_in = host != nullptr && host->signed_in;
        view.signing_in = host != nullptr && host->signing_in;
        if (host != nullptr)
        {
            view.user = to_u8(host->user);
            view.rate_limit_text = rate_limit_text(host->rate_limit);
        }
        const repository_state* const state { find_state(repository_id) };
        if (state == nullptr)
        {
            view.pulls = std::make_shared<const std::vector<pull_request_info>>();
        }
        else
        {
            view.loading = state->loading;
            view.loaded = state->loaded;
            view.include_closed = state->include_closed;
            view.has_more = state->next_page.has_value();
            view.pulls = state->pulls;
            view.error_text = to_u8(state->error_text);
            for (const pull_request_info& info : *state->pulls)
                if (info.ci.has_value())
                    view.list_ci.emplace_back(info.number, *info.ci);
                else if (const auto cached { state->ci.find(info.head_sha) }; cached != state->ci.end() && cached->second.has_status)
                    view.list_ci.emplace_back(info.number, cached->second.status.state);
            if (state->selected_copy.has_value())
            {
                const pull_request_info& info { *state->selected_copy };
                pull_request_detail_view detail {};
                detail.number = info.number;
                detail.title = to_u8(info.title);
                detail.author = to_u8(info.author);
                detail.source_branch = to_u8(info.source_branch);
                detail.target_branch = to_u8(info.target_branch);
                detail.state = info.state;
                detail.draft = info.draft;
                detail.state_text = state_text(info);
                detail.from_fork = info.from_fork;
                detail.head_sha = info.head_sha;
                detail.web_url = info.web_url;
                detail.body = to_u8(info.body);
                if (info.updated_at > 0)
                    detail.updated_text = relative_time_text(dependencies_.now(), info.updated_at);
                detail.abbreviation = view.abbreviation;
                const std::string local { local_branch_for(repository, info.number) };
                detail.local_branch = to_u8(local);
                detail.checked_out = target->current_branch == local;
                detail.can_checkout = repository.remote_name.empty() == false;
                detail.just_created = state->created.has_value() && state->created->number == info.number;
                const auto cached { state->ci.find(info.head_sha) };
                detail.ci = mark_of(repository, cached == state->ci.end() ? nullptr : &cached->second);
                view.detail = std::move(detail);
            }
        }
        view.create_blocked_reason = create_blocked_reason(*target);
        view.can_create = view.create_blocked_reason.empty();
        return view;
    }

    std::vector<hosting_host_row_view> hosting_controller::host_rows(const std::vector<hosting_host_setting>& configured) const
    {
        std::vector<hosting_host_row_view> rows {};
        const auto add = [&](const std::string& host, const hosting_provider provider, const std::string& api_base, const bool builtin) {
            hosting_host_row_view row {};
            row.host = host;
            row.provider = provider;
            row.api_base = api_base;
            row.builtin = builtin;
            if (const host_session* const session { find_session(host) }; session != nullptr)
            {
                row.signed_in = session->signed_in;
                row.user = to_u8(session->user);
                row.rate_limit_text = rate_limit_text(session->rate_limit);
            }
            rows.push_back(std::move(row));
        };
        // 표에 같은 이름이 있으면 그것이 내장 호스트를 덮는다 (`resolve_hosting_repository`와 같은 규칙).
        const auto overridden = [&configured](const std::string_view host) {
            return std::ranges::any_of(configured, [host](const hosting_host_setting& setting) { return normalize_hosting_host(setting.host) == host; });
        };
        if (overridden("github.com") == false)
            add("github.com", hosting_provider::github, "https://api.github.com", true);
        if (overridden("gitlab.com") == false)
            add("gitlab.com", hosting_provider::gitlab, "https://gitlab.com/api/v4", true);
        for (const hosting_host_setting& setting : configured)
            add(setting.host, setting.provider, setting.api_base, false);
        return rows;
    }
} // namespace cyella
