// 인터랙티브 rebase 편집기의 화면 규칙과 controller다. 상태를 직접 만들거나, 가짜 실행기를 꽂은 workspace로 범위 읽기와
// 실행 요청의 흐름을 본다. git과 디스크가 필요 없다.
#include "application/git_workspace.h"
#include "presentation/rebase/rebase_editor_controller.h"
#include "presentation/rebase/rebase_editor_model.h"
#include "presentation/rebase/rebase_editor_view.h"

#include "fake_git_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using namespace std::string_view_literals;

    std::string sha_of(const char letter)
    {
        return std::string(40, letter);
    }

    rebase_commit commit_of(const char letter, std::string subject, std::string body = {}, const bool pushed = false)
    {
        rebase_commit commit {};
        commit.sha = sha_of(letter);
        commit.subject = std::move(subject);
        commit.body = std::move(body);
        commit.pushed = pushed;
        return commit;
    }

    rebase_plan plan_of(std::vector<rebase_commit> commits)
    {
        rebase_plan plan {};
        plan.onto = "main";
        plan.branch = "feature";
        plan.onto_sha = sha_of('0');
        plan.commits = std::move(commits);
        return plan;
    }

    rebase_editor_state editing(std::vector<rebase_commit> commits)
    {
        rebase_editor_state state {};
        state.onto = "main";
        state.branch = "feature";
        state.onto_label = u8"main";
        state.branch_label = u8"feature";
        start_rebase_editing(state, plan_of(std::move(commits)));
        return state;
    }

    std::vector<std::string> order(const rebase_editor_state& state)
    {
        std::vector<std::string> shas {};
        for (const rebase_todo_entry& entry : state.todo)
            shas.push_back(entry.sha);
        return shas;
    }

    std::u8string u8(const std::string_view text)
    {
        return std::u8string { text.begin(), text.end() };
    }

    void set_text(luil::text::text_edit_state& state, const std::u8string_view text)
    {
        state.text = std::u8string { text };
        state.caret = state.text.size();
        state.anchor = state.caret;
    }

    // 범위 읽기와 실행을 흉내 내는 실행기다. 실행 요청은 기록하고 정해 둔 결과로 끝낸다.
    class scripted_rebase_executor final : public git_operation_executor
    {
    public:
        void execute(const git_operation_job& job, const operation_event_sink& emit) noexcept override
        {
            if (std::holds_alternative<load_rebase_plan_request>(job.operation))
                emit(operation_event { job.id, job.repository_id, rebase_plan_loaded { plan } });
            operation_finished finished { .state = operation_state::succeeded };
            if (const auto* const request { std::get_if<interactive_rebase_request>(&job.operation) })
            {
                requests.push_back(*request);
                finished = outcome;
            }
            emit(operation_event { job.id, job.repository_id, finished });
        }

        rebase_plan plan {};
        operation_finished outcome { .state = operation_state::succeeded };
        std::vector<interactive_rebase_request> requests {};
    };

    struct controller_harness
    {
        controller_harness()
        {
            git_workspace_options options {};
            options.refresh_on_open = false;
            workspace = std::make_unique<git_workspace>(executor, dispatcher.dispatcher(), events.sink(), nullptr, options);
            repository = workspace->open_repository(u8"C:/repo");
            controller = std::make_unique<rebase_editor_controller>(rebase_editor_controller::dependencies { workspace.get() });
        }

        // 낸 job을 모두 실행하고 사건을 반영한 뒤 controller의 후속을 부른다. 자동 refresh도 같이 돈다.
        void settle()
        {
            for (int round { 0 }; round < 16; ++round)
            {
                while (dispatcher.run())
                {
                }
                const std::vector<operation_event> taken { events.take() };
                if (taken.empty() && dispatcher.pending() == 0)
                    break;
                for (const operation_event& event : taken)
                    workspace->apply(event);
            }
            controller->after_events();
        }

        void send(const luil::app_message& message)
        {
            REQUIRE(controller->handle(message));
        }

        scripted_rebase_executor executor {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        std::unique_ptr<git_workspace> workspace {};
        std::string repository {};
        std::unique_ptr<rebase_editor_controller> controller {};
    };
} // namespace

