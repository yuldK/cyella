// GitHub·GitLab 화면을 `shell_controller` 수준에서 끝까지 확인한다: 사이드바 구역, 로그인 일감, 목록, 상세, 커밋 CI, PR 만들기 대화 상자(push 뒤),
// 체크아웃, 브라우저 열기. git은 가짜 runner이고 호스팅 일감은 수신함에 모은다 - 네트워크도 UI도 쓰지 않는다. UI Automation으로 눌러 볼 수 없는 몸짓
// (우클릭이 아닌 것도 포함해 포인터·키 입력이 필요한 것)은 이 test가 controller의 메시지로 대신 확인한다.
#include "application/app_messages.h"
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/commands/command_intents.h"
#include "presentation/commands/command_registry.h"
#include "presentation/dialog_intents.h"
#include "presentation/extension_intents.h"
#include "presentation/hosting_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/shell_controller.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    namespace snap = cyella::test::snapshot;

    constexpr std::int64_t base_time { 1700000000 + 3600 };

    repository_snapshot hosting_snapshot(const std::string& remote_url)
    {
        repository_snapshot snapshot { snap::make_snapshot(snap::sha_of(4), 0) };
        snapshot.summary.remotes = { { .name = "origin", .fetch_url = remote_url, .push_url = remote_url } };
        snapshot.summary.current_branch = "feature/graph";
        snapshot.summary.has_upstream = false;
        snapshot.summary.ahead = 0;
        snapshot.refs[0].is_current = false;
        snapshot.refs[1].is_current = true;
        return snapshot;
    }

    struct harness
    {
        explicit harness(const std::string& remote_url = "git@github.com:octo/hello.git")
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies() }
        {
            controller.start({});
            controller.open_repository(u8"C:\\work\\repo");
            repository = controller.selected_repository_id();
            repository_session* const session { workspace.find_session(repository) };
            REQUIRE(session != nullptr);
            std::vector<commit_info> commits { snap::linear_history(5) };
            const std::uint64_t generation { session->begin_refresh() };
            REQUIRE(session->apply(snapshot_loaded { generation, hosting_snapshot(remote_url) }));
            REQUIRE(session->apply(commits_loaded { .generation = generation, .first_index = 0, .commits = std::move(commits), .page_complete = true, .history_complete = true }));
        }

        shell_controller::dependencies make_dependencies()
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { base_time } }; };
            dependencies.submit_hosting_job = [this](hosting_job job) { jobs.push_back(std::move(job)); };
            dependencies.hosting_sign_out = [this](const hosting_sign_out_request& request) { sign_outs.push_back(request.host_key); };
            dependencies.open_url = [this](const std::string& url) { opened.push_back(url); };
            return dependencies;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            controller.handle(luil::app_message { std::move(intent) });
            controller.after_events();
        }

        [[nodiscard]] view_snapshot view()
        {
            return controller.make_view(1440.0f, 900.0f, 1.0f);
        }

        [[nodiscard]] const sidebar_row_view* row(const std::u8string& key)
        {
            const view_snapshot built { view() };
            last_view = built;
            if (last_view.repository.has_value() == false)
                return nullptr;
            const auto found { std::ranges::find_if(last_view.repository->sidebar, [&key](const sidebar_row_view& entry) { return entry.key == key; }) };
            return found == last_view.repository->sidebar.end() ? nullptr : &*found;
        }

        void activate(const std::string& path)
        {
            send(activate_sidebar_item_intent { sidebar_item_kind::pull_request, path });
        }

        void deliver(const hosting_job& job, hosting_payload payload, const bool signed_in = true)
        {
            hosting_result_intent result {};
            result.serial = job.serial;
            result.repository_id = job.repository_id;
            result.host_key = job.context.repository.host_key;
            result.payload = std::move(payload);
            result.signed_in = signed_in;
            result.user = "octocat";
            result.at = base_time;
            send(std::move(result));
        }

        void sign_in_and_load(const std::vector<pull_request_info>& items)
        {
            activate(hosting_row_login);
            REQUIRE(jobs.size() == 1);
            deliver(jobs[0], hosting_result<std::string> { .value = std::string { "octocat" } });
            const auto list { std::find_if(jobs.begin(), jobs.end(), [](const hosting_job& job) { return std::holds_alternative<hosting_list_job>(job.work); }) };
            REQUIRE(list != jobs.end());
            pull_request_page page {};
            page.items = items;
            deliver(hosting_job { *list }, hosting_result<pull_request_page> { .value = std::move(page) });
        }

        void settle()
        {
            for (int round { 0 }; round < 16; ++round)
            {
                while (dispatcher.run())
                {
                }
                const std::vector<operation_event> taken { events.take() };
                if (taken.empty() && dispatcher.pending() == 0)
                    return;
                for (const operation_event& event : taken)
                    workspace.apply(event);
                controller.after_events();
            }
        }

        template<typename request_type>
        [[nodiscard]] const request_type* last_request() const
        {
            const std::deque<operation_id>& order { workspace.operation_order() };
            for (auto it { order.rbegin() }; it != order.rend(); ++it)
            {
                const operation_status* const status { workspace.find_operation(*it) };
                if (status == nullptr || status->automatic)
                    continue;
                const git_operation* const request { workspace.request_of(*it) };
                if (request != nullptr)
                    if (const auto* const typed { std::get_if<request_type>(request) }; typed != nullptr)
                        return typed;
            }
            return nullptr;
        }

        void type(const std::size_t field, const std::u8string& text)
        {
            luil::text_edit_request request {};
            request.target = to_luil_target(static_cast<text_target>(static_cast<std::uint32_t>(text_target::form_field_0) + field));
            request.command = luil::text::text_edit_command::select_all;
            send(edit_text_intent { request });
            request.command = luil::text::text_edit_command::insert;
            request.text = text;
            send(edit_text_intent { request });
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_operation_runner runner;
        git_workspace workspace;
        shell_controller controller;
        std::string repository {};
        std::vector<hosting_job> jobs {};
        std::vector<std::string> sign_outs {};
        std::vector<std::string> opened {};
        view_snapshot last_view {};
    };

    pull_request_info make_pull(const std::uint64_t number, std::string title)
    {
        pull_request_info info {};
        info.number = number;
        info.title = std::move(title);
        info.author = "octocat";
        info.source_branch = "feature/" + std::to_string(number);
        info.target_branch = "main";
        info.head_sha = std::string(40, 'b');
        info.web_url = "https://github.com/octo/hello/pull/" + std::to_string(number);
        info.updated_at = base_time - 7200;
        return info;
    }
} // namespace

