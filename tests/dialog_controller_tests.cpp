// 대화 상자·메뉴·끌어 놓기·충돌 해결의 화면 상태 흐름이다. workspace는 가짜 runner와 수동 dispatcher로 세우고, job은
// 실행하지 않고 낸 operation 요청만 읽는다. operation 사건은 test가 직접 넣는다.
#include "application/app_messages.h"
#include "application/git_workspace.h"
#include "domain/credential_prompt.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/shell_controller.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    namespace snap = cyella::test::snapshot;

    struct askpass_recorder
    {
        [[nodiscard]] askpass_reply_callback callback()
        {
            return [this](askpass_reply reply) { replies.push_back(std::move(reply)); };
        }

        std::vector<askpass_reply> replies {};
    };

    struct harness
    {
        // `hook`을 주면 인증 대화 상자가 내는 백엔드 메시지(`answer_input_intent`·`cancel_input_intent`)를 workspace 대신 그리로
        // 넘긴다 (driver가 꽂는 처리기의 자리).
        explicit harness(std::function<void(const luil::app_message&)> hook = {})
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(workspace, std::move(hook)) }
        {
            controller.start({});
            controller.open_repository(u8"C:\\work\\repo");
            repository = controller.selected_repository_id();
        }

        static shell_controller::dependencies make_dependencies(git_workspace& workspace, std::function<void(const luil::app_message&)> hook)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.send_backend_intent = std::move(hook);
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } }; };
            dependencies.temporary_directory = u8"C:\\temp\\cyella-test";
            dependencies.ssh_executable = u8"C:/Git/usr/bin/ssh.exe";
            return dependencies;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            controller.handle(luil::app_message { std::move(intent) });
            controller.after_events();
        }

        // 읽어 둔 저장소다. main(현재)은 가장 새 커밋, feature/graph·origin/main·v1.0은 가장 오래된 커밋을 가리킨다.
        repository_session& load(const std::size_t count = 5, const std::uint32_t changed = 0)
        {
            repository_session* const session { workspace.find_session(repository) };
            REQUIRE(session != nullptr);
            snap::load(*session, snap::linear_history(count), changed);
            return *session;
        }

        [[nodiscard]] view_snapshot view()
        {
            return controller.make_view(1440.0f, 900.0f, 1.0f);
        }

        // 마지막으로 낸 operation의 요청이다. 쓰기가 끝난 뒤 세션이 자동으로 내는 refresh는 세지 않는다.
        template<typename request_type>
        [[nodiscard]] const request_type* last_request() const
        {
            const operation_id id { last_operation() };
            if (id == 0)
                return nullptr;
            const git_operation* const request { workspace.request_of(id) };
            return request == nullptr ? nullptr : std::get_if<request_type>(request);
        }

        // 사용자나 controller가 낸 operation 수다 (`automatic`이 아닌 것).
        [[nodiscard]] std::size_t operation_count() const
        {
            std::size_t count { 0 };
            for (const operation_id id : workspace.operation_order())
                if (const operation_status* const status { workspace.find_operation(id) }; status != nullptr && status->automatic == false)
                    ++count;
            return count;
        }

        [[nodiscard]] operation_id last_operation() const
        {
            const std::deque<operation_id>& order { workspace.operation_order() };
            for (auto it { order.rbegin() }; it != order.rend(); ++it)
                if (const operation_status* const status { workspace.find_operation(*it) }; status != nullptr && status->automatic == false)
                    return *it;
            return 0;
        }

        void type(const std::size_t field, const std::u8string& text)
        {
            luil::text_edit_request request {};
            request.target = to_luil_target(static_cast<text_target>(static_cast<std::uint32_t>(text_target::form_field_0) + field));
            request.command = luil::text::text_edit_command::insert;
            request.text = text;
            send(edit_text_intent { request });
        }

        void press(const std::u8string& key)
        {
            send(form_button_intent { key });
        }

        // worker가 실행기에 있다고 보고 사건을 반영한다.
        void start(const operation_id id)
        {
            workspace.apply(operation_event { id, repository, operation_started {} });
        }

        void finish(const operation_id id, const operation_state state, std::optional<operation_failure> failure = std::nullopt, std::optional<input_request> needs_input = std::nullopt)
        {
            workspace.apply(operation_event { id, repository, operation_finished { state, std::move(failure), std::move(needs_input), {} } });
            controller.after_events();
        }

        void ask(const operation_id id, const std::uint64_t prompt_id, const std::u8string_view text, askpass_reply_callback reply)
        {
            workspace.apply(operation_event { id, {}, operation_input_requested { prompt_id, classify_credential_prompt(text), std::move(reply) } });
            controller.after_events();
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_operation_runner runner;
        git_workspace workspace;
        shell_controller controller;
        std::string repository {};
    };

    drop_party branch_party(const ref_kind kind, const std::string& name)
    {
        return { .kind = kind, .name = name, .sha = {}, .is_commit = false };
    }

    bool entry_enabled(const menu_view& menu, const std::u8string& key)
    {
        for (const menu_entry& entry : menu.entries)
            if (entry.key == key)
                return entry.enabled;
        return false;
    }

    bool has_entry(const menu_view& menu, const std::u8string& key)
    {
        for (const menu_entry& entry : menu.entries)
            if (entry.key == key)
                return true;
        return false;
    }
} // namespace

TEST_CASE("dropping a branch on another opens the drop menu and runs the picked action", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    // main(가장 새 커밋)을 feature/graph(가장 오래된 커밋)에 놓는다.
    test.send(drop_intent { branch_party(ref_kind::local, "main"), branch_party(ref_kind::local, "feature/graph"), 400.0f, 300.0f });
    REQUIRE(test.controller.state().menu.has_value());
    const view_snapshot view { test.view() };
    REQUIRE(view.menu.has_value());
    CHECK(view.menu->title == u8"main → feature/graph");
    CHECK(view.menu->x == 400.0f);
    // 놓은 쪽이 끈 쪽의 조상이라 fast-forward가 있고, 읽어 둔 이력으로 알아서 아무 것도 더 묻지 않는다.
    REQUIRE(has_entry(*view.menu, u8"fast_forward"));
    CHECK(entry_enabled(*view.menu, u8"fast_forward"));
    CHECK_FALSE(entry_enabled(*view.menu, u8"rebase"));
    const std::size_t before { test.operation_count() };

    test.send(menu_select_intent { u8"fast_forward" });
    CHECK_FALSE(test.controller.state().menu.has_value());
    REQUIRE(test.operation_count() == before + 1);
    const auto* const request { test.last_request<fast_forward_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->target == "feature/graph");
    CHECK(request->to == "main");
}