TEST_CASE("rebase editor: editing starts with fixup! commits arranged and the first row selected", "[presentation][rebase]")
{
    const rebase_editor_state state { editing({ commit_of('a', "add parser"), commit_of('b', "add view"), commit_of('c', "fixup! add parser") }) };
    CHECK(state.phase == rebase_editor_phase::editing);
    CHECK(state.autosquash);
    CHECK(order(state) == std::vector<std::string> { sha_of('a'), sha_of('c'), sha_of('b') });
    CHECK(state.todo[1].action == rebase_action::fixup);
    CHECK(state.selected == sha_of('a'));

    rebase_editor_state plain { state };
    set_rebase_autosquash(plain, false);
    CHECK(order(plain) == std::vector<std::string> { sha_of('a'), sha_of('b'), sha_of('c') });
    CHECK(std::ranges::all_of(plain.todo, [](const rebase_todo_entry& entry) { return entry.action == rebase_action::pick; }));

    // 표지 커밋이 없으면 자동 정렬 칸이 서지 않는다.
    const rebase_editor_state none { editing({ commit_of('a', "one"), commit_of('b', "two") }) };
    CHECK_FALSE(none.autosquash);
    CHECK_FALSE(build_rebase_editor_view(none, std::nullopt).show_autosquash);
}

TEST_CASE("rebase editor: actions, moves and drops keep git's rules", "[presentation][rebase]")
{
    rebase_editor_state state { editing({ commit_of('a', "one"), commit_of('b', "two"), commit_of('c', "three") }) };
    // 첫 줄은 합칠 앞 커밋이 없다.
    CHECK_FALSE(set_rebase_action(state, sha_of('a'), rebase_action::squash));
    CHECK(set_rebase_action(state, sha_of('b'), rebase_action::squash));
    CHECK(state.todo[1].action == rebase_action::squash);

    // Alt+↑↓는 한 칸씩, 끝을 넘지 않는다.
    CHECK(move_rebase_row(state, sha_of('c'), -1));
    CHECK(order(state) == std::vector<std::string> { sha_of('a'), sha_of('c'), sha_of('b') });
    CHECK_FALSE(move_rebase_row(state, sha_of('a'), -1));
    CHECK_FALSE(move_rebase_row(state, sha_of('b'), 1));

    // 끌어 놓기: 아래로 옮기면 대상 뒤, 위로 옮기면 대상 앞이다.
    CHECK(reorder_rebase_rows(state, sha_of('a'), sha_of('b')));
    CHECK(order(state) == std::vector<std::string> { sha_of('c'), sha_of('b'), sha_of('a') });
    CHECK(reorder_rebase_rows(state, sha_of('a'), sha_of('c')));
    CHECK(order(state) == std::vector<std::string> { sha_of('a'), sha_of('c'), sha_of('b') });

    // 앞이 모두 drop이면 squash할 수 없다.
    rebase_editor_state dropped { editing({ commit_of('a', "one"), commit_of('b', "two") }) };
    CHECK(set_rebase_action(dropped, sha_of('a'), rebase_action::drop));
    CHECK_FALSE(set_rebase_action(dropped, sha_of('b'), rebase_action::fixup));

    // 옮겨서 squash가 맨 위로 오면 시작할 수 없고 이유가 보인다.
    rebase_editor_state moved { editing({ commit_of('a', "one"), commit_of('b', "two") }) };
    CHECK(set_rebase_action(moved, sha_of('b'), rebase_action::squash));
    CHECK(move_rebase_row(moved, sha_of('b'), -1));
    const rebase_editor_view view { build_rebase_editor_view(moved, std::nullopt) };
    CHECK_FALSE(view.can_start);
    CHECK(view.problem.find(u8"squash·fixup할 수 없습니다") != std::u8string::npos);
}

