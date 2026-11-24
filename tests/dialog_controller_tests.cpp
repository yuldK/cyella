// 대화 상자·메뉴·끌어 놓기·충돌 해결의 화면 상태 흐름이다. workspace는 가짜 runner와 수동 dispatcher로 세우고, job은
// 실행하지 않고 낸 operation 요청만 읽는다. operation 사건은 test가 직접 넣는다.
#include "application/app_messages.h"
#include "domain/external_diff_tool.h"
#include "application/git_workspace.h"
#include "domain/credential_prompt.h"
#include "infrastructure/git_operation_runner.h"
#include "native_path_support.h"
#include "presentation/commands/command_intents.h"
#include "presentation/graph_geometry.h"
#include "presentation/palette/palette_intents.h"
#include "presentation/shell_controller.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/cyella_policy.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/settings/appearance_ui.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include "luil/ui/ui_interaction.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using cyella::test::native_path;

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
            : runner { process, native_path(u8"C:/Git/cmd/git.exe"), files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(workspace, std::move(hook), opened_folders, steady_now) }
        {
            controller.start({});
            controller.open_repository(native_path(u8"C:\\work\\repo"));
            repository = controller.selected_repository_id();
        }

        static shell_controller::dependencies make_dependencies(
            git_workspace& workspace, std::function<void(const luil::app_message&)> hook, std::vector<std::pair<external_file_app, std::u8string>>& opened,
            std::optional<std::chrono::steady_clock::time_point>& steady_now)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.send_backend_intent = std::move(hook);
            dependencies.open_folder_app = [&opened](const external_file_app& app, const std::u8string& folder) { opened.emplace_back(app, folder); };
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } }; };
            // 호버 시계를 정하는 test만 `steady_now`를 채운다. 비어 있으면 실제 시각이다 (토스트 test는 실제 시계를 쓴다).
            dependencies.steady_clock = [&steady_now] { return steady_now.value_or(std::chrono::steady_clock::now()); };
            dependencies.temporary_directory = native_path(u8"C:\\temp\\cyella-test");
            dependencies.ssh_executable = native_path(u8"C:/Git/usr/bin/ssh.exe");
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
        // 저장소 폴더를 연 앱과 폴더다 (driver가 꽂는 worker 길의 자리).
        std::vector<std::pair<external_file_app, std::u8string>> opened_folders {};
        // controller의 steady 시계다. 값이 있으면 그 시각을 준다.
        std::optional<std::chrono::steady_clock::time_point> steady_now {};
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

TEST_CASE("overflow ref lists open for a loaded commit and hand actions to its branch menu", "[presentation][dialogs][refs]")
{
    harness test {};
    test.load();
    const std::string sha { snap::sha_of(4) };
    test.send(open_ref_list_intent { sha, 120.0f, 140.0f });
    REQUIRE(test.controller.state().ref_list.has_value());
    auto view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    REQUIRE(view.ref_list.has_value());
    CHECK(view.ref_list->sha == sha);
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "main", .sha = sha }, 120.0f, 140.0f });
    CHECK_FALSE(test.controller.state().ref_list.has_value());
    REQUIRE(test.controller.state().menu.has_value());
    CHECK(test.controller.state().menu->target.name == "main");
    test.send(close_menu_intent {});
    test.send(open_ref_list_intent { "missing", 120.0f, 140.0f });
    CHECK_FALSE(test.controller.state().ref_list.has_value());

    // 카드는 칩 자리에 서 있으므로 그래프가 흐르면 닫힌다. 다른 판의 스크롤은 그대로 둔다.
    test.send(open_ref_list_intent { .sha = sha, .x = 120.0f, .y = 140.0f, .width = 24.0f, .height = 20.0f });
    REQUIRE(test.controller.state().ref_list.has_value());
    CHECK(test.controller.state().ref_list->width == 24.0f);
    test.send(scroll_intent { .target = scroll_target::sidebar, .value = 30.0f });
    CHECK(test.controller.state().ref_list.has_value());
    test.send(scroll_intent { .target = scroll_target::graph, .value = 30.0f });
    CHECK_FALSE(test.controller.state().ref_list.has_value());

    // 행이나 칩을 눌러 커밋을 고르면 닫힌다. 포인터가 "+N" 칸 안이면 카드 popup의 바깥 누름을 controller가 버리므로 이 고르기가 닫는다.
    test.send(open_ref_list_intent { .sha = sha, .x = 120.0f, .y = 140.0f, .width = 24.0f, .height = 20.0f });
    REQUIRE(test.controller.state().ref_list.has_value());
    test.send(select_commit_intent { .key = graph_item_key(sha) });
    CHECK_FALSE(test.controller.state().ref_list.has_value());
}

TEST_CASE("an open ref list closes when the window or the graph moves its chip", "[presentation][dialogs][refs]")
{
    harness test {};
    test.load();
    const std::string sha { snap::sha_of(4) };

    // 연 때의 칩 사각형은 창 폭이 바뀌면 다른 자리다. 그대로 그리면 카드가 엉뚱한 곳을 가리킨다.
    test.send(open_ref_list_intent { .sha = sha, .x = 120.0f, .y = 140.0f, .width = 24.0f, .height = 20.0f });
    REQUIRE(test.view().ref_list.has_value());
    REQUIRE(test.view().ref_list.has_value());
    CHECK_FALSE(test.controller.make_view(1200.0f, 900.0f, 1.0f).ref_list.has_value());
    CHECK_FALSE(test.controller.state().ref_list.has_value());

    // 그래프를 다시 읽어도(새 generation) 행과 칩이 다시 놓이므로 닫는다.
    test.send(open_ref_list_intent { .sha = sha, .x = 120.0f, .y = 140.0f, .width = 24.0f, .height = 20.0f });
    REQUIRE(test.view().ref_list.has_value());
    test.load();
    CHECK_FALSE(test.view().ref_list.has_value());
}

namespace {
    // 참조 목록 카드의 호버 test다. 칸의 hover 영역·머무름과 카드 popup의 영역이 낼 메시지를 controller에 그대로 넣고, 시각은 손으로 정한다.
    struct ref_hover
    {
        ref_hover()
        {
            test.load();
            test.steady_now = start;
        }

        void at(const int milliseconds)
        {
            test.steady_now = start + std::chrono::milliseconds { milliseconds };
        }

        // 영역 들어옴과 머무름이다. 덮개가 없고 누르지 않은 칸이면 luil이 둘 다 낸다.
        void enter(const std::string& sha, const bool dwell = true)
        {
            test.send(ref_cell_hover_intent { .row_key = graph_item_key(sha), .sha = sha, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f, .inside = true });
            if (dwell)
                test.send(ref_cell_dwell_intent { .row_key = graph_item_key(sha), .live = true });
        }

        void leave(const std::string& sha)
        {
            test.send(ref_cell_dwell_intent { .row_key = graph_item_key(sha), .live = false });
            test.send(ref_cell_hover_intent { .row_key = graph_item_key(sha), .sha = sha, .inside = false });
        }

        void tick()
        {
            REQUIRE(test.steady_now.has_value());
            test.controller.tick(*test.steady_now);
        }

        [[nodiscard]] bool open() const
        {
            return test.controller.state().ref_list.has_value();
        }

        [[nodiscard]] bool open_for(const std::string& sha) const
        {
            return open() && test.controller.state().ref_list->sha == sha;
        }

        [[nodiscard]] ref_list_origin origin() const
        {
            REQUIRE(open());
            return test.controller.state().ref_list->origin;
        }

        harness test {};
        std::chrono::steady_clock::time_point start { std::chrono::steady_clock::time_point {} + std::chrono::hours { 1 } };
        std::string oldest { snap::sha_of(4) };
        std::string newest { snap::sha_of(0) };
    };
} // namespace

TEST_CASE("hovering a refs cell opens the card after 180ms and closes it 400ms after leaving", "[presentation][dialogs][refs]")
{
    ref_hover hover {};
    hover.enter(hover.oldest);
    CHECK_FALSE(hover.open());
    // 열 시각을 tick에 알린다. 그보다 이르면 열지 않는다.
    REQUIRE(hover.test.controller.next_tick().has_value());
    CHECK(*hover.test.controller.next_tick() == hover.start + std::chrono::milliseconds { 180 });
    hover.at(179);
    hover.tick();
    CHECK_FALSE(hover.open());
    hover.at(180);
    hover.tick();
    REQUIRE(hover.open_for(hover.oldest));
    CHECK(hover.origin() == ref_list_origin::hover);
    // 카드는 호버로 연 칸의 "+N" 칩을 가리킨다. 항목을 고르면 메뉴도 그 칩 아래에 선다.
    CHECK(hover.test.controller.state().ref_list->x == 100.0f);
    CHECK(hover.test.controller.state().ref_list->height == 18.0f);
    CHECK(hover.test.controller.state().ref_list->row_key == graph_item_key(hover.oldest));

    // 칸과 카드를 떠나면 400ms 뒤에 닫는다.
    hover.at(300);
    hover.leave(hover.oldest);
    REQUIRE(hover.test.controller.next_tick().has_value());
    CHECK(*hover.test.controller.next_tick() == hover.start + std::chrono::milliseconds { 700 });
    hover.at(699);
    hover.tick();
    CHECK(hover.open());
    hover.at(700);
    hover.tick();
    CHECK_FALSE(hover.open());
    // 떠난 뒤 닫혔으므로 돌아오면 다시 연다.
    hover.at(1000);
    hover.enter(hover.oldest);
    hover.at(1180);
    hover.tick();
    CHECK(hover.open_for(hover.oldest));

    // 머무름 없이 영역만 선 칸(덮개 아래, 끄는 도중)은 열지 않는다.
    ref_hover covered {};
    covered.enter(covered.oldest, false);
    covered.at(500);
    covered.tick();
    CHECK_FALSE(covered.open());
    CHECK_FALSE(covered.test.controller.next_tick().has_value());
    // 덮개가 걷혀 머무름이 서면 그때부터 180ms다.
    covered.test.send(ref_cell_dwell_intent { .row_key = graph_item_key(covered.oldest), .live = true });
    covered.at(679);
    covered.tick();
    CHECK_FALSE(covered.open());
    covered.at(680);
    covered.tick();
    CHECK(covered.open());
}

TEST_CASE("an open hover card switches rows at once and stays while the pointer is on the card", "[presentation][dialogs][refs]")
{
    ref_hover hover {};
    hover.enter(hover.oldest);
    hover.at(200);
    hover.tick();
    REQUIRE(hover.open_for(hover.oldest));
    const std::uint64_t first { hover.test.controller.state().ref_list->serial };

    // 카드 위에 있는 동안은 칸을 떠나도 닫지 않는다.
    hover.leave(hover.oldest);
    hover.test.send(ref_card_hover_intent { true });
    hover.at(2000);
    hover.tick();
    CHECK(hover.open_for(hover.oldest));
    CHECK_FALSE(hover.test.controller.next_tick().has_value());
    // 카드를 떠나면 그때부터 400ms다.
    hover.test.send(ref_card_hover_intent { false });
    hover.at(2399);
    hover.tick();
    CHECK(hover.open());
    hover.enter(hover.newest);
    // 호버로 연 카드가 떠 있으면 다른 행의 칸으로 옮기는 즉시 그 행으로 바뀐다. 새로 연 카드라 번호가 다르다.
    REQUIRE(hover.open_for(hover.newest));
    CHECK(hover.origin() == ref_list_origin::hover);
    CHECK(hover.test.controller.state().ref_list->serial != first);
}