TEST_CASE("a drop menu asks the backend only for relations the loaded history cannot answer", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load() };
    // 읽지 않은 먼 커밋을 가리키는 브랜치를 더한다.
    repository_snapshot snapshot { session.snapshot() };
    snapshot.refs.push_back({ .kind = ref_kind::local, .name = "archive", .full_name = "refs/heads/archive", .target_sha = snap::sha_of(900) });
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));

    const std::size_t before { test.operation_count() };
    test.send(drop_intent { branch_party(ref_kind::local, "main"), branch_party(ref_kind::local, "archive"), 10.0f, 10.0f });
    const std::size_t asked { test.operation_count() - before };
    CHECK(asked == 4);
    CHECK(test.workspace.find_operation(test.last_operation())->kind == operation_kind::count_commits);
    // 같은 메뉴가 열려 있는 동안 다시 묻지 않는다.
    test.send(drop_intent { branch_party(ref_kind::local, "main"), branch_party(ref_kind::local, "archive"), 10.0f, 10.0f });
    CHECK(test.operation_count() - before == 8);

    // 읽어 둔 이력이 모르는 동안은 fast-forward가 꺼져 있고, 답이 오면 켜진다.
    const view_snapshot waiting { test.view() };
    REQUIRE(waiting.menu.has_value());
    CHECK_FALSE(entry_enabled(*waiting.menu, u8"fast_forward"));
    session.apply(ancestry_answered { .ancestor = snap::sha_of(900), .descendant = snap::sha_of(4), .is_ancestor = true });
    const view_snapshot answered { test.view() };
    REQUIRE(answered.menu.has_value());
    CHECK(entry_enabled(*answered.menu, u8"fast_forward"));
}

TEST_CASE("a drop on a commit offers a new branch there", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    drop_party commit_party {};
    commit_party.is_commit = true;
    commit_party.sha = snap::sha_of(2);
    test.send(drop_intent { branch_party(ref_kind::local, "main"), commit_party, 0.0f, 0.0f });
    const view_snapshot view { test.view() };
    REQUIRE(view.menu.has_value());
    CHECK(has_entry(*view.menu, u8"create_branch_here"));
    CHECK(has_entry(*view.menu, u8"reset_hard"));

    test.send(menu_select_intent { u8"create_branch_here" });
    REQUIRE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().dialog->kind == dialog_kind::create_branch);
    CHECK(test.controller.state().dialog->subject == snap::sha_of(2));
}

TEST_CASE("a rebase asks before autostashing a dirty worktree and runs at once on a clean one", "[presentation][dialogs]")
{
    harness test {};
    test.load(5, 2);
    // 변경이 있으면 --autostash를 확인받는다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    const std::size_t before { test.operation_count() };
    test.send(menu_select_intent { u8"rebase_current_onto" });
    CHECK(test.operation_count() == before);
    REQUIRE(test.view().form.has_value());
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const stashed { test.last_request<rebase_request>() };
    REQUIRE(stashed != nullptr);
    CHECK(stashed->branch == "main");
    CHECK(stashed->onto == "feature/graph");
    CHECK(stashed->autostash);

    harness clean {};
    clean.load();
    clean.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    const std::size_t clean_before { clean.operation_count() };
    clean.send(menu_select_intent { u8"rebase_current_onto" });
    REQUIRE(clean.operation_count() == clean_before + 1);
    CHECK_FALSE(clean.last_request<rebase_request>()->autostash);
}

TEST_CASE("hard reset is confirmed and other resets run at once", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    menu_target target { .kind = menu_target_kind::commit, .sha = snap::sha_of(1) };
    test.send(open_context_menu_intent { target, 10.0f, 10.0f });
    const std::size_t before { test.operation_count() };
    test.send(menu_select_intent { u8"reset_mixed" });
    REQUIRE(test.operation_count() == before + 1);
    const auto* const mixed { test.last_request<reset_request>() };
    REQUIRE(mixed != nullptr);
    CHECK(mixed->mode == reset_mode::mixed);
    CHECK(mixed->to == snap::sha_of(1));

    test.send(open_context_menu_intent { target, 10.0f, 10.0f });
    test.send(menu_select_intent { u8"reset_hard" });
    CHECK(test.operation_count() == before + 1);
    REQUIRE(test.controller.state().dialog.has_value());
    const view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"confirm-dialog");
    CHECK(view.form->buttons.back().tone == form_tone::danger);
    test.press(u8"cancel");
    CHECK_FALSE(test.controller.state().dialog.has_value());
    CHECK(test.operation_count() == before + 1);

    test.send(open_context_menu_intent { target, 10.0f, 10.0f });
    test.send(menu_select_intent { u8"reset_hard" });
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 2);
    const auto* const hard { test.last_request<reset_request>() };
    REQUIRE(hard != nullptr);
    CHECK(hard->mode == reset_mode::hard);
}

TEST_CASE("the new branch dialog validates the name live and submits create_branch", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(toolbar_intent { toolbar_action::branch });
    REQUIRE(test.controller.state().dialog.has_value());
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"branch-dialog");
    CHECK_FALSE(view.form->buttons.back().enabled);
    CHECK(view.form->error.empty());

    test.type(0, u8"bad name");
    view = test.view();
    CHECK_FALSE(view.form->error.empty());
    CHECK_FALSE(view.form->buttons.back().enabled);

    // 이미 있는 이름도 막는다.
    test.press(u8"cancel");
    test.send(toolbar_intent { toolbar_action::branch });
    test.type(0, u8"main");
    view = test.view();
    CHECK(view.form->error.find(u8"이미 있습니다") != std::u8string::npos);

    test.press(u8"cancel");
    test.send(toolbar_intent { toolbar_action::branch });
    test.type(0, u8"topic/new");
    view = test.view();
    CHECK(view.form->error.empty());
    CHECK(view.form->buttons.back().enabled);
    // "만든 뒤 체크아웃"은 기본으로 켜져 있고 끌 수 있다.
    REQUIRE(view.form->checks.size() == 1);
    CHECK(view.form->checks[0].checked);
    test.send(form_check_intent { 0 });
    const std::size_t before { test.operation_count() };
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const request { test.last_request<create_branch_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->name == "topic/new");
    CHECK(request->at.empty());
    CHECK_FALSE(request->checkout);
    CHECK_FALSE(test.controller.state().dialog.has_value());
}

TEST_CASE("a branch created at a commit keeps that commit", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(2) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"create_branch" });
    REQUIRE(test.controller.state().dialog.has_value());
    test.type(0, u8"from-commit");
    test.press(u8"submit");
    const auto* const request { test.last_request<create_branch_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->at == snap::sha_of(2));
    CHECK(request->checkout);
}

