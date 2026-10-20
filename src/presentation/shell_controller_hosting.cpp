#include "presentation/dialog_intents.h"
#include "presentation/form_hosting.h"
#include "presentation/shell_controller.h"

#include <algorithm>
#include <utility>

// GitHub·GitLab의 controller 몫이다: 저장소 탭에서 호스팅 값을 읽어 `hosting_controller`에 건네고, PR 만들기 대화 상자를 연다.
namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }
    } // namespace

    std::optional<hosting_target> shell_controller::hosting_target_of(const std::string& repository_id) const
    {
        if (dependencies_.workspace == nullptr || repository_id.empty())
            return std::nullopt;
        const repository_session* const session { std::as_const(*dependencies_.workspace).find_session(repository_id) };
        if (session == nullptr)
            return std::nullopt;
        const repository_snapshot& snapshot { session->snapshot() };
        const repository_summary& summary { snapshot.summary };
        hosting_target target {};
        target.repository_id = repository_id;
        std::vector<hosting_remote_candidate> remotes {};
        for (const remote_info& remote : summary.remotes)
            remotes.push_back({ remote.name, remote.fetch_url.empty() ? remote.push_url : remote.fetch_url });
        target.repository = pick_hosting_repository(remotes, settings().hosting.hosts);
        target.working_directory = std::string { session->path().begin(), session->path().end() };
        target.current_branch = summary.current_branch;
        target.detached = summary.is_detached;
        target.has_upstream = summary.has_upstream;
        target.ahead = summary.ahead;
        target.head_sha = summary.head_sha;
        // HEAD 커밋의 제목은 PR 제목의 처음 값이다. 읽어 둔 이력의 앞쪽에서만 찾는다.
        const std::vector<commit_info>& commits { session->graph_commits() };
        const std::size_t limit { std::min<std::size_t>(commits.size(), 64) };
        for (std::size_t index { 0 }; index < limit; ++index)
            if (commits[index].sha == summary.head_sha)
            {
                target.head_subject = commits[index].subject;
                break;
            }
        for (const auto& [tab_id, id] : tab_repositories_)
            if (id == repository_id)
                if (const auto state { ui_.tabs.find(tab_id) }; state != ui_.tabs.end())
                {
                    target.selected_sha = state->second.selected_sha == graph_wip_sha ? std::string {} : state->second.selected_sha;
                    break;
                }
        return target;
    }

    operation_id shell_controller::submit_git_sequence(const std::string& repository_id, std::vector<git_operation> operations)
    {
        if (operations.empty())
            return 0;
        git_operation first { std::move(operations.front()) };
        operations.erase(operations.begin());
        const operation_id id { submit_to(repository_id, std::move(first)) };
        if (id != 0 && operations.empty() == false)
            chains_[id] = chain_entry { repository_id, std::move(operations) };
        return id;
    }

    hosting_controller::dependencies shell_controller::hosting_dependencies()
    {
        hosting_controller::dependencies result {};
        result.target_of = [this](const std::string& repository_id) { return hosting_target_of(repository_id); };
        result.remote_branches_of = [this](const std::string& repository_id) {
            std::vector<std::string> names {};
            if (dependencies_.workspace == nullptr)
                return names;
            if (const repository_session* const session { std::as_const(*dependencies_.workspace).find_session(repository_id) }; session != nullptr)
                for (const ref& value : session->snapshot().refs)
                    if (value.kind == ref_kind::remote)
                        names.push_back(value.name);
            return names;
        };
        result.selected_repository_id = [this] { return selected_repository_id(); };
        result.submit_job = [this](hosting_job job) {
            if (dependencies_.submit_hosting_job != nullptr)
                dependencies_.submit_hosting_job(std::move(job));
        };
        result.sign_out = [this](const hosting_sign_out_request& request) {
            if (dependencies_.hosting_sign_out != nullptr)
                dependencies_.hosting_sign_out(request);
        };
        result.now = [this] { return std::chrono::duration_cast<std::chrono::seconds>(dependencies_.clock().time_since_epoch()).count(); };
        result.notice = [this](std::u8string text, const bool warning) { show_notice(std::move(text), warning); };
        result.open_url = [this](const std::string& url) {
            if (dependencies_.open_url != nullptr)
                dependencies_.open_url(url);
        };
        result.submit_git = [this](const std::string& repository_id, std::vector<git_operation> operations) { return submit_git_sequence(repository_id, std::move(operations)); };
        result.find_operation = [this](const operation_id id) -> const operation_status* { return dependencies_.workspace != nullptr ? dependencies_.workspace->find_operation(id) : nullptr; };
        return result;
    }

    void shell_controller::open_create_pull_request_dialog()
    {
        const std::string repository_id { selected_repository_id() };
        const std::optional<create_pull_request_defaults> defaults { hosting_.create_defaults(repository_id) };
        if (defaults.has_value() == false)
        {
            show_notice(u8"이 저장소에는 GitHub·GitLab으로 연동할 원격이 없습니다.", true);
            return;
        }
        if (defaults->blocked_reason.empty() == false)
        {
            show_notice(defaults->blocked_reason, true);
            return;
        }
        const hosting_repository_view view { hosting_.view_of(repository_id) };
        dialog_state dialog {};
        dialog.kind = dialog_kind::create_pull_request;
        dialog.serial = ++ui_.dialog_serial;
        dialog.title = view.abbreviation + u8" 만들기";
        dialog.subject = defaults->source_branch;
        dialog.lines.push_back(
            to_u8(defaults->source_branch) + u8" → " + (defaults->target_branch.empty() ? std::u8string { u8"대상 브랜치" } : to_u8(defaults->target_branch)) + u8" · " + view.provider_name);
        if (defaults->push_remote.empty() == false)
            dialog.choices.push_back(defaults->push_remote);
        dialog.flag_c = defaults->needs_push && defaults->push_remote.empty() == false;
        // 원격에 없으면 먼저 push하는 것이 기본이다 (서버가 커밋을 알아야 만들 수 있다).
        dialog.flag_b = dialog.flag_c;
        clear_form_inputs();
        set_form_field(0, to_u8(defaults->target_branch));
        set_form_field(1, to_u8(defaults->title));
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::submit_create_pull_request(const dialog_state& dialog)
    {
        hosting_.submit_create(selected_repository_id(), make_create_pull_request_params(dialog, ui_.form), dialog.flag_c && dialog.flag_b);
    }
} // namespace cyella