TEST_CASE("a press-opened card survives leaving and the overflow chip pins a hover card", "[presentation][dialogs][refs]")
{
    ref_hover hover {};
    hover.enter(hover.oldest);
    hover.test.send(open_ref_list_intent { .sha = hover.oldest, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f, .row_key = graph_item_key(hover.oldest) });
    REQUIRE(hover.open_for(hover.oldest));
    CHECK(hover.origin() == ref_list_origin::press);
    // 누름으로 연 카드는 칸과 카드를 떠나도 남는다. 같은 칸에 머물러도 호버 카드로 바뀌지 않는다.
    hover.at(500);
    hover.tick();
    CHECK(hover.origin() == ref_list_origin::press);
    hover.leave(hover.oldest);
    hover.at(5000);
    hover.tick();
    CHECK(hover.open_for(hover.oldest));

    // 호버로 연 카드에서 "+N"을 누르면(같은 커밋의 열기) 고정되어 떠나도 남는다. 스크롤은 그대로다.
    ref_hover pinned {};
    pinned.enter(pinned.oldest);
    pinned.at(200);
    pinned.tick();
    REQUIRE(pinned.origin() == ref_list_origin::hover);
    pinned.test.send(scroll_ref_list_intent { 30.0f, true });
    // "+N"을 누르면 머무름은 끝나지만 포인터는 칸 자리 안이라 닫기 시계가 서지 않는다.
    pinned.test.send(ref_cell_dwell_intent { .row_key = graph_item_key(pinned.oldest), .live = false });
    CHECK_FALSE(pinned.test.controller.next_tick().has_value());
    pinned.test.send(open_ref_list_intent { .sha = pinned.oldest, .row_key = graph_item_key(pinned.oldest) });
    CHECK(pinned.origin() == ref_list_origin::press);
    CHECK(pinned.test.controller.state().ref_list->scroll == 30.0f);
    pinned.leave(pinned.oldest);
    pinned.at(5000);
    pinned.tick();
    CHECK(pinned.open());
    // 고정한 카드의 "+N"은 닫기를 낸다 (`ref_overflow_element`).
    pinned.test.send(close_ref_list_intent {});
    CHECK_FALSE(pinned.open());

    // "+N"이나 칩을 우클릭해도 머무름은 끝나고 칸을 떠날 때까지 다시 서지 않는다. 포인터는 칸 위라 호버 카드가 남는다.
    ref_hover clicked {};
    clicked.enter(clicked.oldest);
    clicked.at(200);
    clicked.tick();
    REQUIRE(clicked.origin() == ref_list_origin::hover);
    clicked.test.send(ref_cell_dwell_intent { .row_key = graph_item_key(clicked.oldest), .live = false });
    clicked.at(5000);
    clicked.tick();
    CHECK(clicked.open_for(clicked.oldest));
    CHECK_FALSE(clicked.test.controller.next_tick().has_value());
}

TEST_CASE("a card opened from the keyboard on another row is not replaced by the hovered cell", "[presentation][dialogs][refs]")
{
    // 마우스가 머무는 칸 B의 호버 카드가 떠 있을 때 키·보조 기술로 행 A의 "+N"을 실행했다. 이어지는 호버 판정이 A를 B로 갈아 끼우면
    // 사용자가 연 카드는 한 frame도 서지 않는다.
    ref_hover hover {};
    hover.enter(hover.newest);
    hover.at(200);
    hover.tick();
    REQUIRE(hover.open_for(hover.newest));
    hover.test.send(open_ref_list_intent { .sha = hover.oldest, .x = 100.0f, .y = 80.0f, .width = 24.0f, .height = 18.0f });
    REQUIRE(hover.open_for(hover.oldest));
    CHECK(hover.origin() == ref_list_origin::press);
    CHECK_FALSE(hover.test.controller.next_tick().has_value());
    hover.at(2000);
    hover.tick();
    CHECK(hover.open_for(hover.oldest));

    // B를 떠났다 돌아와 머물면 평소처럼 그 칸의 카드로 바뀐다 (누름 카드가 떠 있어도 다른 행에 180ms 머물면 바뀐다).
    hover.leave(hover.newest);
    hover.at(3000);
    hover.enter(hover.newest);
    hover.at(3179);
    hover.tick();
    CHECK(hover.open_for(hover.oldest));
    hover.at(3180);
    hover.tick();
    CHECK(hover.open_for(hover.newest));
}

TEST_CASE("a hover card closes while the pointer is over a cover above another refs cell", "[presentation][dialogs][refs]")
{
    // 레일 판·토스트가 덮은 칸은 영역만 들어옴을 알리고 머무름은 서지 않는다. 그 위의 포인터는 칸 밖이라 400ms 뒤 닫는다.
    ref_hover hover {};
    hover.enter(hover.oldest);
    hover.at(200);
    hover.tick();
    REQUIRE(hover.open_for(hover.oldest));
    hover.at(300);
    hover.leave(hover.oldest);
    hover.enter(hover.newest, false);
    REQUIRE(hover.test.controller.next_tick().has_value());
    CHECK(*hover.test.controller.next_tick() == hover.start + std::chrono::milliseconds { 700 });
    hover.at(699);
    hover.tick();
    CHECK(hover.open());
    hover.at(700);
    hover.tick();
    CHECK_FALSE(hover.open());
    // 덮개가 걷혀 그 칸에 머물면 평소처럼 연다.
    hover.test.send(ref_cell_dwell_intent { .row_key = graph_item_key(hover.newest), .live = true });
    hover.at(880);
    hover.tick();
    CHECK(hover.open_for(hover.newest));
}

TEST_CASE("a card closed with the pointer in its cell does not reopen until the cell is left", "[presentation][dialogs][refs]")
{
    const auto open_by_hover = [](ref_hover& hover) {
        hover.enter(hover.oldest);
        hover.at(200);
        hover.tick();
        REQUIRE(hover.open_for(hover.oldest));
    };
    const auto stays_closed = [](ref_hover& hover) {
        hover.at(5000);
        hover.tick();
        CHECK_FALSE(hover.open());
        // 칸을 떠났다 돌아오면 다시 연다.
        hover.leave(hover.oldest);
        hover.at(6000);
        hover.enter(hover.oldest);
        hover.at(6180);
        hover.tick();
        CHECK(hover.open_for(hover.oldest));
    };

    SECTION("Esc")
    {
        ref_hover hover {};
        open_by_hover(hover);
        hover.test.send(close_ref_list_intent {});
        stays_closed(hover);
    }
    SECTION("selecting the commit")
    {
        ref_hover hover {};
        open_by_hover(hover);
        hover.test.send(select_commit_intent { .key = graph_item_key(hover.oldest) });
        stays_closed(hover);
    }
    SECTION("a press outside the card while the pointer is only under a cover over a refs cell")
    {
        ref_hover hover {};
        open_by_hover(hover);
        // 칸을 떠났다가 덮개 아래로만 다시 들어온 칸이다(머무름이 선 적 없음). 그 자리의 누름은 덮개 위의 누름이라 닫는다.
        hover.leave(hover.oldest);
        hover.enter(hover.oldest, false);
        hover.test.send(close_ref_list_intent { .pressed_outside = true });
        CHECK_FALSE(hover.open());
        hover.test.send(ref_cell_dwell_intent { .row_key = graph_item_key(hover.oldest), .live = true });
        stays_closed(hover);
    }
    SECTION("graph scrolling brings another cell under the pointer")
    {
        ref_hover hover {};
        open_by_hover(hover);
        hover.test.send(scroll_intent { .target = scroll_target::graph, .value = 30.0f });
        CHECK_FALSE(hover.open());
        // 새 tree에서 영역을 다시 재면 옛 칸의 나감과 새 칸의 들어옴이 온다. 새 칸도 떠날 때까지 열지 않는다.
        hover.at(220);
        hover.leave(hover.oldest);
        hover.enter(hover.newest);
        hover.at(2000);
        hover.tick();
        CHECK_FALSE(hover.open());
        hover.leave(hover.newest);
        hover.at(3000);
        hover.enter(hover.newest);
        hover.at(3180);
        hover.tick();
        CHECK(hover.open_for(hover.newest));
    }
    SECTION("closing while the pointer is on the card keeps the revealed cell closed")
    {
        ref_hover hover {};
        open_by_hover(hover);
        hover.leave(hover.oldest);
        hover.test.send(ref_card_hover_intent { true });
        hover.test.send(close_ref_list_intent {});
        CHECK_FALSE(hover.open());
        // 카드가 사라진 자리에 드러난 칸이다. popup 영역의 나감이 오지 않아도 카드 위 상태는 내려놓는다.
        hover.at(250);
        hover.enter(hover.newest);
        hover.at(2000);
        hover.tick();
        CHECK_FALSE(hover.open());
        // 늦게 온 카드 들어옴은 닫힌 카드의 것이라 버린다.
        hover.test.send(ref_card_hover_intent { true });
        hover.leave(hover.newest);
        hover.at(3000);
        hover.enter(hover.newest);
        hover.at(3180);
        hover.tick();
        REQUIRE(hover.open_for(hover.newest));
        hover.leave(hover.newest);
        hover.at(3580);
        hover.tick();
        CHECK_FALSE(hover.open());
    }
    SECTION("a later cell is not caught by the closed one")
    {
        ref_hover hover {};
        open_by_hover(hover);
        hover.test.send(ref_card_hover_intent { true });
        hover.test.send(close_ref_list_intent {});
        // 닫힌 뒤 시간이 지나 옮겨 간 칸은 평소처럼 연다.
        hover.at(900);
        hover.enter(hover.newest);
        hover.at(1080);
        hover.tick();
        CHECK(hover.open_for(hover.newest));
    }
    SECTION("a delayed leave of another cell keeps the closed cell")
    {
        ref_hover hover {};
        hover.enter(hover.newest);
        hover.enter(hover.oldest);
        hover.at(200);
        hover.tick();
        REQUIRE(hover.open_for(hover.oldest));
        hover.test.send(close_ref_list_intent {});
        // 잡은 동안 미룬 앞 칸의 나감이 이제 온다. 지금 칸의 "다시 열지 않음"은 남는다.
        hover.test.send(ref_cell_hover_intent { .row_key = graph_item_key(hover.newest), .sha = hover.newest, .inside = false });
        stays_closed(hover);
    }
}

TEST_CASE("a press outside the card is left to the refs cell the pointer is on", "[presentation][dialogs][refs]")
{
    // 포인터가 "+N" 칸 안일 때의 바깥 누름은 그 누름이 스스로 처리한다 (칩·빈 칸 = 고르기·메뉴, "+N" = 열기·고정·닫기).
    ref_hover hover {};
    hover.enter(hover.newest);
    hover.test.send(open_ref_list_intent { .sha = hover.oldest, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f });
    REQUIRE(hover.open_for(hover.oldest));
    hover.test.send(close_ref_list_intent { .pressed_outside = true });
    CHECK(hover.open_for(hover.oldest));
    // 다른 행의 "+N"이면 그 행으로 바뀐다 (지워졌다 다시 서지 않는다).
    const std::uint64_t serial { hover.test.controller.state().ref_list->serial };
    hover.test.send(open_ref_list_intent { .sha = hover.newest, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f });
    REQUIRE(hover.open_for(hover.newest));
    CHECK(hover.test.controller.state().ref_list->serial == serial + 1);
    // 칩이나 빈 칸이면 고르기가 닫는다.
    hover.test.send(close_ref_list_intent { .pressed_outside = true });
    hover.test.send(select_commit_intent { .key = graph_item_key(hover.newest) });
    CHECK_FALSE(hover.open());
    // 칸 밖이면 바로 닫는다.
    hover.leave(hover.newest);
    hover.test.send(open_ref_list_intent { .sha = hover.oldest });
    REQUIRE(hover.open());
    hover.test.send(close_ref_list_intent { .pressed_outside = true });
    CHECK_FALSE(hover.open());
}

TEST_CASE("a press on the overflow chip after a right click in its cell pins the hover card in place", "[presentation][dialogs][refs]")
{
    // 칸 안의 우클릭("+N"·HEAD·stash 칩이면 메시지가 없다)도 머무름을 끝내고, 머무름은 칸을 떠날 때까지 다시 서지 않는다. 이어서 "+N"을
    // 누르면 그 누름의 바깥 누름이 카드를 지우고 뗄 때의 열기가 새 카드를 세워 깜빡이고 스크롤이 처음으로 돌아갔다.
    ref_hover hover {};
    hover.enter(hover.oldest);
    hover.at(200);
    hover.tick();
    REQUIRE(hover.origin() == ref_list_origin::hover);
    hover.test.send(scroll_ref_list_intent { 30.0f, true });
    const std::uint64_t serial { hover.test.controller.state().ref_list->serial };
    // 우클릭이다. 누를 때는 머무름이 살아 있어 바깥 누름을 버린다.
    hover.test.send(close_ref_list_intent { .pressed_outside = true });
    hover.test.send(ref_cell_dwell_intent { .row_key = graph_item_key(hover.oldest), .live = false });
    REQUIRE(hover.open_for(hover.oldest));
    // 칸을 떠나지 않고 "+N"을 누른다. 머무름은 비었지만 포인터는 머문 칸 안이라 바깥 누름을 버린다.
    hover.at(400);
    hover.test.send(close_ref_list_intent { .pressed_outside = true });
    REQUIRE(hover.open_for(hover.oldest));
    CHECK(hover.test.controller.state().ref_list->serial == serial);
    // 뗄 때 누를 때의 tree가 낸 같은 커밋의 열기다. 다시 열지 않고 고정하므로 번호와 스크롤이 그대로다.
    hover.test.send(open_ref_list_intent { .sha = hover.oldest, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f, .row_key = graph_item_key(hover.oldest) });
    REQUIRE(hover.open_for(hover.oldest));
    CHECK(hover.origin() == ref_list_origin::press);
    CHECK(hover.test.controller.state().ref_list->serial == serial);
    CHECK(hover.test.controller.state().ref_list->scroll == 30.0f);
    // 고정한 카드에서 머무름이 끝난 뒤의 "+N" 누름은 바깥 누름을 버리고, 뗄 때의 닫기가 닫는다.
    hover.test.send(close_ref_list_intent { .pressed_outside = true });
    REQUIRE(hover.open_for(hover.oldest));
    hover.test.send(close_ref_list_intent {});
    CHECK_FALSE(hover.open());
}