TEST_CASE("the tag dialog sends an annotated tag only with a message", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(3) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"create_tag" });
    test.type(0, u8"v2.0");
    view_snapshot view { test.view() };
    CHECK(view.form->buttons.back().enabled);
    REQUIRE(view.form->fields.size() == 1);

    // annotated를 고르면 메시지 칸이 서고 메시지가 있어야 한다.
    test.send(form_choose_intent { 1 });
    view = test.view();
    REQUIRE(view.form->fields.size() == 2);
    CHECK_FALSE(view.form->buttons.back().enabled);
    test.type(1, u8"릴리스");
    view = test.view();
    CHECK(view.form->buttons.back().enabled);
    const std::size_t before { test.operation_count() };
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const request { test.last_request<create_tag_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->name == "v2.0");
    CHECK(request->sha == snap::sha_of(3));
    CHECK(request->annotated);
    CHECK(request->message == "릴리스");

    // 이미 있는 태그 이름은 막는다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(3) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"create_tag" });
    test.type(0, u8"v1.0");
    view = test.view();
    CHECK_FALSE(view.form->error.empty());
    CHECK_FALSE(view.form->buttons.back().enabled);
}

TEST_CASE("the push dialog defaults to set-upstream without an upstream and warns about force", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    // feature/graph에는 upstream이 없다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"push" });
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"push-dialog");
    REQUIRE(view.form->checks.size() == 3);
    CHECK(view.form->checks[0].checked);
    CHECK_FALSE(view.form->checks[1].checked);
    CHECK_FALSE(view.form->checks[2].checked);
    REQUIRE(view.form->choices.size() == 1);
    CHECK(view.form->fields[0].view.text == u8"feature/graph");
    CHECK(view.form->preview.find(u8"--set-upstream origin feature/graph:feature/graph") != std::u8string::npos);
    CHECK(view.form->warnings.empty());

    test.send(form_check_intent { 2 });
    view = test.view();
    REQUIRE_FALSE(view.form->warnings.empty());
    CHECK(view.form->warnings.front().warning);
    CHECK(view.form->buttons.back().tone == form_tone::danger);
    test.send(form_check_intent { 1 });

    const std::size_t before { test.operation_count() };
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const request { test.last_request<push_execute_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->remote == "origin");
    CHECK(request->branch == "feature/graph");
    CHECK(request->remote_branch == "feature/graph");
    CHECK(request->set_upstream);
    CHECK(request->follow_tags);
    CHECK(request->force_with_lease);
}

TEST_CASE("the set-upstream dialog needs an existing remote branch and sets the upstream without pushing", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    // feature/graph에는 upstream이 없고 origin/feature/graph도 없다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"set_upstream" });
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"set-upstream-dialog");
    REQUIRE(view.form->choices.size() == 1);
    CHECK_FALSE(view.form->error.empty());
    CHECK_FALSE(view.form->buttons.back().enabled);
    CHECK(view.form->preview == u8"git branch --set-upstream-to=origin/feature/graph feature/graph");

    // 원격 브랜치가 있는 이름(main은 origin/main을 가진다)이면 단추가 켜지고, push가 아니라 set_upstream을 낸다.
    test.press(u8"cancel");
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "main", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"set_upstream" });
    view = test.view();
    REQUIRE(view.form.has_value());
    CHECK(view.form->error.empty());
    REQUIRE(view.form->buttons.back().enabled);
    CHECK(view.form->fields[0].view.text == u8"main");
    const std::size_t before { test.operation_count() };
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const request { test.last_request<set_upstream_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->branch == "main");
    CHECK(request->remote == "origin");
    CHECK(request->remote_branch == "main");
    CHECK(test.last_request<push_execute_request>() == nullptr);
    CHECK_FALSE(test.controller.state().dialog.has_value());
}

TEST_CASE("unsetting the upstream goes straight to a branch operation", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "main", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    const std::size_t before { test.operation_count() };
    test.send(menu_select_intent { u8"unset_upstream" });
    REQUIRE(test.operation_count() == before + 1);
    const auto* const request { test.last_request<unset_upstream_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->branch == "main");
}

TEST_CASE("an untracked file goes to the recycle bin after a confirmation that says so", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(discard_files_intent { { "new.txt" }, true });
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"파일을 휴지통으로 보낼까요?");
    bool says_recycle_bin { false };
    for (const form_note& note : view.form->notes)
        says_recycle_bin = says_recycle_bin || note.text.find(u8"휴지통으로 보냅니다") != std::u8string::npos;
    CHECK(says_recycle_bin);
    CHECK(test.last_request<discard_files_request>() == nullptr);
    test.press(u8"submit");
    const auto* const request { test.last_request<trash_files_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->paths == std::vector<std::string> { "new.txt" });

    // 추적 중인 파일은 그대로 restore다.
    test.send(discard_files_intent { { "tracked.txt" }, false });
    view = test.view();
    CHECK(view.form->title == u8"변경을 버릴까요?");
    test.press(u8"submit");
    CHECK(test.last_request<discard_files_request>() != nullptr);
}

TEST_CASE("the toolbar push opens the dialog only when there is no upstream", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    // main에는 upstream(origin/main)이 있다 — 그냥 낸다.
    const std::size_t before { test.operation_count() };
    test.send(toolbar_intent { toolbar_action::push });
    CHECK(test.operation_count() == before + 1);
    CHECK(test.last_request<push_execute_request>() != nullptr);
    CHECK_FALSE(test.controller.state().dialog.has_value());
}

TEST_CASE("deleting a tag with its remote copy deletes the remote one first and keeps the local tag when that fails", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    for (ref& value : snapshot.refs)
        if (value.name == "v1.0")
            value.remotes = { "origin" };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));

    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::tag, .name = "v1.0", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"delete_local" });
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    REQUIRE(view.form->checks.size() == 1);
    CHECK(view.form->checks[0].label.find(u8"origin") != std::u8string::npos);
    test.send(form_check_intent { 0 });
    const std::size_t before { test.operation_count() };
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const remote { test.last_request<delete_remote_tag_request>() };
    REQUIRE(remote != nullptr);
    CHECK(remote->remote == "origin");
    CHECK(remote->name == "v1.0");

    // 원격 삭제가 실패하면 로컬은 남긴다.
    test.start(test.last_operation());
    test.finish(test.last_operation(), operation_state::failed, operation_failure { failure_kind::other, u8"거절", {} });
    CHECK(test.operation_count() == before + 1);

    // 성공하면 이어서 로컬 태그를 지운다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::tag, .name = "v1.0", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"delete_local" });
    test.send(form_check_intent { 0 });
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 2);
    const operation_id first { test.last_operation() };
    test.start(first);
    test.finish(first, operation_state::succeeded);
    REQUIRE(test.operation_count() >= before + 3);
    bool deleted_local { false };
    for (const operation_id id : test.workspace.operation_order())
        if (const git_operation* request { test.workspace.request_of(id) }; request != nullptr && std::holds_alternative<delete_tag_request>(*request))
            deleted_local = true;
    CHECK(deleted_local);
}