TEST_CASE("rebase editor: reword and squash drafts become the messages of the final todo", "[presentation][rebase][messages]")
{
    rebase_editor_state state { editing({ commit_of('a', "one", "body a"), commit_of('b', "two"), commit_of('c', "three"), commit_of('d', "four") }) };
    REQUIRE(set_rebase_action(state, sha_of('a'), rebase_action::reword));
    REQUIRE(set_rebase_action(state, sha_of('c'), rebase_action::squash));
    REQUIRE(set_rebase_action(state, sha_of('d'), rebase_action::fixup));

    // reword 초안은 원래 메시지로 시작한다.
    CHECK(rebase_draft_key(state, sha_of('a')) == std::string { "r:" + sha_of('a') });
    const rebase_message_draft& reword { state.drafts.at("r:" + sha_of('a')) };
    CHECK(reword.subject.text == u8"one");
    CHECK(reword.body.text == u8"body a");
    // squash 묶음(b, c, d)의 초안은 묶음 첫 줄의 열쇠이고 git처럼 pick·squash 메시지를 잇는다 (fixup은 빠진다).
    CHECK(rebase_draft_key(state, sha_of('d')) == std::string { "g:" + sha_of('b') });
    CHECK(rebase_draft_message(state.drafts.at("g:" + sha_of('b'))) == "two\n\nthree");

    set_text(state.drafts.at("r:" + sha_of('a')).subject, u8"첫 커밋 고침");
    set_text(state.drafts.at("r:" + sha_of('a')).body, u8"본문 1\n본문 2");
    rebase_message_draft& group { state.drafts.at("g:" + sha_of('b')) };
    set_text(group.subject, u8"둘과 셋");
    set_text(group.body, u8"");
    group.edited = true;

    const std::vector<rebase_todo_entry> todo { rebase_final_todo(state) };
    REQUIRE(todo.size() == 4);
    CHECK(todo[0].message == std::optional<std::string> { "첫 커밋 고침\n\n본문 1\n본문 2" });
    CHECK_FALSE(todo[1].message.has_value());
    CHECK_FALSE(todo[2].message.has_value());
    // 묶음의 합친 메시지는 묶음 마지막 줄(git이 편집기를 여는 줄)에 실린다.
    CHECK(todo[3].message == std::optional<std::string> { "둘과 셋" });
    CHECK(find_rebase_todo_problem(todo, std::vector<std::string> { sha_of('a'), sha_of('b'), sha_of('c'), sha_of('d') }) == std::nullopt);

    // 고친 묶음 초안은 묶음이 바뀌어도 남고, 고치지 않은 초안은 다시 만들어진다.
    REQUIRE(set_rebase_action(state, sha_of('d'), rebase_action::squash));
    CHECK(rebase_draft_message(state.drafts.at("g:" + sha_of('b'))) == "둘과 셋");
    reset_rebase_draft(state, "g:" + sha_of('b'));
    CHECK(rebase_draft_message(state.drafts.at("g:" + sha_of('b'))) == "two\n\nthree\n\nfour");
}

TEST_CASE("rebase editor: the view lists rows, warnings and the summary", "[presentation][rebase][view]")
{
    rebase_editor_state state { editing({ commit_of('a', "one", {}, true), commit_of('b', "two"), commit_of('c', "three") }) };
    REQUIRE(set_rebase_action(state, sha_of('b'), rebase_action::fixup));
    REQUIRE(set_rebase_action(state, sha_of('c'), rebase_action::drop));
    rebase_editor_view view { build_rebase_editor_view(state, std::nullopt) };
    REQUIRE(view.rows.size() == 3);
    // 줄 이름은 숫자로 시작한다 (목록 글자 탐색이 단축키를 가져가지 않는다).
    CHECK(view.rows[0].label == u8"1  pick  aaaaaaa  one  · 원격에 있음");
    CHECK(view.rows[1].label.starts_with(u8"2  fixup  bbbbbbb"));
    CHECK(view.rows[1].melded);
    CHECK(view.summary == u8"커밋 3개 → 1개");
    REQUIRE(view.preview.size() == 1);
    CHECK(view.preview[0].subject == u8"one");
    CHECK(view.preview[0].detail == u8"커밋 2개 합침");
    CHECK(view.warnings.size() == 1);
    CHECK(view.warnings[0].find(u8"강제 push") != std::u8string::npos);
    CHECK(view.can_start);
    CHECK(view.title == u8"feature를 main 위로 인터랙티브 rebase");

    // 병합 커밋이 있으면 확인해야 시작한다.
    state.plan.merge_count = 2;
    view = build_rebase_editor_view(state, std::nullopt);
    CHECK(view.needs_flatten_confirm);
    CHECK_FALSE(view.can_start);
    state.flatten_confirmed = true;
    CHECK(build_rebase_editor_view(state, std::nullopt).can_start);

    // 작업 트리가 더러우면 autostash를 켜야 한다.
    state.worktree_dirty = true;
    state.autostash = false;
    view = build_rebase_editor_view(state, std::nullopt);
    CHECK(view.show_autostash);
    CHECK_FALSE(view.can_start);
    state.autostash = true;
    CHECK(build_rebase_editor_view(state, std::nullopt).can_start);

    // 고른 줄의 메시지 칸: pick은 칸이 없고 reword는 있다.
    state.selected = sha_of('a');
    view = build_rebase_editor_view(state, std::nullopt);
    CHECK(view.has_selection);
    CHECK_FALSE(view.message_editable);
    CHECK_FALSE(view.can_meld);
    REQUIRE(set_rebase_action(state, sha_of('a'), rebase_action::reword));
    view = build_rebase_editor_view(state, std::nullopt);
    CHECK(view.message_editable);
    CHECK(view.subject.text == u8"one");
}