TEST_CASE("a press outside the card closes it once the pointer left the cell or only passed under a cover", "[presentation][dialogs][refs]")
{
    // 우클릭으로 머무름이 끝난 칸을 떠나면 그 칸의 "칸 안"도 끝난다.
    ref_hover left {};
    left.enter(left.oldest);
    left.at(200);
    left.tick();
    REQUIRE(left.open_for(left.oldest));
    left.test.send(ref_cell_dwell_intent { .row_key = graph_item_key(left.oldest), .live = false });
    left.test.send(ref_cell_hover_intent { .row_key = graph_item_key(left.oldest), .sha = left.oldest, .inside = false });
    left.test.send(close_ref_list_intent { .pressed_outside = true });
    CHECK_FALSE(left.open());

    // 누름으로 연 카드가 떠 있는데 포인터가 덮개(레일 판·토스트) 아래로만 다른 칸에 들어왔다. 머무름이 선 적 없는 칸은 칸 안이 아니다.
    ref_hover covered {};
    covered.test.send(open_ref_list_intent { .sha = covered.oldest, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f, .row_key = graph_item_key(covered.oldest) });
    REQUIRE(covered.open_for(covered.oldest));
    covered.enter(covered.newest, false);
    covered.test.send(close_ref_list_intent { .pressed_outside = true });
    CHECK_FALSE(covered.open());
}

TEST_CASE("hover does not open while a menu, selector, notification center or dialog is up", "[presentation][dialogs][refs]")
{
    const auto check_blocked = [](ref_hover& hover) {
        hover.enter(hover.oldest);
        hover.at(500);
        hover.tick();
        CHECK_FALSE(hover.open());
    };
    SECTION("menu")
    {
        ref_hover hover {};
        hover.test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = hover.newest }, 10.0f, 10.0f });
        REQUIRE(hover.test.controller.state().menu.has_value());
        check_blocked(hover);
        // 메뉴가 닫히면 이미 머문 칸이라 곧 연다.
        hover.test.send(close_menu_intent {});
        CHECK(hover.open_for(hover.oldest));
    }
    SECTION("selector")
    {
        // 열면 선택 상자를 닫으므로(`open_ref_list`) 막지 않으면 열어 둔 목록이 머무름 하나에 사라진다.
        ref_hover hover {};
        hover.test.send(open_selector_intent { selector_kind::repository, 200.0f, 60.0f, 220.0f });
        REQUIRE(hover.test.controller.state().selector.has_value());
        check_blocked(hover);
        CHECK(hover.test.controller.state().selector.has_value());
        CHECK_FALSE(hover.test.controller.next_tick().has_value());
    }
    SECTION("notification center")
    {
        ref_hover hover {};
        hover.test.send(toggle_notification_center_intent {});
        REQUIRE(hover.test.controller.state().notifications_open);
        check_blocked(hover);
    }
    SECTION("dialog")
    {
        ref_hover hover {};
        hover.test.send(run_command_intent { "git.branch.new" });
        REQUIRE(hover.test.view().form.has_value());
        check_blocked(hover);
    }
}

TEST_CASE("a refs cell whose card cannot open is not retried until it is left", "[presentation][dialogs][refs]")
{
    // 그래프에 없는 커밋의 칸이다. 지난 열 시각이 `next_tick`에 남으면 logic이 쉬지 않고 tick을 돈다.
    ref_hover hover {};
    hover.test.send(ref_cell_hover_intent { .row_key = graph_item_key("missing"), .sha = "missing", .inside = true });
    hover.test.send(ref_cell_dwell_intent { .row_key = graph_item_key("missing"), .live = true });
    hover.at(200);
    hover.tick();
    CHECK_FALSE(hover.open());
    CHECK_FALSE(hover.test.controller.next_tick().has_value());
}

TEST_CASE("the ref card takes the placement the host reports for it", "[presentation][dialogs][refs]")
{
    ref_hover hover {};
    hover.test.send(open_ref_list_intent { .sha = hover.oldest, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f });
    REQUIRE(hover.open());
    const std::uint64_t serial { hover.test.controller.state().ref_list->serial };
    // 반올림 차이는 보정이 아니다.
    hover.test.send(ref_list_placed_intent { .serial = serial, .requested_x = 40.0f, .requested_y = 60.0f, .x = 40.5f, .y = 60.4f });
    CHECK(hover.test.controller.state().ref_list->placed_shift == 0.0f);
    CHECK_FALSE(hover.test.controller.state().ref_list->flipped);
    // 가로로 민 거리는 꼬리가 거꾸로 옮길 거리다.
    hover.test.send(ref_list_placed_intent { .serial = serial, .requested_x = 40.0f, .requested_y = 60.0f, .x = 52.0f, .y = 60.0f });
    CHECK(hover.test.controller.state().ref_list->placed_shift == 12.0f);
    // 앞서 닫은 카드의 늦은 보고는 버린다.
    hover.test.send(ref_list_placed_intent { .serial = serial - 1, .requested_x = 40.0f, .requested_y = 60.0f, .x = 10.0f, .y = 0.0f });
    CHECK(hover.test.controller.state().ref_list->placed_shift == 12.0f);
    CHECK_FALSE(hover.test.controller.state().ref_list->flipped);
    // 몸통까지 세로로 밀렸으면 화면 끝이라 칩 반대쪽으로 한 번 뒤집는다.
    hover.test.send(ref_list_placed_intent { .serial = serial, .requested_x = 40.0f, .requested_y = 60.0f, .x = 40.0f, .y = 20.0f, .margin_below = 4.0f });
    CHECK(hover.test.controller.state().ref_list->flipped);
    CHECK(hover.test.controller.state().ref_list->placed_shift == 0.0f);
    CHECK(hover.test.controller.state().ref_list->shadow_trim == 0.0f);
    // 뒤집은 쪽도 몸통까지 밀리면 오가지 않도록 그대로 둔다.
    hover.test.send(ref_list_placed_intent { .serial = serial, .requested_x = 40.0f, .requested_y = 60.0f, .x = 40.0f, .y = 90.0f });
    CHECK(hover.test.controller.state().ref_list->flipped);
}

TEST_CASE("a ref card pushed only by its shadow trims the shadow instead of flipping", "[presentation][dialogs][refs]")
{
    // 호스트는 그림자 여백까지 포함한 popup 전체를 작업 영역 안으로 민다. 몸통은 들어가는데 그림자만 넘쳤으면 뒤집지 않는다.
    ref_hover hover {};
    hover.test.send(open_ref_list_intent { .sha = hover.oldest, .x = 100.0f, .y = 40.0f, .width = 24.0f, .height = 18.0f });
    REQUIRE(hover.open());
    const std::uint64_t serial { hover.test.controller.state().ref_list->serial };
    hover.test.send(ref_list_placed_intent { .serial = serial, .requested_x = 40.0f, .requested_y = 60.0f, .x = 43.0f, .y = 57.0f, .margin_below = 4.0f });
    CHECK_FALSE(hover.test.controller.state().ref_list->flipped);
    CHECK(hover.test.controller.state().ref_list->shadow_trim == 3.0f);
    CHECK(hover.test.controller.state().ref_list->placed_shift == 3.0f);
    // 깎은 popup이 제자리에 서면 다음 보고는 밀림이 없다. 깎은 값은 그대로다 (0과 값 사이를 오가지 않는다).
    hover.test.send(ref_list_placed_intent { .serial = serial, .requested_x = 40.0f, .requested_y = 60.0f, .x = 40.0f, .y = 60.0f, .margin_below = 1.0f });
    CHECK(hover.test.controller.state().ref_list->shadow_trim == 3.0f);
    CHECK(hover.test.controller.state().ref_list->placed_shift == 0.0f);
    CHECK_FALSE(hover.test.controller.state().ref_list->flipped);
    // 칩 쪽(여백 0)으로 밀렸으면 몸통이 밀린 것이라 뒤집는다.
    hover.test.send(ref_list_placed_intent { .serial = serial, .requested_x = 40.0f, .requested_y = 60.0f, .x = 40.0f, .y = 63.0f, .margin_below = 1.0f });
    CHECK(hover.test.controller.state().ref_list->flipped);
    CHECK(hover.test.controller.state().ref_list->shadow_trim == 0.0f);
}

TEST_CASE("form scrolling clamps to layout feedback and resets for the next dialog", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(run_command_intent { "git.branch.new" });
    REQUIRE(test.view().form.has_value());
    test.controller.apply_feedback(frame_feedback { .form_maximum = 300.0f });
    test.send(form_scroll_intent { .value = 200.0f, .absolute = true });
    CHECK(test.view().form->scroll == 200.0f);
    test.send(form_scroll_intent { .value = 1000.0f });
    CHECK(test.view().form->scroll == 300.0f);
    test.controller.apply_feedback(frame_feedback { .form_maximum = 100.0f });
    CHECK(test.view().form->scroll == 100.0f);
    test.send(form_scroll_intent { .value = -1000.0f });
    CHECK(test.view().form->scroll == 0.0f);
    test.send(form_button_intent { u8"cancel" });
    CHECK_FALSE(test.view().form.has_value());
    test.send(form_scroll_intent { .value = 200.0f });
    test.send(run_command_intent { "git.branch.new" });
    CHECK(test.view().form->scroll == 0.0f);
}

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

TEST_CASE("a branch below the current one is fast-forwarded to it from its own menu without a checkout", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    // feature/graph(가장 오래된 커밋)는 현재 브랜치 main의 조상이다. 읽어 둔 이력으로 알아 바로 켜진다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    const view_snapshot view { test.view() };
    REQUIRE(view.menu.has_value());
    REQUIRE(has_entry(*view.menu, u8"fast_forward_to_current"));
    CHECK(entry_enabled(*view.menu, u8"fast_forward_to_current"));
    const std::size_t before { test.operation_count() };

    test.send(menu_select_intent { u8"fast_forward_to_current" });
    REQUIRE(test.operation_count() == before + 1);
    const auto* const forward { test.last_request<fast_forward_request>() };
    REQUIRE(forward != nullptr);
    CHECK(forward->target == "feature/graph");
    CHECK(forward->to == "main");

    // 현재 브랜치 자신의 메뉴에는 없다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "main", .sha = snap::sha_of(4) }, 0.0f, 0.0f });
    const view_snapshot current { test.view() };
    REQUIRE(current.menu.has_value());
    CHECK_FALSE(has_entry(*current.menu, u8"fast_forward_to_current"));
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

TEST_CASE("pushing without a remote opens the publish dialog that adds the remote and then pushes", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.remotes.clear();
    snapshot.summary.has_upstream = false;
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, std::move(snapshot) }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    test.controller.after_events();

    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "main", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"push" });
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"add-remote-dialog");
    REQUIRE(view.form->fields.size() == 2);
    CHECK(view.form->fields[0].view.text == u8"origin");
    REQUIRE(view.form->checks.size() == 1);
    CHECK(view.form->checks[0].checked);
    // URL이 비면 단추만 꺼지고 오류 글은 없다.
    CHECK(view.form->error.empty());
    CHECK_FALSE(view.form->buttons.back().enabled);

    test.type(1, u8"-upload-pack=evil");
    view = test.view();
    CHECK_FALSE(view.form->error.empty());
    CHECK_FALSE(view.form->buttons.back().enabled);

    test.press(u8"cancel");
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "main", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"push" });
    test.type(1, u8"https://example.com/repo.git");
    view = test.view();
    REQUIRE(view.form->buttons.back().enabled);
    CHECK(view.form->preview == u8"git remote add origin https://example.com/repo.git && git push --set-upstream origin main:main");

    const std::size_t before { test.operation_count() };
    test.press(u8"submit");
    CHECK_FALSE(test.controller.state().dialog.has_value());
    // 원격 추가가 먼저 나가고, 성공해야 push가 잇는다.
    REQUIRE(test.operation_count() == before + 1);
    const auto* const add { test.last_request<add_remote_request>() };
    REQUIRE(add != nullptr);
    CHECK(add->name == "origin");
    CHECK(add->url == "https://example.com/repo.git");
    const operation_id adding { test.last_operation() };
    test.start(adding);
    test.finish(adding, operation_state::succeeded);
    REQUIRE(test.operation_count() == before + 2);
    const auto* const push { test.last_request<push_execute_request>() };
    REQUIRE(push != nullptr);
    CHECK(push->remote == "origin");
    CHECK(push->branch == "main");
    CHECK(push->remote_branch == "main");
    CHECK(push->set_upstream);
}