TEST_CASE("the PULL REQUESTS section shows a login row and no job is submitted until it is activated", "[hosting][shell]")
{
    harness test {};
    const sidebar_row_view* const header { test.row(u8"h:PULL REQUESTS") };
    REQUIRE(header != nullptr);
    CHECK(header->section == sidebar_section::pull_requests);
    const sidebar_row_view* const login { test.row(u8"pr:login") };
    REQUIRE(login != nullptr);
    CHECK(login->item == sidebar_item_kind::pull_request);
    CHECK(login->detail == u8"GitHub · github.com");
    // 화면을 짓고 커밋을 골라도 조용히 로그인하지 않는다.
    test.send(select_commit_intent { graph_item_key(snap::sha_of(0)) });
    static_cast<void>(test.view());
    CHECK(test.jobs.empty());

    test.activate(hosting_row_login);
    REQUIRE(test.jobs.size() == 1);
    CHECK(std::holds_alternative<hosting_sign_in_job>(test.jobs[0].work));
    CHECK(test.jobs[0].context.repository.owner == "octo");
    CHECK(test.jobs[0].context.working_directory == "C:\\work\\repo");
    CHECK(test.row(u8"pr:login")->label == u8"로그인 중…");
}

TEST_CASE("a remote of an unknown host explains how to add it", "[hosting][shell]")
{
    harness test { "https://git.example.com/team/app.git" };
    REQUIRE(test.row(u8"pr:info") != nullptr);
    CHECK(test.row(u8"pr:info")->tooltip.find(u8"호스팅") != std::u8string::npos);
    CHECK(test.row(u8"pr:login") == nullptr);
    // 환경 설정의 표에 더하면 같은 원격이 연동된다.
    test.send(open_settings_intent { settings_page::hosting });
    luil::text_edit_request request {};
    request.target = to_luil_target(text_target::settings_hosting_host);
    request.command = luil::text::text_edit_command::insert;
    request.text = u8"git.example.com";
    test.send(edit_text_intent { request });
    test.send(add_hosting_host_intent {});
    REQUIRE(test.controller.settings().hosting.hosts.size() == 1);
    REQUIRE(test.row(u8"pr:login") != nullptr);
    CHECK(test.row(u8"pr:login")->detail == u8"GitHub · git.example.com");
}