TEST_CASE("tag push and tags section push use the remote named in the entry", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::tag, .name = "v1.0", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"push:origin" });
    const auto* const tag { test.last_request<push_tag_request>() };
    REQUIRE(tag != nullptr);
    CHECK(tag->remote == "origin");
    CHECK(tag->name == "v1.0");

    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::tags_section }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"push_all:origin" });
    const auto* const all { test.last_request<push_tags_request>() };
    REQUIRE(all != nullptr);
    CHECK(all->remote == "origin");
}

TEST_CASE("checkout blocked by the worktree opens the policy dialog and resubmits with the chosen policy", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    const operation_id blocked { test.workspace.submit(test.repository, checkout_branch_request { "feature/graph" }) };
    REQUIRE(blocked != 0);
    test.start(blocked);
    input_request needed {};
    needed.kind = input_kind::dirty_worktree;
    needed.files = { "a.txt", "b.txt", "c.txt", "d.txt", "e.txt", "f.txt", "g.txt" };
    needed.target = "feature/graph";
    test.finish(blocked, operation_state::needs_input, std::nullopt, needed);

    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"dirty-dialog");
    CHECK_FALSE(view.banner.has_value());
    // 파일은 5개까지 보이고 나머지는 "외 N개"다.
    std::size_t listed { 0 };
    bool more { false };
    for (const form_note& note : view.form->notes)
    {
        listed += note.code ? 1 : 0;
        more = more || note.text == u8"외 2개";
    }
    CHECK(listed == 5);
    CHECK(more);
    REQUIRE(view.form->choices.size() == 3);
    CHECK(view.form->chosen == 0);

    // 변경 버리기는 한 번 더 확인한다.
    test.send(form_choose_intent { 2 });
    test.press(u8"submit");
    view = test.view();
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"변경을 버리고 이동할까요?");
    const std::size_t before { test.operation_count() };
    test.press(u8"back");
    view = test.view();
    CHECK(view.form->choices.size() == 3);
    CHECK(test.operation_count() == before);

    test.send(form_choose_intent { 0 });
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const retry { test.last_request<checkout_branch_request>() };
    REQUIRE(retry != nullptr);
    CHECK(retry->name == "feature/graph");
    CHECK(retry->policy == checkout_policy::stash_and_reapply);
    // 새 operation이 오래된 대화 상자를 대신한다.
    CHECK_FALSE(test.view().form.has_value());
}

TEST_CASE("cancelling the policy dialog leaves the checkout undone and closes the prompt", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    const operation_id blocked { test.workspace.submit(test.repository, checkout_branch_request { "feature/graph" }) };
    test.start(blocked);
    input_request needed {};
    needed.kind = input_kind::dirty_worktree;
    needed.files = { "a.txt" };
    needed.target = "feature/graph";
    test.finish(blocked, operation_state::needs_input, std::nullopt, needed);
    REQUIRE(test.view().form.has_value());
    const std::size_t before { test.operation_count() };
    test.press(u8"cancel");
    CHECK(test.operation_count() == before);
    const view_snapshot view { test.view() };
    CHECK_FALSE(view.form.has_value());
    CHECK_FALSE(view.banner.has_value());
}

TEST_CASE("a password prompt is answered from the masked field and the input is wiped", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id fetching { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(fetching);
    askpass_recorder recorder {};
    test.ask(fetching, 7, u8"Password for 'https://alice@example.com': ", recorder.callback());

    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"auth-dialog");
    REQUIRE(view.form->fields.size() == 1);
    CHECK(view.form->fields[0].secret);
    CHECK(view.tabs.front().marker == tab_marker::needs_input);

    test.type(0, u8"s3cret");
    view = test.view();
    CHECK(view.form->fields[0].view.text == u8"s3cret");
    test.send(form_check_intent { 0 });
    view = test.view();
    CHECK(view.form->fields[0].reveal);

    test.press(u8"answer");
    REQUIRE(recorder.replies.size() == 1);
    CHECK(recorder.replies[0].answered);
    CHECK(recorder.replies[0].answer == u8"s3cret");
    // 입력 상자는 비워지고 operation은 다시 돈다.
    for (const luil::text::text_edit_state& field : test.controller.state().form.fields)
        CHECK(field.text.empty());
    CHECK(test.workspace.find_operation(fetching)->state == operation_state::running);
    CHECK_FALSE(test.view().form.has_value());
}

TEST_CASE("answering and closing a prompt send the backend intents", "[presentation][dialogs][auth]")
{
    std::vector<luil::app_message> sent {};
    harness test { [&sent](const luil::app_message& message) { sent.push_back(message); } };
    test.load();
    const operation_id fetching { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(fetching);
    askpass_recorder recorder {};
    test.ask(fetching, 11, u8"Password for 'https://bob@example.com': ", recorder.callback());
    test.type(0, u8"pw-1");
    test.press(u8"answer");
    REQUIRE(sent.size() == 1);
    const auto* const answer { sent[0].get<answer_input_intent>() };
    REQUIRE(answer != nullptr);
    CHECK(answer->id == fetching);
    CHECK(answer->prompt_id == 11);
    CHECK(answer->answer == u8"pw-1");
    // 처리기가 받아 가기만 했으므로 workspace의 질문은 그대로이고, 화면의 입력은 비워졌다.
    CHECK(recorder.replies.empty());
    for (const luil::text::text_edit_state& field : test.controller.state().form.fields)
        CHECK(field.text.empty());

    test.press(u8"cancel");
    REQUIRE(sent.size() == 2);
    const auto* const closed { sent[1].get<cancel_input_intent>() };
    REQUIRE(closed != nullptr);
    CHECK(closed->id == fetching);
    CHECK(closed->prompt_id == 11);
}

TEST_CASE("prompts from setup operations without a repository open the same dialog", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    // clone·init·ssh-keygen 같은 저장소 없는 작업도 인증을 묻는다. 이 탭의 저장소 operation이 아니어도 상자가 뜬다.
    const operation_id cloning { test.workspace.submit_setup(clone_request { .url = "https://example.com/r.git", .target_path = u8"C:\\work\\clone" }) };
    REQUIRE(cloning != 0);
    test.workspace.apply(operation_event { cloning, {}, operation_started {} });
    askpass_recorder recorder {};
    test.workspace.apply(operation_event { cloning, {}, operation_input_requested { 1, classify_credential_prompt(u8"Enter passphrase for key '/k': "), recorder.callback() } });
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"SSH 키 passphrase");
    test.type(0, u8"pp");
    test.press(u8"answer");
    REQUIRE(recorder.replies.size() == 1);
    CHECK(recorder.replies[0].answer == u8"pp");
    CHECK_FALSE(test.view().form.has_value());

    // 새 키 passphrase와 확인 문구도 같은 상자다.
    askpass_recorder confirm {};
    test.workspace.apply(operation_event { cloning, {}, operation_input_requested { 2, classify_credential_prompt(u8"Enter same passphrase again: "), confirm.callback() } });
    view = test.view();
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"passphrase 확인");
    test.press(u8"cancel");
    CHECK(confirm.replies.size() == 1);
}