TEST_CASE("the publish dialog only adds the remote when publishing is off and stops when adding fails", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.remotes.clear();
    snapshot.summary.has_upstream = false;
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, std::move(snapshot) }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    test.controller.after_events();

    test.send(run_command_intent { "git.push" });
    REQUIRE(test.view().form.has_value());
    test.type(1, u8"git@example.com:me/repo.git");
    test.send(form_check_intent { 0 });
    view_snapshot view { test.view() };
    CHECK(view.form->preview == u8"git remote add origin git@example.com:me/repo.git");
    std::size_t before { test.operation_count() };
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    operation_id adding { test.last_operation() };
    test.start(adding);
    test.finish(adding, operation_state::succeeded);
    CHECK(test.operation_count() == before + 1);
    CHECK(test.last_request<push_execute_request>() == nullptr);

    // 추가가 실패하면 뒤의 push는 버린다.
    test.send(run_command_intent { "git.push" });
    test.type(1, u8"https://example.com/repo.git");
    before = test.operation_count();
    test.press(u8"submit");
    adding = test.last_operation();
    test.start(adding);
    test.finish(adding, operation_state::failed, operation_failure { failure_kind::other, u8"error: remote origin already exists.", {} });
    CHECK(test.operation_count() == before + 1);
    CHECK(test.last_request<push_execute_request>() == nullptr);
}

TEST_CASE("the add remote command refuses a remote name that already exists and does not publish by default", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(run_command_intent { "git.remote.add" });
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"add-remote-dialog");
    // origin이 이미 있어 이름을 비워 둔다. 원격이 이미 있으면 upstream을 옮기지 않게 게시는 꺼 둔다.
    CHECK(view.form->fields[0].view.text.empty());
    REQUIRE(view.form->checks.size() == 1);
    CHECK_FALSE(view.form->checks[0].checked);
    test.type(0, u8"origin");
    test.type(1, u8"https://example.com/other.git");
    view = test.view();
    CHECK_FALSE(view.form->error.empty());
    CHECK_FALSE(view.form->buttons.back().enabled);
}

TEST_CASE("an ssh URL to a plain server offers to create the bare repository before adding the remote and pushing", "[presentation][dialogs][ssh]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.remotes.clear();
    snapshot.summary.has_upstream = false;
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, std::move(snapshot) }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    test.controller.after_events();

    // https와 GitHub의 ssh 주소에는 칸이 없다. GitHub는 셸 명령을 받지 않는다.
    test.send(run_command_intent { "git.push" });
    test.type(1, u8"https://git.example.org/repos/app.git");
    CHECK(test.view().form->checks.size() == 1);
    test.press(u8"cancel");
    test.send(run_command_intent { "git.push" });
    test.type(1, u8"git@github.com:me/app.git");
    CHECK(test.view().form->checks.size() == 1);
    test.press(u8"cancel");

    test.send(run_command_intent { "git.push" });
    test.type(1, u8"me@git.example.org:repos/app.git");
    view_snapshot view { test.view() };
    REQUIRE(view.form->checks.size() == 2);
    CHECK(view.form->checks[1].label.find(u8"git init --bare") != std::u8string::npos);
    // 서버에 명령을 내는 일이라 기본은 끈다.
    CHECK_FALSE(view.form->checks[1].checked);
    CHECK(view.form->preview == u8"git remote add origin me@git.example.org:repos/app.git && git push --set-upstream origin main:main");

    test.send(form_check_intent { 1 });
    view = test.view();
    CHECK(view.form->checks[1].checked);
    // push 칸은 그대로다.
    CHECK(view.form->checks[0].checked);
    CHECK(view.form->preview == u8"ssh me@git.example.org git init --bare repos/app.git && git remote add origin me@git.example.org:repos/app.git && git push --set-upstream origin main:main");
    bool says_where { false };
    for (const form_note& warning : view.form->warnings)
        says_where = says_where || (warning.text.find(u8"me@git.example.org") != std::u8string::npos && warning.text.find(u8"repos/app.git") != std::u8string::npos);
    CHECK(says_where);

    // 서버에 만들고, 성공해야 원격을 더하고, 그 뒤에 push한다.
    const std::size_t before { test.operation_count() };
    test.press(u8"submit");
    REQUIRE(test.operation_count() == before + 1);
    const auto* const create { test.last_request<create_ssh_repository_request>() };
    REQUIRE(create != nullptr);
    CHECK(create->url == "me@git.example.org:repos/app.git");
    const operation_id creating { test.last_operation() };
    test.start(creating);
    test.finish(creating, operation_state::succeeded);
    REQUIRE(test.operation_count() == before + 2);
    REQUIRE(test.last_request<add_remote_request>() != nullptr);
    const operation_id adding { test.last_operation() };
    test.start(adding);
    test.finish(adding, operation_state::succeeded);
    REQUIRE(test.operation_count() == before + 3);
    CHECK(test.last_request<push_execute_request>() != nullptr);

    // 서버에서 실패하면 원격을 더하지 않는다. 상자를 다시 열어 고칠 수 있다.
    test.send(run_command_intent { "git.push" });
    test.type(1, u8"ssh://me@git.example.org:2222/srv/git/app.git");
    test.send(form_check_intent { 1 });
    const std::size_t again { test.operation_count() };
    test.press(u8"submit");
    const operation_id failing { test.last_operation() };
    REQUIRE(test.last_request<create_ssh_repository_request>() != nullptr);
    test.start(failing);
    test.finish(failing, operation_state::failed, operation_failure { failure_kind::auth_failed, {}, {} });
    CHECK(test.operation_count() == again + 1);
    CHECK(test.last_request<add_remote_request>() == nullptr);
}

TEST_CASE("trusting a new server once keeps the remote add and push that follow the repository creation", "[presentation][dialogs][ssh][auth]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.remotes.clear();
    snapshot.summary.has_upstream = false;
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, std::move(snapshot) }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
    test.controller.after_events();

    test.send(run_command_intent { "git.push" });
    test.type(1, u8"git.example.org:app.git");
    test.send(form_check_intent { 1 });
    test.press(u8"submit");
    const operation_id creating { test.last_operation() };
    REQUIRE(test.last_request<create_ssh_repository_request>() != nullptr);
    test.start(creating);
    askpass_recorder recorder {};
    test.ask(creating, 4, u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ", recorder.callback());
    const std::size_t before { test.operation_count() };
    test.press(u8"once");

    REQUIRE(test.operation_count() == before + 1);
    const auto* const retry { test.last_request<create_ssh_repository_request>() };
    REQUIRE(retry != nullptr);
    CHECK(retry->url == "git.example.org:app.git");
    REQUIRE(retry->ssh_command_override.has_value());
    CHECK(retry->ssh_command_override->known_hosts_path.find(u8"cyella-known-hosts-") != std::u8string::npos);

    // 거둔 operation이 취소로 끝나도 뒤의 원격 추가는 다시 낸 operation을 기다린다.
    test.finish(creating, operation_state::cancelled);
    CHECK(test.operation_count() == before + 1);
    const operation_id retried { test.last_operation() };
    test.start(retried);
    test.finish(retried, operation_state::succeeded);
    REQUIRE(test.operation_count() == before + 2);
    const auto* const add { test.last_request<add_remote_request>() };
    REQUIRE(add != nullptr);
    CHECK(add->url == "git.example.org:app.git");
}

TEST_CASE("an untracked file goes to the recycle bin after a confirmation that says so", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(discard_files_intent { .paths = {}, .untracked = { "new.txt" } });
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
    test.send(discard_files_intent { .paths = { "tracked.txt" }, .untracked = {} });
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
    CHECK(view.toasts.empty());
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
    CHECK(view.toasts.empty());
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
    const operation_id cloning { test.workspace.submit_setup(clone_request { .url = "https://example.com/r.git", .target_path = native_path(u8"C:\\work\\clone") }) };
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
    test.ask(fetching, 1, u8"Enter the device name: ", recorder.callback());
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    REQUIRE(view.form->fields.size() == 1);
    CHECK_FALSE(view.form->fields[0].secret);
    CHECK(view.form->notes.front().text == u8"Enter the device name:");
    test.type(0, u8"laptop");
    test.send(form_enter_intent {});
    REQUIRE(recorder.replies.size() == 1);

    // 알아보지 못한 질문이라도 원문이 일회용 코드를 물으면 입력을 가린다.
    test.ask(fetching, 2, u8"Verification code: ", recorder.callback());
    view = test.view();
    REQUIRE(view.form.has_value());
    REQUIRE(view.form->fields.size() == 1);
    CHECK(view.form->fields[0].secret);
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
    REQUIRE(view.toasts.empty() == false);
    REQUIRE(view.toasts.front().actions.size() == 1);
    CHECK(view.toasts.front().actions[0].key == u8"pull_then_push");

    const std::size_t before { test.operation_count() };
    test.send(notification_action_intent { u8"pull_then_push", pushing });
    REQUIRE(test.operation_count() == before + 1);
    const auto* const pull { test.last_request<pull_request>() };
    REQUIRE(pull != nullptr);
    CHECK(pull->strategy == pull_strategy::rebase);
    // 배너는 닫히고, pull이 끝나야 push가 다시 나간다.
    CHECK(test.view().toasts.empty());
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
    REQUIRE(view.toasts.empty() == false);
    REQUIRE(view.toasts.front().actions.size() == 1);
    CHECK(view.toasts.front().actions[0].key == u8"fetch");
}