TEST_CASE("sign-in, list, open and CI flow through the sidebar and the detail panel", "[hosting][shell]")
{
    harness test {};
    test.sign_in_and_load({ make_pull(12, "Add feature"), make_pull(11, "Fix bug") });
    const sidebar_row_view* const first { test.row(u8"pr:n:12") };
    REQUIRE(first != nullptr);
    CHECK(first->label == u8"#12 Add feature");
    CHECK(test.row(u8"h:PULL REQUESTS")->count == 2);
    CHECK(test.row(u8"pr:login") == nullptr);

    test.activate("12");
    REQUIRE(test.jobs.size() == 3);
    REQUIRE(std::holds_alternative<hosting_ci_job>(test.jobs[2].work));
    view_snapshot built { test.view() };
    REQUIRE(built.repository.has_value());
    CHECK(built.repository->detail == detail_mode::pull_request);
    REQUIRE(built.repository->hosting.has_value());
    REQUIRE(built.repository->hosting->detail.has_value());
    CHECK(built.repository->hosting->detail->title == u8"Add feature");
    CHECK(built.repository->hosting->detail->ci.loading);

    ci_status_info status {};
    status.sha = std::string(40, 'b');
    status.state = ci_state::success;
    test.deliver(test.jobs[2], hosting_result<ci_status_info> { .value = status });
    built = test.view();
    CHECK(built.repository->hosting->detail->ci.text == u8"CI 통과");

    // 커밋을 고르면 상세가 커밋으로 돌아온다.
    test.send(select_commit_intent { graph_item_key(snap::sha_of(1)) });
    built = test.view();
    CHECK(built.repository->detail == detail_mode::commit);
    CHECK(built.repository->hosting->detail.has_value() == false);
}

TEST_CASE("opening a pull request leaves the diff and reveals the detail drawer", "[hosting][shell][ui]")
{
    for (const float width : { 480.0f, 900.0f, 1440.0f })
    {
        CAPTURE(width);
        harness test {};
        test.sign_in_and_load({ make_pull(12, "Review changes") });
        test.send(select_file_intent { { .path = "README.md" } });
        REQUIRE(test.controller.make_view(width, 700.0f, 1.0f).repository->diff.has_value());
        test.activate("12");
        const view_snapshot view { test.controller.make_view(width, 700.0f, 1.0f) };
        CHECK_FALSE(view.repository->diff.has_value());
        CHECK(view.drawer_open == (width < wide_layout_minimum_width));
        test.send(hosting_detail_action_intent { hosting_detail_action::close });
        CHECK_FALSE(test.controller.make_view(width, 700.0f, 1.0f).drawer_open);
    }
}