TEST_CASE("an ssh retry arrives as a new prompt id and clears the previous answer", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id fetching { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(fetching);
    askpass_recorder first {};
    test.ask(fetching, 1, u8"Enter passphrase for key '/home/u/.ssh/id_ed25519': ", first.callback());
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"SSH 키 passphrase");
    test.type(0, u8"wrong");
    test.press(u8"answer");
    REQUIRE(first.replies.size() == 1);

    askpass_recorder second {};
    test.ask(fetching, 2, u8"Enter passphrase for key '/home/u/.ssh/id_ed25519': ", second.callback());
    view = test.view();
    REQUIRE(view.form.has_value());
    CHECK(view.form->fields[0].view.text.empty());
    // 질문 번호가 바뀌어 상자의 열쇠도 바뀌었다.
    test.press(u8"cancel");
    CHECK(second.replies.size() == 1);
    CHECK_FALSE(second.replies[0].answered);
}

TEST_CASE("a host key prompt shows the fingerprint and defaults to refusing", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id fetching { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(fetching);
    askpass_recorder recorder {};
    test.ask(fetching, 3,
        u8"The authenticity of host 'github.com (140.82.112.3)' can't be established.\nED25519 key fingerprint is SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG.\nAre you sure you want to "
        u8"continue connecting (yes/no/[fingerprint])? ",
        recorder.callback());
    const view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"처음 보는 호스트 키");
    bool host { false };
    bool fingerprint { false };
    for (const form_note& note : view.form->notes)
    {
        host = host || note.text.find(u8"github.com") != std::u8string::npos;
        fingerprint = fingerprint || note.text.find(u8"SHA256:abcdefghij") != std::u8string::npos;
    }
    CHECK(host);
    CHECK(fingerprint);
    REQUIRE(view.form->buttons.size() == 3);
    CHECK(view.form->buttons[0].key == u8"no");
    CHECK(view.form->buttons[0].default_button);
    CHECK(view.form->buttons[1].key == u8"once");
    CHECK(view.form->buttons[2].key == u8"yes");

    test.send(form_enter_intent {});
    REQUIRE(recorder.replies.size() == 1);
    CHECK(recorder.replies[0].answer == u8"no");
}

