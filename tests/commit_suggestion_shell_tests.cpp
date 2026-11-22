// 커밋 메시지 제안의 화면 쪽이다 (ADR-009). worker는 띄우지 않는다 — controller가 낸 일감을 받아 적고, 결과 메시지를 손으로 넣는다.
#include "application/app_settings_service.h"
#include "application/commit_suggestion_messages.h"
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/commands/command_registry.h"
#include "presentation/graph_geometry.h"
#include "presentation/shell_controller.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui_intents.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace cyella;
    namespace snap = cyella::test::snapshot;

    class memory_store final : public app_settings_store
    {
    public:
        [[nodiscard]] app_settings_load_result load(std::u8string_view) noexcept override
        {
            app_settings_load_result result {};
            result.settings = initial;
            return result;
        }

        [[nodiscard]] app_settings_save_result save(std::u8string_view, const app_settings&, std::u8string_view) noexcept override
        {
            app_settings_save_result result {};
            result.succeeded = true;
            return result;
        }

        app_settings initial {};
    };

    app_settings enabled_settings()
    {
        app_settings settings {};
        settings.commit_suggestion.enabled = true;
        settings.commit_suggestion.executable = u8"  claude ";
        settings.commit_suggestion.arguments = u8"-p --tools \"\"";
        settings.commit_suggestion.instruction = u8"제목은 한국어로";
        return settings;
    }

    struct harness
    {
        explicit harness(app_settings settings = enabled_settings())
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , service { store, u8"memory" }
        {
            store.initial = std::move(settings);
            static_cast<void>(service.load());

            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.settings = &service;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } }; };
            dependencies.steady_clock = [] { return std::chrono::steady_clock::now(); };
            dependencies.request_commit_suggestion = [this](commit_suggestion_job job) { jobs.push_back(std::move(job)); };
            dependencies.cancel_commit_suggestion = [this](const std::uint64_t serial) { cancelled.push_back(serial); };
            controller.emplace(std::move(dependencies));
            controller->start({});
            controller->open_repository(u8"C:\\work\\repo");
            repository = controller->selected_repository_id();
            tab = controller->tabs().selected().id;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            controller->handle(luil::app_message { std::move(intent) });
            controller->after_events();
        }

        // 읽어 둔 저장소다. `changed`가 0보다 크면 staged 파일이 하나 있다.
        void load(const std::uint32_t changed = 2)
        {
            repository_session* const session { workspace.find_session(repository) };
            REQUIRE(session != nullptr);
            snap::load(*session, snap::linear_history(5), changed);
            send(select_commit_intent { graph_item_key(std::string { graph_wip_sha }) });
        }

        void type(const text_target target, const std::u8string& text)
        {
            luil::text_edit_request request {};
            request.target = to_luil_target(target);
            request.command = luil::text::text_edit_command::insert;
            request.text = text;
            send(edit_text_intent { request });
        }

        void deliver(const std::uint64_t serial, const commit_suggestion_state state, suggested_commit_message message = {}, std::u8string error = {})
        {
            send(commit_suggestion_result_intent { .serial = serial, .repository_id = repository, .state = state, .message = std::move(message), .error = std::move(error) });
        }

        [[nodiscard]] const tab_ui_state& tab_state() const
        {
            return controller->state().tabs.at(tab);
        }

        [[nodiscard]] working_area_view working()
        {
            const view_snapshot view { controller->make_view(1440.0f, 900.0f, 1.0f) };
            REQUIRE(view.repository.has_value());
            REQUIRE(view.repository->working.has_value());
            return *view.repository->working;
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        memory_store store {};
        git_operation_runner runner;
        git_workspace workspace;
        app_settings_service service;
        std::optional<shell_controller> controller {};
        std::vector<commit_suggestion_job> jobs {};
        std::vector<std::uint64_t> cancelled {};
        std::string repository {};
        repository_tab_id tab {};
    };
} // namespace