TEST_CASE("pull request details wrap and scroll without hiding actions or description lines", "[hosting][shell][ui]")
{
    harness test {};
    pull_request_info pull { make_pull(12, "A long pull request title explaining the complete change in repository navigation and review behavior") };
    pull.source_branch = "feature/a-very-long-review-branch-name-with-more-context";
    for (int line { 0 }; line < 25; ++line)
        pull.body += "Description line " + std::to_string(line) + " contains the review context and expected behavior.\n";
    test.sign_in_and_load({ pull });
    for (const float width : { 480.0f, 900.0f, 1440.0f })
    {
        CAPTURE(width);
        static_cast<void>(test.controller.make_view(width, 400.0f, 1.0f));
        test.activate("12");
        const ui::built_tree built { ui::build_ui_tree(test.controller.make_view(width, 400.0f, 1.0f)) };
        REQUIRE(built.feedback.detail_maximum.has_value());
        CHECK(*built.feedback.detail_maximum > 0.0f);
        const luil::ui_element* const body { built.tree->find({ ui::kind_label, u8"pr-body" }) };
        REQUIRE(body != nullptr);
        CHECK(body->accessibility().name.find(u8"Description line 24") != std::u8string::npos);
        const luil::ui_element* const bar { built.tree->find({ ui::kind_panel, u8"pr-actions-bar" }) };
        REQUIRE(bar != nullptr);
        for (const std::u8string owner : { u8"pr-checkout", u8"pr-open-browser", u8"pr-refresh-ci" })
        {
            const luil::ui_element* const button { built.tree->find({ ui::kind_text_button, owner }) };
            REQUIRE(button != nullptr);
            CHECK(button->bounds().x >= bar->bounds().x);
            CHECK(button->bounds().x + button->bounds().width <= bar->bounds().x + bar->bounds().width + 0.1f);
            CHECK(button->bounds().y + button->bounds().height <= 400.1f);
        }
        CHECK(built.tree->duplicate_ids().empty());
        CHECK(built.tree->unarranged().empty());
    }
}

TEST_CASE("the commit detail shows the CI mark of a signed-in host", "[hosting][shell]")
{
    harness test {};
    test.send(select_commit_intent { graph_item_key(snap::sha_of(1)) });
    view_snapshot built { test.view() };
    REQUIRE(built.repository->commit.has_value());
    CHECK(built.repository->commit->ci.has_value() == false);

    test.sign_in_and_load({});
    const std::size_t jobs_before { test.jobs.size() };
    // 로그인 직후 고른 커밋의 CI를 읽는다.
    REQUIRE(jobs_before >= 3);
    REQUIRE(std::holds_alternative<hosting_ci_job>(test.jobs.back().work));
    CHECK(std::get<hosting_ci_job>(test.jobs.back().work).sha == snap::sha_of(1));
    ci_status_info status {};
    status.sha = snap::sha_of(1);
    status.state = ci_state::failure;
    test.deliver(test.jobs.back(), hosting_result<ci_status_info> { .value = status });
    built = test.view();
    REQUIRE(built.repository->commit.has_value());
    REQUIRE(built.repository->commit->ci.has_value());
    CHECK(built.repository->commit->ci->state == ci_state::failure);
    CHECK(built.repository->commit->ci->text == u8"CI 실패");
    // 다른 커밋을 고르면 그 커밋의 CI를 읽는다.
    test.send(select_commit_intent { graph_item_key(snap::sha_of(2)) });
    CHECK(test.jobs.size() == jobs_before + 1);
    CHECK(std::get<hosting_ci_job>(test.jobs.back().work).sha == snap::sha_of(2));
}