TEST_CASE("trusting a host once retries the operation with a temporary known_hosts", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id pushing { test.workspace.submit(test.repository, push_execute_request { .remote = "origin", .branch = "main", .follow_tags = true }) };
    test.start(pushing);
    askpass_recorder recorder {};
    test.ask(pushing, 5, u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ", recorder.callback());
    const std::size_t before { test.operation_count() };
    test.press(u8"once");

    // 질문 중이던 operation은 알림 없이 취소되고, 같은 요청이 임시 신뢰와 함께 다시 나간다.
    CHECK(test.workspace.find_operation(pushing)->cancel_requested);
    REQUIRE(test.operation_count() == before + 1);
    const auto* const retry { test.last_request<push_execute_request>() };
    REQUIRE(retry != nullptr);
    CHECK(retry->remote == "origin");
    CHECK(retry->follow_tags);
    REQUIRE(retry->ssh_command_override.has_value());
    CHECK(retry->ssh_command_override->command.find(u8"UserKnownHostsFile") != std::u8string::npos);
    CHECK(retry->ssh_command_override->known_hosts_path.find(u8"cyella-known-hosts-") != std::u8string::npos);
    // 사용자의 known_hosts를 건드리는 "yes"로는 답하지 않는다.
    CHECK(recorder.replies.empty() == false);
    CHECK_FALSE(recorder.replies[0].answered);

    // 다시 낸 작업이 성공하면 같은 임시 신뢰로 원격 태그를 한 번 갱신한다 (자동 후속은 임시 파일이 없어 다시 묻는다).
    const operation_id retried { test.last_operation() };
    test.start(retried);
    const std::size_t count { test.operation_count() };
    test.finish(retried, operation_state::succeeded);
    static_cast<void>(count);
    const refresh_remote_tags_request* tags { nullptr };
    for (const operation_id id : test.workspace.operation_order())
        if (const git_operation* request { test.workspace.request_of(id) }; request != nullptr && std::holds_alternative<refresh_remote_tags_request>(*request))
            if (const auto* candidate { std::get_if<refresh_remote_tags_request>(request) }; candidate->ssh_command_override.has_value())
                tags = candidate;
    REQUIRE(tags != nullptr);
    CHECK(tags->ssh_command_override->known_hosts_path != retry->ssh_command_override->known_hosts_path);
}

TEST_CASE("automatic operations that ask for credentials are cancelled silently", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id automatic { test.workspace.submit(test.repository, refresh_remote_tags_request {}, true) };
    test.start(automatic);
    askpass_recorder recorder {};
    test.ask(automatic, 9, u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ", recorder.callback());
    // 사용자가 시작하지 않은 작업의 질문은 상자를 띄우지 않고 취소로 답한다.
    CHECK_FALSE(test.view().form.has_value());
    CHECK(test.workspace.find_operation(automatic)->cancel_requested);
}

TEST_CASE("credentials arrive as two sequential prompts", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id cloning { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(cloning);
    askpass_recorder user {};
    askpass_recorder password {};
    test.ask(cloning, 1, u8"Username for 'https://example.com': ", user.callback());
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"HTTPS 사용자 이름");
    CHECK_FALSE(view.form->fields[0].secret);
    test.type(0, u8"alice");
    test.send(form_enter_intent {});
    REQUIRE(user.replies.size() == 1);
    CHECK(user.replies[0].answer == u8"alice");

    test.ask(cloning, 2, u8"Password for 'https://alice@example.com': ", password.callback());
    view = test.view();
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"HTTPS 비밀번호 또는 토큰");
    CHECK(view.form->fields[0].secret);
    CHECK(view.form->fields[0].view.text.empty());
}

TEST_CASE("an unknown prompt shows the raw text with a free text field", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id fetching { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(fetching);
    askpass_recorder recorder {};
    test.ask(fetching, 1, u8"Verification code: ", recorder.callback());
    const view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    REQUIRE(view.form->fields.size() == 1);
    CHECK_FALSE(view.form->fields[0].secret);
    CHECK(view.form->notes.front().text == u8"Verification code:");
}

TEST_CASE("push rejections become action buttons that run the follow-up chain", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    const push_execute_request original { .remote = "origin", .branch = "main", .remote_branch = "main" };
    const operation_id pushing { test.workspace.submit(test.repository, original) };
    test.start(pushing);
    test.finish(pushing, operation_state::failed, operation_failure { failure_kind::rejected_non_ff, u8"원격이 앞서 있습니다.", u8"! [rejected]" });
    view_snapshot view { test.view() };
    REQUIRE(view.banner.has_value());
    REQUIRE(view.banner->actions.size() == 1);
    CHECK(view.banner->actions[0].key == u8"pull_then_push");

    const std::size_t before { test.operation_count() };
    test.send(banner_action_intent { u8"pull_then_push", pushing });
    REQUIRE(test.operation_count() == before + 1);
    const auto* const pull { test.last_request<pull_request>() };
    REQUIRE(pull != nullptr);
    CHECK(pull->strategy == pull_strategy::rebase);
    // 배너는 닫히고, pull이 끝나야 push가 다시 나간다.
    CHECK_FALSE(test.view().banner.has_value());
    const operation_id pulling { test.last_operation() };
    test.start(pulling);
    test.finish(pulling, operation_state::succeeded);
    REQUIRE(test.operation_count() == before + 2);
    const auto* const again { test.last_request<push_execute_request>() };
    REQUIRE(again != nullptr);
    CHECK(again->remote == "origin");
    CHECK(again->branch == "main");

    // 같은 거절이 stale info면 fetch를 권한다.
    const operation_id forced { test.workspace.submit(test.repository, push_execute_request { .remote = "origin", .branch = "main", .force_with_lease = true }) };
    test.start(forced);
    test.finish(forced, operation_state::failed, operation_failure { failure_kind::stale_info, u8"stale", {} });
    view = test.view();
    REQUIRE(view.banner.has_value());
    REQUIRE(view.banner->actions.size() == 1);
    CHECK(view.banner->actions[0].key == u8"fetch");
}

TEST_CASE("a pull that cannot fast-forward offers a rebase pull and deleting an unmerged branch offers force", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    const operation_id pulling { test.workspace.submit(test.repository, pull_request { pull_strategy::ff_only }) };
    test.start(pulling);
    test.finish(pulling, operation_state::failed, operation_failure { failure_kind::not_possible_to_fast_forward, u8"fast-forward할 수 없습니다.", {} });
    view_snapshot view { test.view() };
    REQUIRE(view.banner.has_value());
    REQUIRE(view.banner->actions.size() == 1);
    CHECK(view.banner->actions[0].key == u8"pull_rebase");
    test.send(banner_action_intent { u8"pull_rebase", pulling });
    const auto* const rebased { test.last_request<pull_request>() };
    REQUIRE(rebased != nullptr);
    CHECK(rebased->strategy == pull_strategy::rebase);

    const operation_id deleting { test.workspace.submit(test.repository, delete_branch_request { .name = "topic", .force = false }) };
    test.start(deleting);
    test.finish(deleting, operation_state::failed, operation_failure { failure_kind::other, u8"삭제하지 못했습니다.", u8"error: the branch 'topic' is not fully merged." });
    view = test.view();
    REQUIRE(view.banner.has_value());
    REQUIRE(view.banner->actions.size() == 1);
    CHECK(view.banner->actions[0].key == u8"force_delete");
    test.send(banner_action_intent { u8"force_delete", deleting });
    REQUIRE(test.controller.state().dialog.has_value());
    test.press(u8"submit");
    const auto* const forced { test.last_request<delete_branch_request>() };
    REQUIRE(forced != nullptr);
    CHECK(forced->force);
}

TEST_CASE("double clicking a commit checks out its branch, or asks about detached HEAD", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    // feature/graph가 붙은 가장 오래된 커밋이다.
    const std::size_t before { test.operation_count() };
    test.send(activate_commit_intent { snap::sha_of(0) });
    REQUIRE(test.operation_count() == before + 1);
    const auto* const branch { test.last_request<checkout_branch_request>() };
    REQUIRE(branch != nullptr);
    CHECK(branch->name == "feature/graph");

    // 브랜치가 없는 커밋은 detached HEAD 경고를 한 번 묻고, "다시 보지 않기"로 끈다.
    test.send(activate_commit_intent { snap::sha_of(2) });
    CHECK(test.operation_count() == before + 1);
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"detached HEAD로 이동");
    test.send(form_check_intent { 0 });
    test.press(u8"submit");
    const auto* const detached { test.last_request<checkout_detached_request>() };
    REQUIRE(detached != nullptr);
    CHECK(detached->revision == snap::sha_of(2));
    test.send(activate_commit_intent { snap::sha_of(3) });
    CHECK_FALSE(test.view().form.has_value());
    const auto* const direct { test.last_request<checkout_detached_request>() };
    REQUIRE(direct != nullptr);
    CHECK(direct->revision == snap::sha_of(3));
}

TEST_CASE("discard asks first and the working tree menu stages and ignores", "[presentation][dialogs]")
{
    harness test {};
    test.load(5, 2);
    const std::size_t before { test.operation_count() };
    test.send(discard_files_intent { { "src/a.cpp" } });
    CHECK(test.operation_count() == before);
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->buttons.back().tone == form_tone::danger);
    test.press(u8"submit");
    const auto* const discard { test.last_request<discard_files_request>() };
    REQUIRE(discard != nullptr);
    CHECK(discard->paths == std::vector<std::string> { "src/a.cpp" });

    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::file, .name = "src/a.cpp", .side = change_side::unstaged }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"ignore" });
    const auto* const ignore { test.last_request<ignore_path_request>() };
    REQUIRE(ignore != nullptr);
    CHECK(ignore->pattern == "/src/a.cpp");
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::file, .name = "src/a.cpp", .side = change_side::unstaged }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"stage" });
    CHECK(test.last_request<stage_files_request>() != nullptr);
}

TEST_CASE("stash menu applies, pops and confirms the drop", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::stash, .name = "stash@{2}", .index = 2 }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"apply" });
    const auto* const apply { test.last_request<stash_apply_request>() };
    REQUIRE(apply != nullptr);
    CHECK(apply->index == 2);
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::stash, .name = "stash@{2}", .index = 2 }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"drop" });
    CHECK(test.last_request<stash_drop_request>() == nullptr);
    test.press(u8"submit");
    CHECK(test.last_request<stash_drop_request>() != nullptr);
}

TEST_CASE("commit menu runs cherry-pick and copies the sha", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(1) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"cherry_pick" });
    const auto* const pick { test.last_request<cherry_pick_request>() };
    REQUIRE(pick != nullptr);
    CHECK(pick->sha == snap::sha_of(1));
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(1) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"copy_sha" });
    const std::optional<std::u8string> copied { test.controller.take_clipboard_text() };
    REQUIRE(copied.has_value());
    const std::string expected { snap::sha_of(1) };
    CHECK(*copied == std::u8string { expected.begin(), expected.end() });
    CHECK_FALSE(test.controller.take_clipboard_text().has_value());
    // WIP 행에는 메뉴가 없다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = "WIP" }, 0.0f, 0.0f });
    CHECK_FALSE(test.controller.state().menu.has_value());
}