TEST_CASE("a failure is notified once, stays until handled and its toast goes down after a later success", "[presentation][dialogs][notifications]")
{
    harness test {};
    test.load();
    const operation_id pushing { test.workspace.submit(test.repository, push_execute_request { .remote = "origin", .branch = "main", .remote_branch = "main" }) };
    test.start(pushing);
    test.finish(pushing, operation_state::failed, operation_failure { failure_kind::rejected_non_ff, u8"원격이 앞서 있습니다.", u8"! [rejected]" });
    const auto failures = [&test, pushing] {
        return std::count_if(test.controller.state().notifications.begin(), test.controller.state().notifications.end(), [pushing](const notification& value) { return value.operation == pushing; });
    };
    REQUIRE(failures() == 1);
    const notification& failure { *test.controller.state().latest_notification() };
    CHECK(failure.severity == notification_severity::error);
    CHECK(failure.repository_id == test.repository);
    CHECK(failure.toast);
    // 조치가 달린 실패는 사용자가 고를 때까지 저절로 내려가지 않는다.
    CHECK_FALSE(failure.toast_until.has_value());
    test.controller.after_events();
    CHECK(failures() == 1);

    // 같은 저장소의 뒤 작업이 성공하면 앞 실패의 토스트는 물러나 내려가고 센터에는 남는다.
    const operation_id fetching { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(fetching);
    test.finish(fetching, operation_state::succeeded);
    REQUIRE(failures() == 1);
    CHECK(test.controller.state().latest_notification()->toast_leaving_at.has_value());
    test.controller.tick(*test.controller.state().latest_notification()->toast_leaving_at + toast_leave_duration);
    CHECK(test.view().toasts.empty());
    CHECK(test.view().status.notifications == 1);
}

TEST_CASE("a pull that cannot fast-forward offers a rebase pull and deleting an unmerged branch offers force", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    const operation_id pulling { test.workspace.submit(test.repository, pull_request { pull_strategy::ff_only }) };
    test.start(pulling);
    test.finish(pulling, operation_state::failed, operation_failure { failure_kind::not_possible_to_fast_forward, u8"fast-forward할 수 없습니다.", {} });
    view_snapshot view { test.view() };
    REQUIRE(view.toasts.empty() == false);
    REQUIRE(view.toasts.front().actions.size() == 1);
    CHECK(view.toasts.front().actions[0].key == u8"pull_rebase");
    test.send(notification_action_intent { u8"pull_rebase", pulling });
    const auto* const rebased { test.last_request<pull_request>() };
    REQUIRE(rebased != nullptr);
    CHECK(rebased->strategy == pull_strategy::rebase);

    const operation_id deleting { test.workspace.submit(test.repository, delete_branch_request { .name = "topic", .force = false }) };
    test.start(deleting);
    test.finish(deleting, operation_state::failed, operation_failure { failure_kind::other, u8"삭제하지 못했습니다.", u8"error: the branch 'topic' is not fully merged." });
    view = test.view();
    REQUIRE(view.toasts.empty() == false);
    REQUIRE(view.toasts.front().actions.size() == 1);
    CHECK(view.toasts.front().actions[0].key == u8"force_delete");
    test.send(notification_action_intent { u8"force_delete", deleting });
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
    test.send(folder_chosen_intent { folder_purpose::open, native_path(u8"C:\\work\\nowhere") });
    CHECK_FALSE(test.view().selector.has_value());
}

TEST_CASE("opening a selector asks once to focus its filter field and reopening asks again", "[presentation][selector][focus]")
{
    harness test {};
    test.load();
    CHECK_FALSE(ui::build_ui_tree(test.view()).focus.has_value());

    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    const view_snapshot opened { test.view() };
    REQUIRE(opened.popup_focus.has_value());
    CHECK(opened.popup_focus->target == popup_focus_target::selector_filter);
    const std::uint64_t first { opened.popup_focus->id };
    CHECK(first != 0);
    const ui::built_tree built { ui::build_ui_tree(opened) };
    REQUIRE(built.focus.has_value());
    CHECK(built.focus->id == first);
    CHECK(built.focus->surface == u8"selector");
    CHECK(built.focus->target == luil::ui_element_id { ui::kind_selector_filter_input, u8"selector-filter" });
    // popup은 활성화를 받지 않는 표면이라 초점만 옮긴다.
    CHECK_FALSE(built.focus->activate);

    // 열린 채 다시 그리거나 글을 쳐도 같은 요청이다. 번호가 바뀌면 사용자가 옮긴 초점을 luil이 도로 빼앗는다.
    type_into_selector(test, u8"graph");
    const view_snapshot typed { test.view() };
    REQUIRE(typed.popup_focus.has_value());
    CHECK(typed.popup_focus->id == first);
    const ui::built_tree redrawn { ui::build_ui_tree(typed) };
    REQUIRE(redrawn.focus.has_value());
    CHECK(redrawn.focus->id == first);

    // 닫힌 popup의 요청은 싣지 않는다.
    test.send(close_menu_intent {});
    CHECK_FALSE(ui::build_ui_tree(test.view()).focus.has_value());

    // 다시 열면 새 번호다. 다른 종류의 상자로 바꿔 열어도 새 번호다.
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    const view_snapshot reopened { test.view() };
    REQUIRE(reopened.popup_focus.has_value());
    const std::uint64_t second { reopened.popup_focus->id };
    CHECK(second > first);
    const ui::built_tree rebuilt { ui::build_ui_tree(reopened) };
    REQUIRE(rebuilt.focus.has_value());
    CHECK(rebuilt.focus->id == second);
    test.send(open_selector_intent { selector_kind::repository, 20.0f, 60.0f, 150.0f });
    REQUIRE(test.view().popup_focus.has_value());
    CHECK(test.view().popup_focus->id > second);

    // 상자를 다시 눌러 닫는 길은 요청을 내지 않는다.
    REQUIRE(test.view().popup_focus.has_value());
    const std::uint64_t before_toggle { test.view().popup_focus->id };
    test.send(open_selector_intent { selector_kind::repository, 20.0f, 60.0f, 150.0f });
    CHECK_FALSE(test.view().selector.has_value());
    REQUIRE(test.view().popup_focus.has_value());
    CHECK(test.view().popup_focus->id == before_toggle);
}

namespace {
    template<typename intent_type>
    std::size_t count_intents(const std::vector<luil::input_action>& actions)
    {
        return static_cast<std::size_t>(std::ranges::count_if(actions, [](const luil::input_action& action) {
            const auto* const message { std::get_if<luil::app_message>(&action) };
            return message != nullptr && message->get<intent_type>() != nullptr;
        }));
    }
} // namespace

TEST_CASE("once the selector filter has the first focus, editing keys reach it and the list keeps arrows and Enter", "[presentation][selector][focus]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    const ui::built_tree built { ui::build_ui_tree(test.view()) };
    REQUIRE(built.focus.has_value());
    const auto popup { std::find_if(built.popups.begin(), built.popups.end(), [](const ui::built_popup& shown) { return shown.id == u8"selector"; }) };
    REQUIRE(popup != built.popups.end());

    ui::cyella_policy policy {};
    luil::interaction_controller interaction { &policy };
    interaction.set_tree(built.tree);
    interaction.set_surface_trees({ { popup->id, popup->tree } });
    // luil이 요청을 이행하는 길과 같다 (frame의 `focus` → `access_focus_event`).
    static_cast<void>(interaction.process(luil::access_focus_event { .target = built.focus->target, .surface = built.focus->surface }));
    CHECK(interaction.snapshot().focused == built.focus->target);
    CHECK(interaction.snapshot().focused_surface == u8"selector");

    // 글자·Backspace와 편집 키는 칸의 편집이 된다. 앱은 popup이 아니라 주 창이 키를 나르는 것으로 받는다.
    const auto edits = [&interaction](const luil::raw_input_event& event) { return count_intents<edit_text_intent>(interaction.process(event)); };
    CHECK(edits(luil::character_typed_event { .character = U'g' }) == 1);
    CHECK(edits(luil::character_typed_event { .character = U'\b' }) == 1);
    CHECK(edits(luil::key_pressed_event { .key = luil::key_code::home }) == 1);
    CHECK(edits(luil::key_pressed_event { .key = luil::key_code::end }) == 1);
    CHECK(edits(luil::key_pressed_event { .key = luil::key_code::arrow_left }) == 1);
    CHECK(edits(luil::key_pressed_event { .key = luil::key_code::delete_forward }) == 1);
    CHECK(edits(luil::key_pressed_event { .key = luil::key_code::key_a, .control = true }) == 1);
    CHECK(edits(luil::key_pressed_event { .key = luil::key_code::key_z, .control = true }) == 1);

    // ↑↓는 목록의 강조를 옮기고 칸을 편집하지 않는다. Enter는 강조된 항목을 고른다.
    const std::vector<luil::input_action> down { interaction.process(luil::key_pressed_event { .key = luil::key_code::arrow_down }) };
    CHECK(count_intents<edit_text_intent>(down) == 0);
    CHECK(interaction.snapshot().menu_highlight != luil::ui_element_id {});
    const std::vector<luil::input_action> chosen { interaction.process(luil::key_pressed_event { .key = luil::key_code::enter }) };
    CHECK(count_intents<edit_text_intent>(chosen) == 0);
    CHECK(count_intents<selector_select_intent>(chosen) == 1);
    // 초점은 고르기 전까지 칸에 남아 있다.
    CHECK(interaction.snapshot().focused == built.focus->target);
}

TEST_CASE("a dialog that appears while the selector is open closes it so the dialog takes the first focus", "[presentation][selector][focus]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    test.send(selector_select_intent { u8"branch:feature/graph" });
    const operation_id id { test.last_operation() };
    REQUIRE(id != 0);
    test.start(id);
    // 체크아웃이 도는 동안 다시 연 목록이다. 칸이 초점을 쥐고 있다.
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    REQUIRE(ui::build_ui_tree(test.view()).focus.has_value());

    // 작업이 늦게 대화 상자를 띄운다. popup 칸이 초점을 쥔 채 남으면 대화 상자의 진입이 서지 않아 글자가 목록으로 간다.
    input_request blocked {};
    blocked.kind = input_kind::dirty_worktree;
    blocked.target = "feature/graph";
    blocked.files = { "a.txt" };
    test.finish(id, operation_state::needs_input, std::nullopt, blocked);
    const view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK_FALSE(view.selector.has_value());
    const ui::built_tree built { ui::build_ui_tree(view) };
    CHECK(built.popups.empty());
    CHECK_FALSE(built.focus.has_value());
    REQUIRE(built.tree != nullptr);
    CHECK(built.tree->focus_trap_entry().has_value());
    // 닫은 것은 상태다. 대화 상자가 사라져도 목록이 되살아나지 않는다.
    CHECK_FALSE(test.view().selector.has_value());
}

TEST_CASE("the font list popup asks to focus its search field and one request wins per frame", "[presentation][settings][focus]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    REQUIRE(test.view().popup_focus.has_value());
    const std::uint64_t selector_request { test.view().popup_focus->id };

    // 환경 설정을 열면 선택 상자가 닫히고, 대화 상자의 첫 초점(가둠의 진입)은 주 창 tree가 그대로 가진다.
    test.send(open_settings_intent {});
    const view_snapshot settings { test.view() };
    REQUIRE(settings.settings.has_value());
    const ui::built_tree dialog { ui::build_ui_tree(settings) };
    CHECK_FALSE(dialog.focus.has_value());
    REQUIRE(dialog.tree != nullptr);
    CHECK(dialog.tree->focus_trap_entry().has_value());

    test.send(open_font_list_intent { { true, 20.0f, 40.0f, 240.0f } });
    const view_snapshot listed { test.view() };
    REQUIRE(listed.popup_focus.has_value());
    CHECK(listed.popup_focus->target == popup_focus_target::code_font_search);
    CHECK(listed.popup_focus->id > selector_request);
    const ui::built_tree built { ui::build_ui_tree(listed) };
    REQUIRE(built.focus.has_value());
    CHECK(built.focus->id == listed.popup_focus->id);
    CHECK(built.focus->surface == u8"font-list");
    const luil::ui_element_id search { ui::kind_code_font_input, u8"settings-font-search" };
    CHECK(built.focus->target == search);
    const auto popup { std::find_if(built.popups.begin(), built.popups.end(), [](const ui::built_popup& shown) { return shown.id == u8"font-list"; }) };
    REQUIRE(popup != built.popups.end());
    REQUIRE(popup->tree != nullptr);
    const luil::ui_element* const field { popup->tree->find(search) };
    REQUIRE(field != nullptr);
    CHECK(field->focusable());
    // luil은 보이는 크기가 있는 칸에만 요청을 이행한다 (`focus_target_ready`).
    CHECK(field->bounds().width > 0.0f);
    CHECK(field->bounds().height > 0.0f);

    // 다른 글꼴 칸의 목록으로 바꿔 열면 그 칸의 검색이 새 번호로 이긴다.
    test.send(open_font_list_intent { { false, 20.0f, 80.0f, 240.0f } });
    const view_snapshot switched { test.view() };
    REQUIRE(switched.popup_focus.has_value());
    CHECK(switched.popup_focus->target == popup_focus_target::ui_font_search);
    CHECK(switched.popup_focus->id > listed.popup_focus->id);
    const ui::built_tree rebuilt { ui::build_ui_tree(switched) };
    REQUIRE(rebuilt.focus.has_value());
    CHECK(rebuilt.focus->target == luil::ui_element_id { ui::kind_ui_font_input, u8"settings-font-search" });

    test.send(close_font_list_intent {});
    CHECK_FALSE(ui::build_ui_tree(test.view()).focus.has_value());
}

namespace {
    // 환경 설정 위에 뜬 대화 상자가 글꼴 목록을 접고 자기 첫 초점(가둠의 진입)을 받는지 본다.
    //  - 검색 칸이 초점을 쥔 popup이 남으면 luil은 초점이 비지 않아 진입을 세우지 않는다. 친 글자와 Enter가 글꼴 칸으로 가
    //    친 글이 글꼴 이름으로 저장된다.
    void check_font_list_folded(const view_snapshot& view)
    {
        REQUIRE(view.settings.has_value());
        CHECK_FALSE(view.settings->font_list.has_value());
        const ui::built_tree built { ui::build_ui_tree(view) };
        CHECK(std::none_of(built.popups.begin(), built.popups.end(), [](const ui::built_popup& shown) { return shown.id == u8"font-list"; }));
        CHECK_FALSE(built.focus.has_value());
        REQUIRE(built.tree != nullptr);
        CHECK(built.tree->focus_trap_entry().has_value());
    }
} // namespace

TEST_CASE("the palette opened over the settings folds the font list", "[presentation][settings][focus]")
{
    harness test {};
    test.load();
    test.send(open_settings_intent {});
    test.send(open_font_list_intent { { false, 20.0f, 40.0f, 240.0f } });
    REQUIRE(ui::build_ui_tree(test.view()).focus.has_value());

    // Ctrl+Shift+P는 글꼴 목록에 걸리지 않고 등록된 단축키로 팔레트를 연다.
    test.send(open_palette_intent {});
    const view_snapshot view { test.view() };
    REQUIRE(view.palette.has_value());
    check_font_list_folded(view);

    // 접은 것은 상태다. 팔레트를 닫아도 목록이 되살아나지 않는다.
    test.send(close_palette_intent {});
    REQUIRE(test.view().settings.has_value());
    CHECK_FALSE(test.view().settings->font_list.has_value());
}

TEST_CASE("a prompt that appears over the settings folds the font list", "[presentation][settings][focus]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::branch, 200.0f, 60.0f, 220.0f });
    test.send(selector_select_intent { u8"branch:feature/graph" });
    const operation_id id { test.last_operation() };
    REQUIRE(id != 0);
    test.start(id);
    test.send(open_settings_intent {});
    test.send(open_font_list_intent { { true, 20.0f, 40.0f, 240.0f } });
    REQUIRE(ui::build_ui_tree(test.view()).focus.has_value());

    // 사용자가 시작한 작업이 환경 설정 위에 질문을 띄운다.
    input_request blocked {};
    blocked.kind = input_kind::dirty_worktree;
    blocked.target = "feature/graph";
    blocked.files = { "a.txt" };
    test.finish(id, operation_state::needs_input, std::nullopt, blocked);
    const view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    check_font_list_folded(view);
}

TEST_CASE("closing the font list by choosing, Enter or Esc returns the focus to its font field", "[presentation][settings][focus]")
{
    harness test {};
    test.load();
    test.send(open_settings_intent {});

    // 돌아갈 칸의 요청을 확인한다. 표면은 주 창이고, 칸은 이 tree에 보이는 크기로 서 있어야 luil이 이행한다.
    const auto check_returned = [&test](const bool code, const std::uint64_t after) {
        const view_snapshot view { test.view() };
        REQUIRE(view.settings.has_value());
        CHECK_FALSE(view.settings->font_list.has_value());
        REQUIRE(view.popup_focus.has_value());
        CHECK(view.popup_focus->target == (code ? popup_focus_target::code_font_field : popup_focus_target::ui_font_field));
        CHECK(view.popup_focus->id > after);
        const ui::built_tree built { ui::build_ui_tree(view) };
        REQUIRE(built.focus.has_value());
        CHECK(built.focus->id == view.popup_focus->id);
        CHECK(built.focus->surface.empty());
        const luil::ui_element_id field { code ? ui::kind_code_font_input : ui::kind_ui_font_input, code ? u8"settings-code-font" : u8"settings-ui-font" };
        CHECK(built.focus->target == field);
        REQUIRE(built.tree != nullptr);
        const luil::ui_element* const element { built.tree->find(field) };
        REQUIRE(element != nullptr);
        CHECK(element->focusable());
        CHECK(element->bounds().width > 0.0f);
        CHECK(element->bounds().height > 0.0f);
        return view.popup_focus->id;
    };

    test.send(open_font_list_intent { { false, 20.0f, 40.0f, 240.0f } });
    std::uint64_t last { test.view().popup_focus->id };
    test.send(select_font_intent { false, u8"Consolas" });
    last = check_returned(false, last);

    test.send(open_font_list_intent { { true, 20.0f, 40.0f, 240.0f } });
    last = test.view().popup_focus->id;
    test.send(apply_font_settings_intent {});
    last = check_returned(true, last);

    test.send(open_font_list_intent { { false, 20.0f, 40.0f, 240.0f } });
    last = test.view().popup_focus->id;
    test.send(close_font_list_intent { .escape = true });
    last = check_returned(false, last);

    // 바깥 누름으로 닫으면 누른 자리가 초점을 잡으므로 돌려보내지 않는다. 앞의 요청은 이미 이행된 같은 번호다.
    test.send(open_font_list_intent { { false, 20.0f, 40.0f, 240.0f } });
    const std::uint64_t opened { test.view().popup_focus->id };
    test.send(close_font_list_intent {});
    REQUIRE(test.view().popup_focus.has_value());
    CHECK(test.view().popup_focus->id == opened);
    CHECK_FALSE(ui::build_ui_tree(test.view()).focus.has_value());

    // 목록이 닫혀 있을 때의 Enter(글꼴 칸에서 적용)는 초점을 옮기지 않는다.
    test.send(apply_font_settings_intent {});
    CHECK(test.view().popup_focus->id == opened);

    // 환경 설정이 닫히면 글꼴 칸의 요청은 버린다. 다시 연 화면의 첫 초점을 빼앗지 않는다.
    test.send(open_font_list_intent { { false, 20.0f, 40.0f, 240.0f } });
    test.send(select_font_intent { false, {} });
    REQUIRE(test.view().popup_focus.has_value());
    test.send(close_settings_intent {});
    CHECK_FALSE(test.view().popup_focus.has_value());
    test.send(open_settings_intent {});
    CHECK_FALSE(ui::build_ui_tree(test.view()).focus.has_value());
}

TEST_CASE("without the return request luil moves the focus from a closed font list to the settings page list", "[presentation][settings][focus]")
{
    harness test {};
    test.load();
    test.send(open_settings_intent {});
    test.send(open_font_list_intent { { false, 20.0f, 40.0f, 240.0f } });
    const ui::built_tree opened { ui::build_ui_tree(test.view()) };
    REQUIRE(opened.focus.has_value());
    const auto popup { std::find_if(opened.popups.begin(), opened.popups.end(), [](const ui::built_popup& shown) { return shown.id == u8"font-list"; }) };
    REQUIRE(popup != opened.popups.end());

    ui::cyella_policy policy {};
    luil::interaction_controller interaction { &policy };
    interaction.set_tree(opened.tree);
    interaction.set_surface_trees({ { popup->id, popup->tree } });
    static_cast<void>(interaction.process(luil::access_focus_event { .target = opened.focus->target, .surface = opened.focus->surface }));
    REQUIRE(interaction.snapshot().focused == opened.focus->target);
    REQUIRE(interaction.snapshot().focused_surface == u8"font-list");

    // Esc로 닫으면 popup tree가 사라지며 칸의 초점도 거둬지고, 환경 설정의 가둠 진입이 페이지 목록에 초점을 세운다.
    test.send(close_font_list_intent { .escape = true });
    const ui::built_tree closed { ui::build_ui_tree(test.view()) };
    interaction.set_surface_trees({});
    interaction.set_tree(closed.tree);
    REQUIRE(closed.tree->focus_trap_entry().has_value());
    CHECK(interaction.snapshot().focused == *closed.tree->focus_trap_entry());

    // 돌려보내는 요청이 이행되면 초점은 글꼴 칸이고, 친 글자는 그 칸의 편집이다.
    REQUIRE(closed.focus.has_value());
    static_cast<void>(interaction.process(luil::access_focus_event { .target = closed.focus->target, .surface = closed.focus->surface }));
    CHECK(interaction.snapshot().focused == ui::font_field_id(false));
    CHECK(interaction.snapshot().focused_surface.empty());
    CHECK(count_intents<edit_text_intent>(interaction.process(luil::character_typed_event { .character = U'x' })) == 1);
}

TEST_CASE("the repository selector switches between open tabs and reopens recent repositories", "[presentation][selector]")
{
    harness test {};
    test.load();
    const repository_tab_id first { test.controller.tabs().selected().id };
    test.controller.open_repository(native_path(u8"C:\\work\\other"));
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
    CHECK(selector_keys(view) == std::vector<std::u8string> { tab_key(first), u8"recent:" + native_path(u8"C:\\work\\other"), u8"open_folder" });
    test.send(selector_select_intent { u8"recent:" + native_path(u8"C:\\work\\other") });
    CHECK(test.controller.tabs().tabs().size() == 2);
    CHECK(test.controller.tabs().selected().path == native_path(u8"C:\\work\\other"));
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
    const std::u8string old_path { native_path(u8"C:\\work\\repo") };
    REQUIRE(test.controller.settings().recent_repositories.empty() == false);
    REQUIRE(test.controller.settings().recent_repositories.front().path == old_path);

    test.files.add_directory(native_path(u8"C:/moved/place"));
    test.process.when({ "rev-parse" }, test::scripted_response { 0, native_path("C:/moved/place/.git\n" + native_path("C:/moved/place\n")) });
    test.send(folder_chosen_intent { folder_purpose::locate, native_path(u8"C:\\moved\\place"), old_path });
    settle_inspection(test);

    REQUIRE(test.controller.settings().recent_repositories.empty() == false);
    CHECK(test.controller.settings().recent_repositories.front().path == native_path(u8"C:\\moved\\place"));
    CHECK(test.controller.settings().recent_repositories.front().display_name == u8"place");
    REQUIRE(test.controller.state().latest_notification() != nullptr);
    CHECK_FALSE(test.controller.state().latest_notification()->severity == notification_severity::warning);
    CHECK(test.controller.state().latest_notification()->text.find(u8"위치를 바꿨습니다") != std::u8string::npos);
}

TEST_CASE("locating to a folder that is not a repository changes nothing and says so", "[presentation][locate]")
{
    harness test {};
    const std::u8string old_path { native_path(u8"C:\\work\\repo") };
    test.files.add_directory(native_path(u8"C:/plain/folder"));
    // rev-parse가 실패하면(저장소가 아님) 그냥 폴더다.
    test.process.when({ "rev-parse" }, test::scripted_response { 128, "" });
    test.send(folder_chosen_intent { folder_purpose::locate, native_path(u8"C:\\plain\\folder"), old_path });
    settle_inspection(test);

    REQUIRE(test.controller.settings().recent_repositories.empty() == false);
    CHECK(test.controller.settings().recent_repositories.front().path == old_path);
    REQUIRE(test.controller.state().latest_notification() != nullptr);
    CHECK(test.controller.state().latest_notification()->severity == notification_severity::warning);
    CHECK(test.controller.state().latest_notification()->text.find(u8"Git 저장소가 아니라서") != std::u8string::npos);
}

TEST_CASE("the open button lists apps for the repository folder and opens the picked one", "[presentation][selector]")
{
    harness test {};
    test.load();
    test.send(open_selector_intent { selector_kind::repository_app, 900.0f, 60.0f, 80.0f });
    const view_snapshot view { test.view() };
    REQUIRE(view.selector.has_value());
    CHECK(view.selector->kind == selector_kind::repository_app);
    // 원격(https://example.com/repo.git)의 웹 페이지 링크가 앱 목록 뒤에 선다.
    CHECK(selector_keys(view) == std::vector<std::u8string> { u8"repository-app:0", u8"repository-app:1", u8"web-link:origin", u8"file-app-settings" });
    // 앞의 두 앱은 이 OS의 기본 앱이다 (Windows: 탐색기·Code.exe, macOS: Finder·`open -b`로 띄우는 VS Code).
    const std::vector<external_file_app> defaults { repository_open_apps({}) };
    CHECK(view.selector->entries[0].label == defaults[0].name);
    CHECK(view.selector->entries[1].label == u8"Visual Studio Code");
    CHECK(view.selector->entries[2].label == u8"example.com에서 열기 (origin)");

    test.send(selector_select_intent { u8"repository-app:1" });
    CHECK_FALSE(test.view().selector.has_value());
    REQUIRE(test.opened_folders.size() == 1);
    CHECK(test.opened_folders[0].first.executable == defaults[1].executable);
    CHECK(test.opened_folders[0].first.arguments == defaults[1].arguments);
    CHECK(test.opened_folders[0].second == native_path(u8"C:\\work\\repo"));
    // 저장소를 열지 않으므로 git operation도 없다.
    CHECK(test.operation_count() == 0);
}

TEST_CASE("palette commands open the repository folder in Explorer or VS Code", "[presentation][palette]")
{
    harness test {};
    test.load();
    test.send(run_command_intent { "repo.reveal" });
    test.send(run_command_intent { "repo.open_vscode" });
    REQUIRE(test.opened_folders.size() == 2);
    const std::vector<external_file_app> defaults { repository_open_apps({}) };
    CHECK(test.opened_folders[0].first.executable == defaults[0].executable);
    CHECK(test.opened_folders[1].first.executable == defaults[1].executable);
    CHECK(test.opened_folders[1].second == native_path(u8"C:\\work\\repo"));

    // 시작 탭에서는 열 저장소가 없다.
    test.send(new_tab_intent {});
    test.send(run_command_intent { "repo.reveal" });
    CHECK(test.opened_folders.size() == 2);
}

TEST_CASE("right-clicking a tab opens its menu even on the start page and runs the picked action", "[presentation][tabs][menu]")
{
    harness test {};
    test.load();
    test.controller.open_repository(native_path(u8"C:\\work\\other"));
    test.send(new_tab_intent {});
    REQUIRE(test.controller.tabs().tabs().size() == 3);
    const repository_tab_id first { test.controller.tabs().tabs()[0].id };

    // 시작 탭이 선택된 채로도 저장소 탭의 메뉴가 선다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::tab, .tab = first }, 40.0f, 30.0f });
    view_snapshot view { test.view() };
    REQUIRE(view.menu.has_value());
    CHECK(view.menu->title == u8"repo");

    test.send(menu_select_intent { u8"copy_path" });
    CHECK(test.controller.take_clipboard_text() == std::optional<std::u8string> { native_path(u8"C:\\work\\repo") });

    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::tab, .tab = first }, 40.0f, 30.0f });
    test.send(menu_select_intent { u8"open_vscode" });
    REQUIRE(test.opened_folders.size() == 1);
    CHECK(test.opened_folders[0].first.executable == repository_open_apps({})[1].executable);
    CHECK(test.opened_folders[0].second == native_path(u8"C:\\work\\repo"));

    // 오른쪽 탭을 닫으면 첫 탭만 남는다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::tab, .tab = first }, 40.0f, 30.0f });
    test.send(menu_select_intent { u8"close_right" });
    REQUIRE(test.controller.tabs().tabs().size() == 1);
    CHECK(test.controller.tabs().tabs()[0].id == first);
}