TEST_CASE("the suggestion button asks the worker and the answer fills the commit box", "[presentation][commit_suggestion]")
{
    harness test {};
    test.load();
    CHECK(test.working().suggest_visible);
    CHECK(test.working().can_suggest);

    test.send(suggest_commit_message_intent {});
    REQUIRE(test.jobs.size() == 1);
    const commit_suggestion_job& job { test.jobs.front() };
    CHECK(job.serial == 1);
    CHECK(job.repository_id == test.repository);
    CHECK(job.repository_path == u8"C:\\work\\repo");
    // 실행 파일 칸의 앞뒤 공백은 뗀다. 나머지는 설정 그대로 간다.
    CHECK(job.executable == u8"claude");
    CHECK(job.arguments == u8"-p --tools \"\"");
    CHECK(job.instruction == u8"제목은 한국어로");
    CHECK(test.tab_state().pending_suggestion == 1);

    // 도는 동안은 단추가 멈춤으로 바뀌고 다시 눌러도 일감이 늘지 않는다.
    CHECK(test.working().suggesting);
    CHECK_FALSE(test.working().can_suggest);
    test.send(suggest_commit_message_intent {});
    CHECK(test.jobs.size() == 1);

    test.deliver(1, commit_suggestion_state::succeeded, { u8"feat: 타이머 간격을 맞춘다", u8"- 0 이하는 무시한다" });
    CHECK(test.tab_state().pending_suggestion == 0);
    CHECK(test.tab_state().commit_subject.text == u8"feat: 타이머 간격을 맞춘다");
    CHECK(test.tab_state().commit_body.text == u8"- 0 이하는 무시한다");
    CHECK(test.working().can_suggest);
    CHECK(test.working().can_commit);
    CHECK(test.controller->state().notifications.empty());
}

TEST_CASE("nothing is asked while the setting is off or the state does not allow it", "[presentation][commit_suggestion]")
{
    // 꺼져 있으면 단추가 없고 메시지가 와도 일감을 내지 않는다.
    {
        harness test { app_settings {} };
        test.load();
        CHECK_FALSE(test.working().suggest_visible);
        test.send(suggest_commit_message_intent {});
        CHECK(test.jobs.empty());
    }
    // 켰지만 명령을 정하지 않았다.
    {
        app_settings settings { enabled_settings() };
        settings.commit_suggestion.executable = u8"  ";
        harness test { settings };
        test.load();
        CHECK(test.working().suggest_visible);
        CHECK_FALSE(test.working().can_suggest);
        test.send(suggest_commit_message_intent {});
        CHECK(test.jobs.empty());
    }
    // staged가 없다.
    {
        harness test {};
        test.load(0);
        test.send(suggest_commit_message_intent {});
        CHECK(test.jobs.empty());
    }
    // amend 중이다.
    {
        harness test {};
        test.load();
        test.send(toggle_amend_intent {});
        CHECK(test.working().suggest_visible);
        CHECK_FALSE(test.working().can_suggest);
        test.send(suggest_commit_message_intent {});
        CHECK(test.jobs.empty());
    }
}

TEST_CASE("the answer replaces a draft but not text typed while waiting", "[presentation][commit_suggestion]")
{
    // 누르기 전에 써 둔 글은 덮어쓴다. 누른 것이 덮어쓰라는 뜻이다.
    {
        harness test {};
        test.load();
        test.type(text_target::commit_subject, u8"wip");
        test.send(suggest_commit_message_intent {});
        test.deliver(1, commit_suggestion_state::succeeded, { u8"fix: 고친다", {} });
        CHECK(test.tab_state().commit_subject.text == u8"fix: 고친다");
        CHECK(test.tab_state().commit_body.text.empty());
    }
    // 기다리는 동안 쓴 글은 그대로 두고 알린다.
    {
        harness test {};
        test.load();
        test.send(suggest_commit_message_intent {});
        test.type(text_target::commit_subject, u8"내가 쓴 제목");
        test.deliver(1, commit_suggestion_state::succeeded, { u8"fix: 고친다", u8"본문" });
        CHECK(test.tab_state().commit_subject.text == u8"내가 쓴 제목");
        CHECK(test.tab_state().commit_body.text.empty());
        CHECK(test.tab_state().pending_suggestion == 0);
        REQUIRE(test.controller->state().notifications.size() == 1);
        CHECK(test.controller->state().notifications.front().severity == notification_severity::info);
    }
}