TEST_CASE("the create dialog pushes the branch first and creates only after the push succeeded", "[hosting][shell]")
{
    harness test {};
    test.sign_in_and_load({ make_pull(3, "three") });
    test.activate(hosting_row_create);
    view_snapshot built { test.view() };
    REQUIRE(built.form.has_value());
    CHECK(built.form->id == u8"create-pull-request-dialog");
    REQUIRE(built.form->fields.size() == 3);
    CHECK(built.form->fields[0].view.displayed_text() == u8"main");
    REQUIRE(built.form->checks.size() == 2);
    CHECK(built.form->checks[1].checked);

    test.type(1, u8"새 기능을 더한다");
    test.type(2, u8"설명");
    test.send(form_check_intent { 0 });
    built = test.view();
    CHECK(built.form->checks[0].checked);
    const std::size_t jobs_before { test.jobs.size() };
    test.send(form_button_intent { u8"submit" });
    CHECK(test.view().form.has_value() == false);
    // push를 먼저 낸다. API 일감은 아직 없다.
    const push_execute_request* const push { test.last_request<push_execute_request>() };
    REQUIRE(push != nullptr);
    CHECK(push->remote == "origin");
    CHECK(push->branch == "feature/graph");
    CHECK(push->set_upstream);
    CHECK(test.jobs.size() == jobs_before);
    test.settle();
    REQUIRE(test.jobs.size() == jobs_before + 1);
    REQUIRE(std::holds_alternative<hosting_create_job>(test.jobs.back().work));
    const create_pull_request_params& params { std::get<hosting_create_job>(test.jobs.back().work).params };
    CHECK(params.title == "새 기능을 더한다");
    CHECK(params.body == "설명");
    CHECK(params.draft);
    CHECK(params.source_branch == "feature/graph");
    CHECK(params.target_branch == "main");

    created_pull_request created {};
    created.number = 4;
    created.title = params.title;
    created.web_url = "https://github.com/octo/hello/pull/4";
    created.head_sha = std::string(40, 'c');
    test.deliver(test.jobs.back(), hosting_result<created_pull_request> { .value = created });
    built = test.view();
    REQUIRE(built.repository->hosting->detail.has_value());
    CHECK(built.repository->hosting->detail->number == 4);
    CHECK(built.repository->hosting->detail->just_created);
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->text.find(u8"https://github.com/octo/hello/pull/4") != std::u8string::npos);
}

TEST_CASE("the create dialog is refused while signed out or on a detached HEAD", "[hosting][shell]")
{
    harness test {};
    test.activate(hosting_row_create);
    CHECK(test.view().form.has_value() == false);
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->warning);
}

TEST_CASE("checking out a pull request fetches pull/N/head into pr/N then switches to it", "[hosting][shell]")
{
    harness test {};
    test.sign_in_and_load({ make_pull(7, "seven") });
    test.activate("7");
    test.send(hosting_detail_action_intent { hosting_detail_action::checkout });
    const fetch_request* const fetch { test.last_request<fetch_request>() };
    REQUIRE(fetch != nullptr);
    CHECK(fetch->remote == "origin");
    CHECK(fetch->refspec == "pull/7/head:pr/7");
    CHECK(fetch->prune == false);
    // fetch가 성공해야 체크아웃이 이어진다.
    CHECK(test.last_request<checkout_branch_request>() == nullptr);
    test.settle();
    const checkout_branch_request* const checkout { test.last_request<checkout_branch_request>() };
    REQUIRE(checkout != nullptr);
    CHECK(checkout->name == "pr/7");
}

TEST_CASE("open in browser hands the address to the opener only for http and https", "[hosting][shell]")
{
    harness test {};
    pull_request_info bad { make_pull(2, "bad") };
    bad.web_url = "javascript:alert(1)";
    test.sign_in_and_load({ make_pull(1, "one"), bad });
    test.activate("1");
    test.send(hosting_detail_action_intent { hosting_detail_action::open_browser });
    REQUIRE(test.opened.size() == 1);
    CHECK(test.opened[0] == "https://github.com/octo/hello/pull/1");
    test.activate("2");
    test.send(hosting_detail_action_intent { hosting_detail_action::open_browser });
    CHECK(test.opened.size() == 1);
}