namespace {
    // 작업 영역에 스테이징 전 파일 넷(추적하지 않는 new.txt 포함)과 커밋에 포함된 파일 하나를 둔 저장소를 읽고 WIP를 고른다.
    void load_working_changes(harness& test)
    {
        repository_session& session { test.load(5, 0) };
        repository_snapshot snapshot { snap::make_snapshot(snap::sha_of(4), 5) };
        snapshot.unstaged = {
            { .path = "src/a.cpp", .status = 'M', .side = change_side::unstaged },
            { .path = "src/b.cpp", .status = 'M', .side = change_side::unstaged },
            { .path = "src/c.cpp", .status = 'M', .side = change_side::unstaged },
            { .path = "src/new.txt", .status = '?', .side = change_side::unstaged },
        };
        snapshot.staged = { { .path = "README.md", .status = 'A', .side = change_side::staged } };
        const std::uint64_t generation { session.begin_refresh() };
        REQUIRE(session.apply(snapshot_loaded { generation, std::move(snapshot) }));
        REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(5), .page_complete = true, .history_complete = true }));
        test.send(select_commit_intent { graph_item_key(std::string { graph_wip_sha }) });
    }

    select_file_intent pick_file(const std::string& path, const bool control = false, const bool shift = false, const diff_view view = diff_view::unstaged)
    {
        const std::u8string prefix { view == diff_view::staged ? u8"s:" : u8"u:" };
        return select_file_intent {
            .request = diff_file_request { .path = path, .view = view, .sha = {}, .untracked = path.ends_with(".txt") },
            .key = prefix + std::u8string { path.begin(), path.end() },
            .control = control,
            .shift = shift,
        };
    }

    // WIP를 고른 저장소 탭이 연 diff다.
    const std::optional<diff_file_request>& shown_diff(harness& test)
    {
        for (const auto& [id, tab] : test.controller.state().tabs)
            if (tab.selected_sha == graph_wip_sha)
                return tab.diff;
        static const std::optional<diff_file_request> none {};
        return none;
    }

    std::vector<std::u8string> working_selection(harness& test)
    {
        const view_snapshot view { test.view() };
        REQUIRE(view.repository.has_value());
        REQUIRE(view.repository->working.has_value());
        return view.repository->working->selection_keys;
    }
} // namespace