TEST_CASE("the conflict screen lists files and sends continue only when none are left", "[presentation][dialogs][conflict]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.operation_state = repository_operation_state::rebase;
    snapshot.summary.in_progress = in_progress_operation {
        .kind = repository_operation_state::rebase, .step = 2, .total = 3, .current_summary = "feature commit", .onto = snap::sha_of(4), .head_name = "refs/heads/feature/graph"
    };
    snapshot.conflicts
        = { { .path = "src/a.cpp", .status = 'U', .side = change_side::unstaged, .conflict_code = "UU" }, { .path = "b.txt", .status = 'U', .side = change_side::unstaged, .conflict_code = "DU" } };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));

    view_snapshot view { test.view() };
    REQUIRE(view.repository.has_value());
    REQUIRE(view.repository->conflict.has_value());
    const conflict_view& conflict { *view.repository->conflict };
    CHECK(conflict.kind == repository_operation_state::rebase);
    CHECK(conflict.progress == u8"커밋 2/3 적용 중: feature commit");
    REQUIRE(conflict.files.size() == 2);
    CHECK(conflict.files[0].name == u8"a.cpp");
    CHECK(conflict.files[0].code == u8"UU");
    CHECK_FALSE(conflict.can_continue);
    CHECK(conflict.can_skip);

    // abort는 확인을 거친다.
    const std::size_t before { test.operation_count() };
    test.send(conflict_action_intent { conflict_action::abort_sequence });
    CHECK(test.operation_count() == before);
    REQUIRE(test.view().form.has_value());
    test.press(u8"submit");
    const auto* const abort_request { test.last_request<sequence_control_request>() };
    REQUIRE(abort_request != nullptr);
    CHECK(abort_request->kind == operation_kind::rebase_abort);

    test.send(conflict_action_intent { conflict_action::skip_sequence });
    CHECK(test.last_request<sequence_control_request>()->kind == operation_kind::rebase_skip);

    // 충돌이 다 풀리면 계속할 수 있다.
    snapshot.conflicts.clear();
    const std::uint64_t next { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { next, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = next, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    // 앞서 낸 abort·skip이 아직 돌고 있다고 보지 않게 끝낸다.
    for (const operation_id id : std::vector<operation_id> { test.workspace.operation_order().begin(), test.workspace.operation_order().end() })
        if (const operation_status* status { test.workspace.find_operation(id) }; status != nullptr && status->finished() == false)
        {
            test.start(id);
            test.finish(id, operation_state::succeeded);
        }
    view = test.view();
    REQUIRE(view.repository->conflict.has_value());
    CHECK(view.repository->conflict->can_continue);
    test.send(conflict_action_intent { conflict_action::continue_sequence });
    CHECK(test.last_request<sequence_control_request>()->kind == operation_kind::rebase_continue);
}

TEST_CASE("merge conflicts have no skip and map to the merge operations", "[presentation][dialogs][conflict]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.operation_state = repository_operation_state::merge;
    snapshot.summary.in_progress.kind = repository_operation_state::merge;
    snapshot.conflicts = { { .path = "a.txt", .status = 'U', .side = change_side::unstaged, .conflict_code = "AA" } };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    const view_snapshot view { test.view() };
    REQUIRE(view.repository->conflict.has_value());
    CHECK_FALSE(view.repository->conflict->can_skip);
    const std::size_t before { test.operation_count() };
    test.send(conflict_action_intent { conflict_action::skip_sequence });
    CHECK(test.operation_count() == before);
    test.send(conflict_action_intent { conflict_action::abort_sequence });
    test.press(u8"submit");
    CHECK(test.last_request<sequence_control_request>()->kind == operation_kind::merge_abort);
}

TEST_CASE("amending an already pushed commit shows a warning", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load(5, 2) };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.head_pushed = true;
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    // WIP 행을 고르고 amend를 켠다.
    test.send(select_commit_intent { graph_item_key(std::string { graph_wip_sha }) });
    view_snapshot view { test.view() };
    REQUIRE(view.repository->working.has_value());
    CHECK_FALSE(view.repository->working->amend_warning);
    test.send(toggle_amend_intent {});
    view = test.view();
    CHECK(view.repository->working->amend_warning);
    // pushed가 아니면 경고하지 않는다.
    snapshot.summary.head_pushed = false;
    const std::uint64_t next { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { next, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = next, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    CHECK_FALSE(test.view().repository->working->amend_warning);
}
namespace {
    // 선택 상자의 거르는 칸에 글을 넣는다 (사용자가 친 것과 같다).
    void type_into_selector(harness& test, const std::u8string& text)
    {
        luil::text_edit_request request {};
        request.target = to_luil_target(text_target::selector_filter);
        request.command = luil::text::text_edit_command::insert;
        request.text = text;
        test.send(edit_text_intent { request });
    }

    std::vector<std::u8string> selector_keys(const view_snapshot& view)
    {
        std::vector<std::u8string> keys {};
        if (view.selector.has_value())
            for (const menu_entry& entry : view.selector->entries)
                keys.push_back(entry.key);
        return keys;
    }
} // namespace

TEST_CASE("the branch selector opens at the toolbar, filters by typing and checks out the picked branch", "[presentation][selector]")
{
    harness test {};
    test.load();
    CHECK_FALSE(test.view().selector.has_value());

    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    view_snapshot view { test.view() };
    REQUIRE(view.selector.has_value());
    CHECK(view.selector->kind == selector_kind::branch);
    CHECK(view.selector->x == 200.0f);
    CHECK(view.selector->y == 60.0f);
    CHECK(view.selector->width == 220.0f);
    CHECK(selector_keys(view) == std::vector<std::u8string> { u8"branch:main", u8"branch:feature/graph" });
    // 현재 브랜치(main)는 고를 수 없다.
    CHECK_FALSE(view.selector->entries[0].enabled);
    CHECK(view.selector->entries[1].enabled);

    type_into_selector(test, u8"GRAPH");
    view = test.view();
    CHECK(view.selector->filter.text == u8"GRAPH");
    CHECK(selector_keys(view) == std::vector<std::u8string> { u8"branch:feature/graph" });

    const std::size_t before { test.operation_count() };
    test.send(selector_select_intent { u8"branch:feature/graph" });
    CHECK_FALSE(test.view().selector.has_value());
    REQUIRE(test.operation_count() == before + 1);
    const auto* const request { test.last_request<checkout_branch_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->name == "feature/graph");
    CHECK(request->policy == checkout_policy::ask);

    // 열 때마다 거르는 글은 비어 있다.
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    CHECK(test.view().selector->filter.text.empty());
}

TEST_CASE("a checkout blocked by local changes goes through the existing policy dialog", "[presentation][selector]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    test.send(selector_select_intent { u8"branch:feature/graph" });
    const operation_id id { test.last_operation() };
    REQUIRE(id != 0);
    input_request blocked {};
    blocked.kind = input_kind::dirty_worktree;
    blocked.target = "feature/graph";
    blocked.files = { "a.txt" };
    test.start(id);
    test.finish(id, operation_state::needs_input, std::nullopt, blocked);
    const view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"dirty-dialog");
}

TEST_CASE("picking the current branch from the selector does nothing", "[presentation][selector]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    const std::size_t before { test.operation_count() };
    test.send(selector_select_intent { u8"branch:main" });
    CHECK(test.operation_count() == before);
    CHECK_FALSE(test.view().selector.has_value());
}

TEST_CASE("pressing the selector again closes it and an outside press does not reopen it on release", "[presentation][selector]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    REQUIRE(test.view().selector.has_value());
    // 열린 상자를 다시 누르면 닫힌다.
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    CHECK_FALSE(test.view().selector.has_value());

    // 바깥 누름(popup dismiss)이 먼저 닫고 곧바로 같은 상자의 뗌이 열기를 청한다: 닫힌 채 둔다.
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    test.send(close_selector_intent {});
    CHECK_FALSE(test.view().selector.has_value());
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    CHECK_FALSE(test.view().selector.has_value());
    // 다른 상자는 그대로 열린다.
    test.send(open_selector_intent { selector_kind::repository, 0.0f, 0.0f, 100.0f });
    CHECK(test.view().selector.has_value());
    // 한 상자가 열려 있는 동안 다른 상자를 열면 바뀐다.
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    REQUIRE(test.view().selector.has_value());
    CHECK(test.view().selector->kind == selector_kind::branch);
}

TEST_CASE("Esc, a context menu, a tab change and a chosen folder all close the selector", "[presentation][selector]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    test.send(close_overlays_intent {});
    CHECK_FALSE(test.view().selector.has_value());

    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(2) }, 10.0f, 10.0f });
    CHECK_FALSE(test.view().selector.has_value());
    CHECK(test.view().menu.has_value());

    test.send(close_menu_intent {});
    test.send(open_selector_intent { selector_kind::repository, 0.0f, 0.0f, 100.0f });
    test.send(folder_chosen_intent { folder_purpose::open, u8"C:\\work\\nowhere" });
    CHECK_FALSE(test.view().selector.has_value());
}