TEST_CASE("rebase editor: body caret moves by line and Home/End stay on the line", "[presentation][rebase][text]")
{
    luil::text::text_edit_state state {};
    set_text(state, u8"첫째 줄\nab\n셋째 줄 길다");
    // 끝(셋째 줄 끝)에서 위로: 둘째 줄은 짧아 그 줄 끝에 선다.
    move_text_caret_line(state, false, false);
    CHECK(state.caret == u8"첫째 줄\nab"sv.size());
    // 같은 열(코드 포인트 둘)이라 한글 두 글자 뒤다.
    move_text_caret_line(state, false, false);
    CHECK(state.caret == u8"첫째"sv.size());
    move_text_caret_line(state, false, false);
    CHECK(state.caret == 0u);
    move_text_caret_line(state, true, true);
    CHECK(state.anchor == 0u);
    CHECK(state.caret == u8"첫째 줄\n"sv.size());
    move_text_caret_line_edge(state, true, false);
    CHECK(state.caret == u8"첫째 줄\nab"sv.size());
    move_text_caret_line_edge(state, false, false);
    CHECK(state.caret == u8"첫째 줄\n"sv.size());
    move_text_caret_line(state, true, false);
    move_text_caret_line(state, true, false);
    CHECK(state.caret == state.text.size());
}