TEST_CASE("working files are picked one by one with Ctrl and as a range with Shift", "[presentation][working]")
{
    harness test {};
    load_working_changes(test);
    test.send(pick_file("src/a.cpp"));
    CHECK(working_selection(test) == std::vector<std::u8string> { u8"u:src/a.cpp" });

    // Ctrl은 하나 더하고 그 파일의 diff를 연다.
    test.send(pick_file("src/c.cpp", true));
    CHECK(working_selection(test) == std::vector<std::u8string> { u8"u:src/a.cpp", u8"u:src/c.cpp" });
    REQUIRE(shown_diff(test).has_value());
    CHECK(shown_diff(test)->path == "src/c.cpp");

    // Shift는 기준(마지막 Ctrl+클릭한 c)부터 누른 b까지다. Ctrl+Shift는 그 범위를 더한다.
    test.send(pick_file("src/b.cpp", false, true));
    CHECK(working_selection(test) == std::vector<std::u8string> { u8"u:src/b.cpp", u8"u:src/c.cpp" });
    test.send(pick_file("src/new.txt", true, true));
    CHECK(working_selection(test) == std::vector<std::u8string> { u8"u:src/b.cpp", u8"u:src/c.cpp", u8"u:src/new.txt" });

    // 보이던 파일(new.txt)을 Ctrl로 빼면 남은 것 중 마지막에 고른 파일이 보인다.
    test.send(pick_file("src/new.txt", true));
    CHECK(working_selection(test) == std::vector<std::u8string> { u8"u:src/b.cpp", u8"u:src/c.cpp" });
    CHECK(shown_diff(test)->path == "src/c.cpp");

    // 다른 묶음의 파일은 함께 고르지 않는다. 그 파일 하나로 다시 시작한다.
    test.send(pick_file("README.md", true, false, diff_view::staged));
    CHECK(working_selection(test) == std::vector<std::u8string> { u8"s:README.md" });

    // 키 없는 고르기(다른 길로 연 diff)는 여러 파일 선택을 푼다. 다음 Ctrl+클릭은 보이는 파일부터 잇는다.
    test.send(select_file_intent { .request = diff_file_request { .path = "src/a.cpp", .view = diff_view::unstaged } });
    CHECK(working_selection(test).empty());
    test.send(pick_file("src/b.cpp", true));
    CHECK(working_selection(test) == std::vector<std::u8string> { u8"u:src/a.cpp", u8"u:src/b.cpp" });

    // 마지막 하나까지 빼면 diff도 닫힌다.
    test.send(pick_file("src/a.cpp", true));
    test.send(pick_file("src/b.cpp", true));
    CHECK_FALSE(shown_diff(test).has_value());
    CHECK(working_selection(test).empty());
}

TEST_CASE("the menu of a multi-file selection stages and discards every chosen file", "[presentation][working]")
{
    harness test {};
    load_working_changes(test);
    test.send(pick_file("src/a.cpp"));
    test.send(pick_file("src/new.txt", true));
    const menu_target clicked { .kind = menu_target_kind::file, .name = "src/a.cpp", .side = change_side::unstaged };
    test.send(open_context_menu_intent { clicked, 0.0f, 0.0f });
    view_snapshot view { test.view() };
    REQUIRE(view.menu.has_value());
    CHECK(view.menu->title == u8"파일 2개");
    test.send(menu_select_intent { u8"stage" });
    const auto* const stage { test.last_request<stage_files_request>() };
    REQUIRE(stage != nullptr);
    CHECK(stage->paths == std::vector<std::string> { "src/a.cpp", "src/new.txt" });

    // 버리기는 추적 중인 파일의 restore와 추적하지 않는 파일의 휴지통을 한 확인 상자로 묻는다.
    test.send(open_context_menu_intent { clicked, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"discard" });
    view = test.view();
    REQUIRE(view.form.has_value());
    CHECK(view.form->title == u8"변경 2개를 버릴까요?");
    const dialog_state& dialog { *test.controller.state().dialog };
    REQUIRE(dialog.plan.primary.size() == 2);
    CHECK(std::get<discard_files_request>(dialog.plan.primary[0]).paths == std::vector<std::string> { "src/a.cpp" });
    CHECK(std::get<trash_files_request>(dialog.plan.primary[1]).paths == std::vector<std::string> { "src/new.txt" });

    // 선택 밖의 파일을 우클릭한 메뉴는 그 파일 하나의 것이다.
    test.press(u8"cancel");
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::file, .name = "src/b.cpp", .side = change_side::unstaged }, 0.0f, 0.0f });
    view = test.view();
    REQUIRE(view.menu.has_value());
    CHECK(view.menu->title == u8"src/b.cpp");
}

namespace {
    // 세션의 snapshot을 바꿔 새로 고침 하나를 끝까지 넣는다 (이력은 다섯 커밋 그대로다).
    void reload(repository_session& session, const repository_snapshot& snapshot, std::vector<commit_info> commits = snap::linear_history(5), const bool complete = true)
    {
        const std::uint64_t generation { session.begin_refresh() };
        REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
        REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = std::move(commits), .page_complete = true, .history_complete = complete }));
    }
} // namespace

TEST_CASE("an untracked file's diff is re-read as tracked once the status shows it in the index", "[presentation][working][diff]")
{
    harness test {};
    load_working_changes(test);
    test.send(pick_file("src/new.txt"));
    REQUIRE(shown_diff(test).has_value());
    REQUIRE(shown_diff(test)->untracked);
    repository_session& session { *test.workspace.find_session(test.repository) };
    repository_snapshot snapshot { session.snapshot() };
    REQUIRE(snapshot.unstaged.size() == 4);
    REQUIRE(snapshot.unstaged[3].path == "src/new.txt");

    // 상태를 다시 읽어도 아직 추적하지 않으면 요청도 그대로이고 다시 읽지 않는다.
    const std::size_t before { test.operation_count() };
    reload(session, snapshot);
    test.controller.after_events();
    CHECK(shown_diff(test)->untracked);
    CHECK(test.operation_count() == before);

    // 줄 일부를 stage해 index에 들어갔다 (커밋에 포함 'A', 작업 트리에 남은 줄 'M'). 같은 보기의 요청을 추적 중인 파일로 다시 낸다.
    snapshot.unstaged[3].status = 'M';
    snapshot.staged.push_back({ .path = "src/new.txt", .status = 'A', .side = change_side::staged });
    reload(session, snapshot);
    test.controller.after_events();
    REQUIRE(shown_diff(test).has_value());
    CHECK_FALSE(shown_diff(test)->untracked);
    CHECK(shown_diff(test)->path == "src/new.txt");
    CHECK(shown_diff(test)->view == diff_view::unstaged);
    CHECK(test.operation_count() == before + 1);
    const auto* const read { test.last_request<diff_file_request>() };
    REQUIRE(read != nullptr);
    CHECK(*read == *shown_diff(test));

    // 고친 뒤에는 상태가 다시 와도 또 내지 않는다.
    reload(session, snapshot);
    test.controller.after_events();
    CHECK(test.operation_count() == before + 1);

    // staged 행에서 index로 다시 빼 '?'로 돌아갔다. 추적 중인 요청은 빈 diff를 받으므로 추적하지 않는 파일로 다시 낸다.
    snapshot.unstaged[3].status = '?';
    snapshot.staged.pop_back();
    reload(session, snapshot);
    test.controller.after_events();
    REQUIRE(shown_diff(test).has_value());
    CHECK(shown_diff(test)->untracked);
    CHECK(shown_diff(test)->path == "src/new.txt");
    CHECK(shown_diff(test)->view == diff_view::unstaged);
    CHECK(test.operation_count() == before + 2);
    const auto* const reread { test.last_request<diff_file_request>() };
    REQUIRE(reread != nullptr);
    CHECK(*reread == *shown_diff(test));

    reload(session, snapshot);
    test.controller.after_events();
    CHECK(test.operation_count() == before + 2);
}

TEST_CASE("an untracked file's diff keeps its request when its row disappears", "[presentation][working][diff]")
{
    harness test {};
    load_working_changes(test);
    test.send(pick_file("src/new.txt"));
    repository_session& session { *test.workspace.find_session(test.repository) };
    repository_snapshot snapshot { session.snapshot() };
    // 휴지통으로 보내 작업 영역에서 사라졌다. index에 있다는 증거가 없으니 지금까지처럼 그대로 둔다.
    snapshot.unstaged.pop_back();
    const std::size_t before { test.operation_count() };
    reload(session, snapshot);
    test.controller.after_events();
    REQUIRE(shown_diff(test).has_value());
    CHECK(shown_diff(test)->untracked);
    CHECK(test.operation_count() == before);
}