TEST_CASE("a failed suggestion is reported and a cancelled one is silent", "[presentation][commit_suggestion]")
{
    harness test {};
    test.load();
    test.send(suggest_commit_message_intent {});
    test.deliver(1, commit_suggestion_state::failed, {}, u8"제안 명령이 실패했습니다");
    CHECK(test.tab_state().pending_suggestion == 0);
    CHECK(test.tab_state().commit_subject.text.empty());
    REQUIRE(test.controller->state().notifications.size() == 1);
    CHECK(test.controller->state().notifications.front().text == u8"제안 명령이 실패했습니다");
    CHECK(test.controller->state().notifications.front().severity == notification_severity::warning);
    CHECK(test.controller->state().notifications.front().repository_id == test.repository);

    // 멈춤 단추는 worker에 취소를 청하고, 취소된 답이 와야 단추가 돌아온다.
    test.send(suggest_commit_message_intent {});
    REQUIRE(test.jobs.size() == 2);
    test.send(cancel_commit_suggestion_intent {});
    CHECK(test.cancelled == std::vector<std::uint64_t> { 2 });
    CHECK(test.working().suggesting);
    test.deliver(2, commit_suggestion_state::cancelled);
    CHECK_FALSE(test.working().suggesting);
    CHECK(test.controller->state().notifications.size() == 1);
}

TEST_CASE("turning the setting off while a suggestion runs keeps the stop button", "[presentation][commit_suggestion]")
{
    harness test {};
    test.load();
    test.send(suggest_commit_message_intent {});
    static_cast<void>(test.service.update([](app_settings& settings) { settings.commit_suggestion.enabled = false; }));

    // 도는 것은 멈출 수 있어야 한다. 끝나면 단추가 사라진다.
    CHECK(test.working().suggesting);
    test.send(cancel_commit_suggestion_intent {});
    CHECK(test.cancelled == std::vector<std::uint64_t> { 1 });
    test.deliver(1, commit_suggestion_state::cancelled);
    CHECK_FALSE(test.working().suggesting);
    CHECK_FALSE(test.working().suggest_visible);
    test.send(suggest_commit_message_intent {});
    CHECK(test.jobs.size() == 1);
}

TEST_CASE("a late or foreign answer is ignored", "[presentation][commit_suggestion]")
{
    harness test {};
    test.load();
    test.send(suggest_commit_message_intent {});

    // 낸 적 없는 serial과 0은 어느 탭에도 닿지 않는다.
    test.deliver(9, commit_suggestion_state::succeeded, { u8"다른 답", {} });
    test.deliver(0, commit_suggestion_state::succeeded, { u8"다른 답", {} });
    CHECK(test.tab_state().commit_subject.text.empty());
    CHECK(test.tab_state().pending_suggestion == 1);

    // 이미 받은 serial이 또 와도 두 번 채우지 않는다.
    test.deliver(1, commit_suggestion_state::succeeded, { u8"첫 답", {} });
    test.deliver(1, commit_suggestion_state::succeeded, { u8"둘째 답", {} });
    CHECK(test.tab_state().commit_subject.text == u8"첫 답");
}