TEST_CASE("signing out from the hosting page clears the sidebar list and tells the bridge", "[hosting][shell]")
{
    harness test {};
    test.sign_in_and_load({ make_pull(1, "one") });
    REQUIRE(test.row(u8"pr:n:1") != nullptr);
    test.send(hosting_sign_out_intent { "github.com" });
    CHECK(test.sign_outs == std::vector<std::string> { "github.com" });
    CHECK(test.row(u8"pr:n:1") == nullptr);
    CHECK(test.row(u8"pr:login") != nullptr);
}

TEST_CASE("the section can be collapsed with the shared toggle", "[hosting][shell]")
{
    harness test {};
    test.sign_in_and_load({ make_pull(1, "one") });
    test.send(toggle_sidebar_section_intent { sidebar_section::pull_requests });
    CHECK(test.row(u8"h:PULL REQUESTS")->collapsed);
    CHECK(test.row(u8"pr:n:1") == nullptr);
    test.send(toggle_sidebar_section_intent { sidebar_section::pull_requests });
    CHECK(test.row(u8"pr:n:1") != nullptr);
}

TEST_CASE("the hosting commands are registered and follow the sign-in state", "[hosting][shell][commands]")
{
    for (const char* id : { "app.settings.hosting", "hosting.sign_in", "hosting.refresh", "hosting.pull_request.new" })
    {
        CAPTURE(id);
        REQUIRE(find_command(id) != nullptr);
        CHECK(find_command(id)->in_palette);
    }
    command_availability none {};
    CHECK(command_available("hosting.sign_in", none) == false);
    CHECK(command_available("hosting.refresh", none) == false);
    CHECK(command_available("hosting.pull_request.new", none) == false);
    command_availability supported { .hosting_supported = true };
    CHECK(command_available("hosting.sign_in", supported));
    CHECK(command_available("hosting.refresh", supported) == false);
    supported.hosting_signed_in = true;
    CHECK(command_available("hosting.sign_in", supported) == false);
    CHECK(command_available("hosting.refresh", supported));
    CHECK(command_available("hosting.pull_request.new", supported) == false);
    supported.hosting_can_create = true;
    CHECK(command_available("hosting.pull_request.new", supported));
    CHECK(command_group_title(command_group::hosting).empty() == false);
}

TEST_CASE("running the hosting commands does what the sidebar rows do", "[hosting][shell][commands]")
{
    harness test {};
    // 로그인하지 않았으면 새로 고침·만들기는 아무 일도 하지 않고, 로그인 명령이 로그인 일감을 낸다.
    test.send(run_command_intent { "hosting.refresh" });
    test.send(run_command_intent { "hosting.pull_request.new" });
    CHECK(test.jobs.empty());
    CHECK(test.view().form.has_value() == false);
    test.send(run_command_intent { "hosting.sign_in" });
    REQUIRE(test.jobs.size() == 1);
    CHECK(std::holds_alternative<hosting_sign_in_job>(test.jobs[0].work));
    test.deliver(test.jobs[0], hosting_result<std::string> { .value = std::string { "octocat" } });
    const std::size_t jobs = test.jobs.size();
    test.send(run_command_intent { "hosting.refresh" });
    CHECK(test.jobs.size() == jobs + 1);
    CHECK(std::holds_alternative<hosting_list_job>(test.jobs.back().work));
    test.send(run_command_intent { "hosting.pull_request.new" });
    REQUIRE(test.view().form.has_value());
    CHECK(test.view().form->id == u8"create-pull-request-dialog");
    // 설정 쪽을 여는 명령.
    test.send(close_settings_intent {});
    test.send(run_command_intent { "app.settings.hosting" });
    REQUIRE(test.view().settings.has_value());
    CHECK(test.view().settings->page == settings_page::hosting);
}