TEST_CASE("rebase editor controller: opening reads the plan, keys edit rows and start submits the todo", "[presentation][rebase][controller]")
{
    controller_harness test {};
    test.executor.plan = plan_of({ commit_of('a', "one"), commit_of('b', "two"), commit_of('c', "three") });
    test.send(luil::app_message { open_rebase_editor_intent { .repository_id = test.repository, .onto = "main", .branch = "feature", .onto_label = u8"main", .branch_label = u8"feature" } });
    REQUIRE(test.controller->is_open());
    CHECK(test.controller->state()->phase == rebase_editor_phase::loading);
    test.settle();
    REQUIRE(test.controller->state()->phase == rebase_editor_phase::editing);
    CHECK(test.controller->state()->todo.size() == 3);

    // 줄 키로 고른 뒤 단축키(빈 키 = 고른 줄)로 동작을 바꾸고, Alt+↓로 옮긴다.
    test.send(luil::app_message { rebase_select_intent { rebase_row_key(sha_of('b')) } });
    test.send(luil::app_message { rebase_set_action_intent { .row_key = {}, .action = rebase_action::reword } });
    test.send(luil::app_message { rebase_move_intent { .row_key = {}, .delta = 1 } });
    CHECK(order(*test.controller->state()) == std::vector<std::string> { sha_of('a'), sha_of('c'), sha_of('b') });
    // 메시지 칸의 편집이 초안에 들어가고 고친 것으로 표시된다. 본문은 줄바꿈을 지킨다.
    luil::text_edit_request subject {};
    subject.target = to_luil_target(text_target::rebase_message_subject);
    subject.command = luil::text::text_edit_command::replace_all;
    subject.text = u8"둘째를 고쳐 쓴다";
    subject.offset = subject.text.size();
    REQUIRE(test.controller->edit_text(subject));
    luil::text_edit_request body {};
    body.target = to_luil_target(text_target::rebase_message_body);
    body.command = luil::text::text_edit_command::insert;
    body.text = u8"한 줄\r\n두 줄";
    REQUIRE(test.controller->edit_text(body));
    CHECK(test.controller->state()->drafts.at("r:" + sha_of('b')).edited);
    CHECK(test.controller->state()->drafts.at("r:" + sha_of('b')).body.text == u8"한 줄\n두 줄");
    // 줄 끌어 놓기도 된다.
    test.send(luil::app_message { rebase_reorder_intent { rebase_row_key(sha_of('c')), rebase_row_key(sha_of('a')) } });
    CHECK(order(*test.controller->state()) == std::vector<std::string> { sha_of('c'), sha_of('a'), sha_of('b') });

    test.send(luil::app_message { rebase_start_intent {} });
    CHECK(test.controller->state()->phase == rebase_editor_phase::running);
    test.settle();
    REQUIRE(test.executor.requests.size() == 1);
    const interactive_rebase_request& request { test.executor.requests.front() };
    CHECK(request.onto == "main");
    CHECK(request.branch == "feature");
    REQUIRE(request.todo.size() == 3);
    CHECK(request.todo[2].action == rebase_action::reword);
    CHECK(request.todo[2].message == std::optional<std::string> { "둘째를 고쳐 쓴다\n\n한 줄\n두 줄" });
    // 성공하면 닫힌다.
    CHECK_FALSE(test.controller->is_open());
}

TEST_CASE("rebase editor controller: a refusal keeps the editor open, a conflict closes it", "[presentation][rebase][controller]")
{
    controller_harness test {};
    test.executor.plan = plan_of({ commit_of('a', "one"), commit_of('b', "two") });
    test.executor.outcome = operation_finished { .state = operation_state::failed, .failure = operation_failure { failure_kind::refused, u8"이미 진행 중인 rebase가 있습니다.", {} } };
    test.send(luil::app_message { open_rebase_editor_intent { .repository_id = test.repository, .onto = "main", .branch = "feature", .onto_label = u8"main", .branch_label = u8"feature" } });
    test.settle();
    test.send(luil::app_message { rebase_start_intent {} });
    test.settle();
    REQUIRE(test.controller->is_open());
    CHECK(test.controller->state()->phase == rebase_editor_phase::editing);
    CHECK(test.controller->state()->error == u8"이미 진행 중인 rebase가 있습니다.");
    CHECK(build_rebase_editor_view(*test.controller->state(), std::nullopt).problem == u8"이미 진행 중인 rebase가 있습니다.");

    input_request conflict {};
    conflict.kind = input_kind::conflict;
    conflict.sequence = repository_operation_state::rebase;
    test.executor.outcome = operation_finished { .state = operation_state::needs_input, .needs_input = conflict };
    test.send(luil::app_message { rebase_start_intent {} });
    test.settle();
    CHECK_FALSE(test.controller->is_open());

    // 닫힌 뒤 늦게 온 메시지는 삼킨다.
    CHECK(test.controller->handle(luil::app_message { rebase_start_intent {} }));
}

TEST_CASE("rebase editor controller: a failed plan read shows the reason", "[presentation][rebase][controller]")
{
    controller_harness test {};
    // 저장소가 열려 있지 않으면 범위를 읽지 못한다.
    test.send(luil::app_message { open_rebase_editor_intent { .repository_id = "missing", .onto = "main" } });
    REQUIRE(test.controller->is_open());
    CHECK(test.controller->state()->phase == rebase_editor_phase::load_failed);
    CHECK(build_rebase_editor_view(*test.controller->state(), std::nullopt).status_text.empty() == false);
    test.send(luil::app_message { close_rebase_editor_intent {} });
    CHECK_FALSE(test.controller->is_open());
}