TEST_CASE("the answer goes to the tab that asked, and closing that tab stops the command", "[presentation][commit_suggestion]")
{
    harness test {};
    test.load();
    test.send(suggest_commit_message_intent {});

    // 도는 동안 다른 저장소 탭으로 옮겼다. 답은 낸 탭의 입력란으로 간다.
    test.controller->open_repository(u8"C:\\work\\other");
    const repository_tab_id other { test.controller->tabs().selected().id };
    REQUIRE(other != test.tab);
    test.deliver(1, commit_suggestion_state::succeeded, { u8"feat: 낸 탭의 답", {} });
    CHECK(test.tab_state().commit_subject.text == u8"feat: 낸 탭의 답");
    CHECK(test.controller->state().tabs.at(other).commit_subject.text.empty());

    // 도는 탭을 닫으면 명령을 멈춘다. 늦게 온 답은 받을 탭이 없다.
    test.send(select_tab_intent { test.tab });
    test.type(text_target::commit_subject, u8" ");
    test.send(suggest_commit_message_intent {});
    REQUIRE(test.jobs.size() == 2);
    test.send(close_tab_intent { test.tab });
    CHECK(test.cancelled == std::vector<std::uint64_t> { 2 });
    test.deliver(2, commit_suggestion_state::succeeded, { u8"늦은 답", {} });
    CHECK(test.controller->state().tabs.at(other).commit_subject.text.empty());
}

TEST_CASE("the palette command follows the same gate as the button", "[presentation][commit_suggestion][commands]")
{
    REQUIRE(find_command("git.suggest_commit_message") != nullptr);
    command_availability availability {};
    CHECK_FALSE(command_available("git.suggest_commit_message", availability));
    availability.suggest_commit_message = true;
    CHECK(command_available("git.suggest_commit_message", availability));
}

TEST_CASE("the commit box shows the suggestion button only when the setting is on", "[presentation][commit_suggestion][ui]")
{
    const auto find = [](const ui::built_tree& built, const luil::ui_element_kind kind, const std::u8string& owner) { return built.tree->find(luil::ui_element_id { kind, owner }); };

    // 꺼져 있으면 단추가 없다.
    {
        harness test { app_settings {} };
        test.load();
        const ui::built_tree built { ui::build_ui_tree(test.controller->make_view(1440.0f, 900.0f, 1.0f)) };
        REQUIRE(find(built, ui::kind_commit_subject_input, u8"commit-subject") != nullptr);
        CHECK(find(built, ui::kind_button, u8"commit-suggest") == nullptr);
    }
    // 켜져 있으면 제목 줄 안에 서고, 도는 동안은 멈춤 단추와 도는 아이콘으로 바뀐다.
    for (const float width : { 480.0f, 900.0f, 1440.0f })
    {
        CAPTURE(width);
        harness test {};
        test.load();
        const ui::built_tree idle { ui::build_ui_tree(test.controller->make_view(width, 900.0f, 1.0f)) };
        const luil::ui_element* const subject { find(idle, ui::kind_commit_subject_input, u8"commit-subject") };
        const luil::ui_element* const button { find(idle, ui::kind_button, u8"commit-suggest") };
        if (subject == nullptr)
            continue; // 좁은 폭에서는 작업 영역이 서랍 안에 있어 tree에 없을 수 있다.
        REQUIRE(button != nullptr);
        CHECK(button->enabled());
        CHECK(button->bounds().x >= subject->bounds().x + subject->bounds().width);
        CHECK(subject->bounds().width > 60.0f);
        CHECK(find(idle, ui::kind_button, u8"commit-suggest-cancel") == nullptr);

        test.send(suggest_commit_message_intent {});
        const ui::built_tree running { ui::build_ui_tree(test.controller->make_view(width, 900.0f, 1.0f)) };
        CHECK(find(running, ui::kind_button, u8"commit-suggest") == nullptr);
        CHECK(find(running, ui::kind_button, u8"commit-suggest-cancel") != nullptr);
        CHECK(find(running, ui::kind_spinner, u8"commit-suggest-spinner") != nullptr);
    }
}