TEST_CASE("a branch deletion confirmed on one tab is not sent after switching tabs and survives closing another tab", "[presentation][dialogs][tabs]")
{
    harness test {};
    test.load();
    const repository_tab_id first_tab { test.controller.tabs().selected().id };
    test.controller.open_repository(native_path(u8"C:\\work\\other"));
    const std::string other { test.controller.selected_repository_id() };
    REQUIRE(other != test.repository);
    // 첫 저장소로 돌아가 브랜치 삭제를 묻고, 확인하기 전에 탭을 바꾼다.
    test.send(cycle_tab_intent { true });
    REQUIRE(test.controller.selected_repository_id() == test.repository);
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"delete" });
    REQUIRE(test.controller.state().dialog.has_value());
    const std::size_t before { test.operation_count() };
    test.send(cycle_tab_intent { true });
    CHECK(test.controller.selected_repository_id() == other);
    CHECK_FALSE(test.controller.state().dialog.has_value());
    test.press(u8"submit");
    CHECK(test.operation_count() == before);

    // 고른 탭이 그대로면 다른 탭을 닫아도 상자는 남고, 확인은 그 상자의 저장소에 간다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "topic", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"delete" });
    REQUIRE(test.controller.state().dialog.has_value());
    test.send(close_tab_intent { first_tab });
    REQUIRE(test.controller.state().dialog.has_value());
    test.press(u8"submit");
    const auto* const deleted { test.last_request<delete_branch_request>() };
    REQUIRE(deleted != nullptr);
    CHECK(deleted->name == "topic");
    CHECK(test.workspace.find_operation(test.last_operation())->repository_id == other);
}

TEST_CASE("amend keeps the HEAD body, filling it when known and keeping the message when not", "[presentation][dialogs][amend]")
{
    // 본문을 읽어 둔 HEAD면 본문 칸을 채우고, 그대로 커밋하면 제목과 본문을 함께 보낸다.
    {
        harness test {};
        repository_session& session { test.load(5, 2) };
        std::vector<commit_info> commits { snap::linear_history(5) };
        commits.front().body = "first body line\nsecond body line";
        reload(session, snap::make_snapshot(commits.front().sha, 2), commits);
        test.send(select_commit_intent { graph_item_key(std::string { graph_wip_sha }) });
        test.send(toggle_amend_intent {});
        const tab_ui_state& tab { test.controller.state().tabs.at(test.controller.tabs().selected().id) };
        CHECK(tab.commit_subject.text == u8"commit 5");
        CHECK(tab.commit_body.text == u8"first body line\nsecond body line");
        test.send(commit_intent {});
        const auto* const commit { test.last_request<commit_request>() };
        REQUIRE(commit != nullptr);
        CHECK(commit->amend);
        CHECK(commit->message == "commit 5\n\nfirst body line\nsecond body line");
    }
    // 본문을 모르면 메시지를 비워 보낸다. git이 기존 메시지를 그대로 쓴다 (제목만 보내면 본문이 사라진다).
    {
        harness test {};
        test.load(5, 2);
        test.send(select_commit_intent { graph_item_key(std::string { graph_wip_sha }) });
        test.send(toggle_amend_intent {});
        CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).commit_subject.text == u8"commit 5");
        test.send(commit_intent {});
        const auto* const commit { test.last_request<commit_request>() };
        REQUIRE(commit != nullptr);
        CHECK(commit->amend);
        CHECK(commit->message.empty());
    }
}

TEST_CASE("trusting a host once for an operation that cannot retry keeps the question and says so", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    // submodule 갱신은 임시 신뢰를 받지 않는다(여러 원격에 붙는다).
    const operation_id updating { test.workspace.submit(test.repository, submodule_update_request {}) };
    test.start(updating);
    askpass_recorder recorder {};
    test.ask(updating, 5, u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ", recorder.callback());
    const std::size_t operations { test.operation_count() };
    const std::size_t notices { test.controller.state().notifications.size() };
    test.press(u8"once");
    // 원래 작업은 거두지 않고(조용히 사라지지 않는다) 다시 내지도 않는다. 대신 경고를 띄운다.
    CHECK_FALSE(test.workspace.find_operation(updating)->cancel_requested);
    CHECK(test.operation_count() == operations);
    REQUIRE(test.controller.state().notifications.size() == notices + 1);
    CHECK(test.controller.state().latest_notification()->severity == notification_severity::warning);
    CHECK(test.view().form.has_value());
}

TEST_CASE("trusting a host once also retries a tag push with a temporary known_hosts", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    const operation_id pushing { test.workspace.submit(test.repository, push_tag_request { .remote = "origin", .name = "v1.0" }) };
    test.start(pushing);
    askpass_recorder recorder {};
    test.ask(pushing, 5, u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ", recorder.callback());
    const std::size_t before { test.operation_count() };
    test.press(u8"once");

    // 처음 보는 호스트에 태그를 보내는 일도 [이번만 연결]이 같은 요청을 임시 신뢰와 함께 다시 낸다.
    CHECK(test.workspace.find_operation(pushing)->cancel_requested);
    REQUIRE(test.operation_count() == before + 1);
    const auto* const retry { test.last_request<push_tag_request>() };
    REQUIRE(retry != nullptr);
    CHECK(retry->remote == "origin");
    CHECK(retry->name == "v1.0");
    REQUIRE(retry->ssh_command_override.has_value());
    CHECK(retry->ssh_command_override->known_hosts_path.find(u8"cyella-known-hosts-") != std::u8string::npos);
}

TEST_CASE("the external diff entry of a working file hands its diff to the injected opener", "[presentation][dialogs]")
{
    harness test {};
    test.load(5, 2);
    std::vector<std::pair<std::string, diff_file_request>> opened {};
    shell_controller::dependencies dependencies {};
    dependencies.workspace = &test.workspace;
    dependencies.open_external_diff = [&opened](const std::string& repository_id, const diff_file_request& request) { opened.emplace_back(repository_id, request); };
    shell_controller controller { std::move(dependencies) };
    controller.start({});
    controller.open_repository(native_path(u8"C:\\work\\repo"));
    REQUIRE(controller.selected_repository_id() == test.repository);
    controller.handle(luil::app_message { open_context_menu_intent { menu_target { .kind = menu_target_kind::file, .name = "README.md", .side = change_side::staged }, 0.0f, 0.0f } });
    controller.handle(luil::app_message { menu_select_intent { u8"open_external_diff" } });
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].first == test.repository);
    CHECK(opened[0].second.path == "README.md");
    CHECK(opened[0].second.view == diff_view::staged);
    CHECK(controller.state().notifications.empty());

    // 열어 줄 길이 없는 조립(harness)에서는 안내만 띄운다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::file, .name = "src/a.cpp", .side = change_side::unstaged }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"open_external_diff" });
    CHECK(test.controller.state().notifications.size() == 1);
}

TEST_CASE("a stash drop is not sent when the stash at that number changed while the dialog was open", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.stashes = { { .index = 0, .name = "stash@{0}", .sha = std::string(40, 'a') } };
    reload(session, snapshot);
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::stash, .name = "stash@{0}", .index = 0 }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"drop" });
    REQUIRE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().dialog->stash_sha == std::string(40, 'a'));

    // 다른 곳에서 stash를 하나 더해 번호가 밀렸다. 확인해도 새 stash@{0}을 지우지 않는다.
    snapshot.stashes = { { .index = 0, .name = "stash@{0}", .sha = std::string(40, 'b') }, { .index = 1, .name = "stash@{1}", .sha = std::string(40, 'a') } };
    reload(session, snapshot);
    const std::size_t notices { test.controller.state().notifications.size() };
    test.press(u8"submit");
    CHECK(test.last_request<stash_drop_request>() == nullptr);
    CHECK_FALSE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().notifications.size() == notices + 1);

    // 지금 번호로 다시 고르면 지운다.
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::stash, .name = "stash@{1}", .index = 1 }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"drop" });
    test.press(u8"submit");
    const auto* const dropped { test.last_request<stash_drop_request>() };
    REQUIRE(dropped != nullptr);
    CHECK(dropped->index == 1);
}

TEST_CASE("an authentication question in front of the push dialog keeps its prefilled and typed fields", "[presentation][dialogs][auth]")
{
    harness test {};
    test.load();
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"push" });
    test.type(0, u8"-x");
    REQUIRE(test.view().form->fields[0].view.text == u8"feature/graph-x");

    // 앞서 낸 fetch가 passphrase를 묻는다. 질문이 상자를 잠시 가리고, 답하면 상자가 그대로 돌아온다.
    const operation_id fetching { test.workspace.submit(test.repository, fetch_request {}) };
    test.start(fetching);
    askpass_recorder recorder {};
    test.ask(fetching, 1, u8"Enter passphrase for key '/k': ", recorder.callback());
    view_snapshot view { test.view() };
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"auth-dialog");
    CHECK(view.form->fields[0].view.text.empty());
    test.type(0, u8"pp");
    test.press(u8"answer");
    REQUIRE(recorder.replies.size() == 1);
    view = test.view();
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"push-dialog");
    CHECK(view.form->fields[0].view.text == u8"feature/graph-x");
}

TEST_CASE("the same failure in two repositories stays as two notifications", "[presentation][dialogs][notifications]")
{
    harness test {};
    test.load();
    test.controller.open_repository(native_path(u8"C:\\work\\other"));
    const std::string other { test.controller.selected_repository_id() };
    const operation_failure offline { failure_kind::other, u8"원격에 연결할 수 없습니다.", {} };
    const operation_id first { test.workspace.submit(test.repository, fetch_request {}) };
    const operation_id second { test.workspace.submit(other, fetch_request {}) };
    for (const auto& [id, repository] : { std::pair { first, test.repository }, std::pair { second, other } })
    {
        test.workspace.apply(operation_event { id, repository, operation_started {} });
        test.workspace.apply(operation_event { id, repository, operation_finished { operation_state::failed, offline, std::nullopt, {} } });
        test.controller.after_events();
    }
    const auto& notifications { test.controller.state().notifications };
    const auto of = [&notifications](const operation_id id) { return std::ranges::count_if(notifications, [id](const notification& value) { return value.operation == id; }); };
    CHECK(of(first) == 1);
    CHECK(of(second) == 1);
}

TEST_CASE("an ignored path is escaped so the pattern matches only that file", "[presentation][dialogs]")
{
    harness test {};
    test.load(5, 2);
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::file, .name = "logs/a[1]*?.txt ", .side = change_side::unstaged }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"ignore" });
    const auto* const ignore { test.last_request<ignore_path_request>() };
    REQUIRE(ignore != nullptr);
    CHECK(ignore->pattern == "/logs/a\\[1]\\*\\?.txt\\ ");
}

TEST_CASE("the keyboard menu follows a commit picked in the graph after a sidebar row", "[presentation][dialogs]")
{
    harness test {};
    test.load();
    test.send(select_sidebar_item_intent { u8"l:main", snap::sha_of(4) });
    test.send(select_commit_intent { graph_item_key(snap::sha_of(1)) });
    test.send(run_command_intent { "view.menu" });
    REQUIRE(test.controller.state().menu.has_value());
    CHECK(test.controller.state().menu->target.kind == menu_target_kind::commit);
    CHECK(test.controller.state().menu->target.sha == snap::sha_of(1));
    test.send(close_menu_intent {});

    // 메뉴가 없는 사이드바 행(서브모듈)을 골랐으면 고른 커밋의 메뉴로 넘어가지 않는다.
    test.send(select_sidebar_item_intent { u8"sm:lib", {} });
    test.send(run_command_intent { "view.menu" });
    CHECK_FALSE(test.controller.state().menu.has_value());
}

TEST_CASE("picking a ref whose commit is not loaded yet says so", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    const std::string old_sha { std::string(40, 'f') };
    snapshot.refs.push_back({ .kind = ref_kind::tag, .name = "v0.1", .full_name = "refs/tags/v0.1", .target_sha = old_sha });
    reload(session, snapshot, snap::linear_history(5), false);
    const std::size_t notices { test.controller.state().notifications.size() };
    test.send(select_sidebar_item_intent { u8"t:v0.1", old_sha });
    CHECK(test.controller.state().notifications.size() == notices + 1);
    // 이 저장소의 이력에 없을 수 있는 행(stash 등)은 알리지 않는다.
    test.send(select_sidebar_item_intent { u8"s:0", std::string(40, 'e') });
    CHECK(test.controller.state().notifications.size() == notices + 1);
}

TEST_CASE("the create-branch dialog names a detached HEAD with its short commit from the catalog", "[presentation][dialogs]")
{
    harness test {};
    repository_session& session { test.load() };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.is_detached = true;
    reload(session, snapshot);
    test.send(run_command_intent { "git.branch.new" });
    REQUIRE(test.controller.state().dialog.has_value());
    const std::string head { snapshot.summary.head_sha };
    REQUIRE(head.size() >= 7);
    CHECK(test.controller.state().dialog->subject_label == u8"HEAD (detached " + std::u8string { head.begin(), head.begin() + 7 } + u8")");
}