TEST_CASE("the repository selector switches between open tabs and reopens recent repositories", "[presentation][selector]")
{
    harness test {};
    test.load();
    const repository_tab_id first { test.controller.tabs().selected().id };
    test.controller.open_repository(u8"C:\\work\\other");
    const repository_tab_id second { test.controller.tabs().selected().id };
    REQUIRE(first != second);

    test.send(open_selector_intent { selector_kind::repository, 0.0f, 0.0f, 150.0f });
    view_snapshot view { test.view() };
    REQUIRE(view.selector.has_value());
    const auto tab_key = [](const repository_tab_id id) {
        const std::string digits { std::to_string(id) };
        return u8"tab:" + std::u8string { digits.begin(), digits.end() };
    };
    CHECK(selector_keys(view) == std::vector<std::u8string> { tab_key(first), tab_key(second), u8"open_folder" });

    // 다른 탭을 고르면 그 탭이 선택된다.
    test.send(selector_select_intent { tab_key(first) });
    CHECK(test.controller.tabs().selected().id == first);
    CHECK_FALSE(test.view().selector.has_value());

    // 탭을 닫으면 최근 목록에 남아 있고 고르면 다시 탭으로 열린다.
    test.send(close_tab_intent { second });
    test.send(open_selector_intent { selector_kind::repository, 0.0f, 0.0f, 150.0f });
    view = test.view();
    CHECK(selector_keys(view) == std::vector<std::u8string> { tab_key(first), u8"recent:C:\\work\\other", u8"open_folder" });
    test.send(selector_select_intent { u8"recent:C:\\work\\other" });
    CHECK(test.controller.tabs().tabs().size() == 2);
    CHECK(test.controller.tabs().selected().path == u8"C:\\work\\other");
}

TEST_CASE("the branch selector is not offered on the start page", "[presentation][selector]")
{
    harness test {};
    test.load();
    test.send(new_tab_intent {});
    REQUIRE(test.controller.tabs().selected().kind == repository_tab_kind::start);
    test.send(open_selector_intent { selector_kind::branch, 0.0f, 0.0f, 100.0f });
    CHECK_FALSE(test.view().selector.has_value());
}
namespace {
    // 경로 검사(`inspect_path`)를 돌려 사건을 반영한다.
    void settle_inspection(harness& test)
    {
        while (test.dispatcher.run())
        {
        }
        for (const operation_event& event : test.events.take())
            test.workspace.apply(event);
        test.controller.after_events();
    }
} // namespace

TEST_CASE("locating a missing recent repository replaces its path once the chosen folder proves to be a repository", "[presentation][locate]")
{
    harness test {};
    // 열어 둔 저장소(C:\work\repo)의 최근 항목을 옮긴 폴더로 바꾼다.
    const std::u8string old_path { u8"C:\\work\\repo" };
    REQUIRE(test.controller.settings().recent_repositories.empty() == false);
    REQUIRE(test.controller.settings().recent_repositories.front().path == old_path);

    test.files.add_directory(u8"C:/moved/place");
    test.process.when({ "rev-parse" }, test::scripted_response { 0, "C:/moved/place/.git\nC:/moved/place\n" });
    test.send(folder_chosen_intent { folder_purpose::locate, u8"C:\\moved\\place", old_path });
    settle_inspection(test);

    REQUIRE(test.controller.settings().recent_repositories.empty() == false);
    CHECK(test.controller.settings().recent_repositories.front().path == u8"C:\\moved\\place");
    CHECK(test.controller.settings().recent_repositories.front().display_name == u8"place");
    REQUIRE(test.controller.state().notice.has_value());
    CHECK_FALSE(test.controller.state().notice->warning);
    CHECK(test.controller.state().notice->text.find(u8"위치를 바꿨습니다") != std::u8string::npos);
}

TEST_CASE("locating to a folder that is not a repository changes nothing and says so", "[presentation][locate]")
{
    harness test {};
    const std::u8string old_path { u8"C:\\work\\repo" };
    test.files.add_directory(u8"C:/plain/folder");
    // rev-parse가 실패하면(저장소가 아님) 그냥 폴더다.
    test.process.when({ "rev-parse" }, test::scripted_response { 128, "" });
    test.send(folder_chosen_intent { folder_purpose::locate, u8"C:\\plain\\folder", old_path });
    settle_inspection(test);

    REQUIRE(test.controller.settings().recent_repositories.empty() == false);
    CHECK(test.controller.settings().recent_repositories.front().path == old_path);
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->warning);
    CHECK(test.controller.state().notice->text.find(u8"Git 저장소가 아니라서") != std::u8string::npos);
}
