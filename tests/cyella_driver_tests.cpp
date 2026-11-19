#include "application/app_messages.h"
#include "application/watch_messages.h"
#include "domain/text_catalog.h"
#include "platform/win32/askpass_helper_path.h"
#include "platform/win32/cyella_delegate.h"
#include "platform/win32/cyella_driver.h"
#include "platform/win32/operation_result_relay.h"
#include "presentation/dialog_intents.h"
#include "presentation/diff_view_intents.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/diff_webview.h"
#include "presentation/ui/diff_webview_theme.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/settings/appearance_ui.h"
#include "presentation/ui_intents.h"

#include "git_test_support.h"

#include "luil/app/app_host.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

TEST_CASE("cyella_driver completes shutdown after the close message", "[driver]")
{
    cyella::win32::cyella_driver driver {};
    REQUIRE_FALSE(driver.shutdown_completed());

    driver.handle(driver.make_close_message());

    CHECK(driver.shutdown_completed());
}

TEST_CASE("cyella_driver applies worker results drained from the relay", "[driver][integration]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { directory.path() / "repo" };
    std::filesystem::create_directories(repo);
    REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "init", "-q", "-b", "main" }).exit_code == 0);
    cyella::test::write_file(repo / "a.txt", "one\n");
    REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "add", "a.txt" }).exit_code == 0);
    REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "commit", "-q", "-m", "first" }).exit_code == 0);

    const std::u8string path { repo.generic_u8string() };
    cyella::win32::cyella_driver driver { path };
    driver.start();
    REQUIRE(driver.workspace() != nullptr);

    // host가 없으니 relay는 깨우지 않고 쌓기만 한다. 깨우기 메시지를 직접 넣어 비운다.
    const auto deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 30 } };
    const cyella::repository_session* session { nullptr };
    while (std::chrono::steady_clock::now() < deadline)
    {
        driver.handle(luil::app_message { cyella::relay_drain_intent {} });
        const std::vector<std::string> ids { driver.workspace()->repository_ids() };
        session = ids.empty() ? nullptr : driver.workspace()->find_session(ids.front());
        if (session != nullptr && session->loaded() && session->log_page_loading() == false)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    REQUIRE(session != nullptr);
    CHECK(session->snapshot().summary.current_branch == "main");
    CHECK(session->graph_rows().size() == 1);
    CHECK(driver.make_frame() != nullptr);

    driver.stop_workers();
}

TEST_CASE("cyella_driver reports why askpass is unavailable and wipes answers it was handed", "[driver]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    cyella::win32::cyella_driver driver {};
    driver.start();
    REQUIRE(driver.workspace() != nullptr);

    // 도우미가 실행 파일 옆에 없으면 진단이 하나 남고, 있으면 남지 않는다.
    const bool helper_present { cyella::win32::locate_askpass_helper().has_value() };
    CHECK(driver.askpass_diagnostic().has_value() == !helper_present);
    if (helper_present == false)
    {
        CHECK(driver.askpass_diagnostic()->code == cyella::diagnostic_code::askpass_helper_missing);
        // 화면은 그 진단을 안내로 한 번 보인다.
        REQUIRE(driver.controller() != nullptr);
        REQUIRE(driver.controller()->state().latest_notification() != nullptr);
        CHECK(driver.controller()->state().latest_notification()->text == driver.askpass_diagnostic()->message);
        CHECK(driver.controller()->state().latest_notification()->severity == cyella::notification_severity::warning);
    }

    // 모르는 operation에 대한 답은 아무 일도 하지 않지만, 메시지가 쥔 비밀은 지운다.
    const luil::app_message answer { cyella::answer_input_intent { 4242, 1, u8"hunter2" } };
    driver.handle(answer);
    REQUIRE(answer.get<cyella::answer_input_intent>() != nullptr);
    CHECK(answer.get<cyella::answer_input_intent>()->answer.empty());
    driver.handle(luil::app_message { cyella::cancel_input_intent { 4242, 1 } });

    // 종료 순서의 askpass 단계는 여러 번 불려도 안전하다.
    driver.stop_workers();
    driver.stop_workers();
}
namespace {
    using namespace std::chrono_literals;

    std::filesystem::path make_committed_repository(const std::filesystem::path& root)
    {
        const std::filesystem::path repo { root / "repo" };
        std::filesystem::create_directories(repo);
        REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "init", "-q", "-b", "main" }).exit_code == 0);
        cyella::test::write_file(repo / "a.txt", "one\n");
        REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "add", "a.txt" }).exit_code == 0);
        REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "commit", "-q", "-m", "first" }).exit_code == 0);
        return repo;
    }

    // 조건이 참이 될 때까지 relay를 비우며 기다린다. 창이 없으니 깨우기 메시지를 직접 넣는다.
    template<typename predicate>
    bool pump_until(cyella::win32::cyella_driver& driver, const predicate& done, const std::chrono::milliseconds limit = 15s)
    {
        const auto deadline { std::chrono::steady_clock::now() + limit };
        while (std::chrono::steady_clock::now() < deadline)
        {
            driver.handle(luil::app_message { cyella::relay_drain_intent {} });
            if (done())
                return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    // 창이 앞으로 돌아온 것과 같다: delegate가 relay에 넣고 logic thread가 비운다.
    void regain_focus(cyella::win32::cyella_driver& driver)
    {
        driver.relay().post(luil::app_message { cyella::window_focus_intent {} });
        driver.handle(luil::app_message { cyella::relay_drain_intent {} });
    }

    const cyella::repository_session* only_session(const cyella::win32::cyella_driver& driver)
    {
        const std::vector<std::string> ids { driver.workspace()->repository_ids() };
        return ids.empty() ? nullptr : driver.workspace()->find_session(ids.front());
    }

    bool all_operations_finished(const cyella::win32::cyella_driver& driver)
    {
        for (const cyella::operation_id id : driver.workspace()->operation_order())
            if (const cyella::operation_status* const status { driver.workspace()->find_operation(id) }; status != nullptr && status->finished() == false)
                return false;
        return true;
    }

    std::size_t operation_count_of(const cyella::win32::cyella_driver& driver, const cyella::operation_kind kind)
    {
        std::size_t count { 0 };
        for (const cyella::operation_id id : driver.workspace()->operation_order())
            if (const cyella::operation_status* const status { driver.workspace()->find_operation(id) }; status != nullptr && status->kind == kind)
                ++count;
        return count;
    }
} // namespace

TEST_CASE("cyella_driver follows edits and commits made outside the app without any input", "[driver][integration][watcher]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");
    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { make_committed_repository(directory.path()) };
    cyella::win32::cyella_driver driver { repo.generic_u8string() };
    driver.start();
    REQUIRE(driver.workspace() != nullptr);
    REQUIRE(pump_until(driver, [&] { return only_session(driver) != nullptr && only_session(driver)->loaded() && only_session(driver)->log_page_loading() == false; }));
    CHECK(only_session(driver)->graph_rows().size() == 1);
    CHECK(only_session(driver)->snapshot().unstaged.empty());

    // 앱 밖에서 파일을 고친다. 클릭 없이 작업 영역이 따라온다 (감시 300 ms + status).
    const auto edited { std::chrono::steady_clock::now() };
    cyella::test::write_file(repo / "a.txt", "one\ntwo\n");
    REQUIRE(pump_until(driver, [&] { return only_session(driver)->snapshot().unstaged.size() == 1; }, 5s));
    // 느린 CI의 흔들림을 받도록 상한을 넉넉히 두되, 감시 지연이 pump 한도(5초)에 닿을 만큼 늘면 잡는다.
    INFO("follow latency ms: " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - edited).count());
    CHECK(std::chrono::steady_clock::now() - edited < 4s);
    CHECK(only_session(driver)->snapshot().unstaged.front().path == "a.txt");
    // 깨끗하던 작업 트리에 변경이 생겨 WIP 행이 끼었다.
    CHECK(only_session(driver)->has_wip_row());

    // 같은 파일을 더 고치면 그래프를 다시 짜지 않고 status만 새로 읽는다.
    const std::uint64_t generation { only_session(driver)->displayed_generation() };
    const std::size_t status_runs { operation_count_of(driver, cyella::operation_kind::refresh_status) };
    cyella::test::write_file(repo / "b.txt", "new\n");
    REQUIRE(pump_until(driver, [&] { return only_session(driver)->snapshot().unstaged.size() == 2; }, 5s));
    CHECK(only_session(driver)->displayed_generation() == generation);
    CHECK(operation_count_of(driver, cyella::operation_kind::refresh_status) > status_runs);

    // 터미널에서 커밋한다. 그래프에 새 커밋이 나타나고 WIP 행이 사라진다.
    REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "add", "-A" }).exit_code == 0);
    REQUIRE(cyella::test::run_git(repo, std::vector<std::string> { "commit", "-q", "-m", "second" }).exit_code == 0);
    REQUIRE(
        pump_until(driver, [&] { return only_session(driver)->has_wip_row() == false && only_session(driver)->graph_rows().size() == 2 && only_session(driver)->log_page_loading() == false; }, 8s));
    CHECK(only_session(driver)->snapshot().unstaged.empty());
    CHECK(only_session(driver)->snapshot().staged.empty());
    CHECK(only_session(driver)->graph_commits().front().subject == "second");

    // 새로 고침이 폭주하지 않았다: 외부 변경 세 번에 전체 refresh는 열 번을 넘지 않는다.
    CHECK(operation_count_of(driver, cyella::operation_kind::refresh) < 10);
    driver.stop_workers();
}

TEST_CASE("cyella_driver stops watching a repository when its tab closes", "[driver][integration][watcher]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");
    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { make_committed_repository(directory.path()) };
    cyella::win32::cyella_driver driver { repo.generic_u8string() };
    driver.start();
    REQUIRE(pump_until(driver, [&] { return only_session(driver) != nullptr && only_session(driver)->loaded(); }));
    REQUIRE(driver.workspace()->repository_ids().size() == 1);

    // 사용자가 닫는 길(탭 닫기)로 닫는다. 그 저장소의 세션과 감시가 함께 사라진다.
    driver.handle(luil::app_message { cyella::close_selected_tab_intent {} });
    CHECK(driver.workspace()->repository_ids().empty());
    // 닫은 뒤의 변경은 아무 일도 하지 않는다 (감시가 멈췄다).
    const std::size_t before { driver.workspace()->operation_order().size() };
    cyella::test::write_file(repo / "late.txt", "x");
    std::this_thread::sleep_for(700ms);
    driver.handle(luil::app_message { cyella::relay_drain_intent {} });
    CHECK(driver.workspace()->operation_order().size() == before);
    driver.stop_workers();
}

TEST_CASE("cyella_driver tells once that a vanished folder is no longer watched and refreshes on window focus", "[driver][integration][watcher]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");
    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { make_committed_repository(directory.path()) };
    cyella::win32::cyella_driver driver { repo.generic_u8string() };
    driver.start();
    REQUIRE(pump_until(driver, [&] { return only_session(driver) != nullptr && only_session(driver)->loaded() && only_session(driver)->log_page_loading() == false; }));
    REQUIRE(driver.controller() != nullptr);
    // 시작할 때의 안내(예: 인증 도우미 없음)도 알림 목록에 있을 수 있다. 감시 끊김 안내만 찾는다.
    const auto lost = [&driver]() -> const cyella::notification* {
        for (const cyella::notification& value : driver.controller()->state().notifications)
            if (value.text.find(u8"감시가 끊겨") != std::u8string::npos)
                return &value;
        return nullptr;
    };
    const auto lost_notice = [&lost] { return lost() != nullptr; };
    CHECK_FALSE(lost_notice());

    // 불러온 뒤 이어지는 자동 작업(서명 확인 등)이 끝나기를 기다린다. git이 이 폴더를 현재 디렉터리로 쥐고 있으면
    // Windows는 폴더를 지우지 못하고(공유 위반) 폴더가 남으니 감시도 끊기지 않는다.
    REQUIRE(pump_until(driver, [&] { return all_operations_finished(driver); }));
    std::error_code removed {};
    std::filesystem::remove_all(repo / ".git", removed);
    std::filesystem::remove_all(repo, removed);
    INFO("remove_all: " << removed.value() << " " << removed.message());
    REQUIRE_FALSE(std::filesystem::exists(repo));
    REQUIRE(pump_until(driver, lost_notice, 5s));
    CHECK(lost()->severity == cyella::notification_severity::warning);
    CHECK(lost()->text.find(u8"창을 다시 누를 때") != std::u8string::npos);

    // 포기한 뒤에는 창 초점이 전체를 다시 읽게 한다 (폴더가 사라졌으니 읽기는 실패하지만 시도는 한다).
    const std::size_t refreshes { operation_count_of(driver, cyella::operation_kind::refresh) };
    pump_until(driver, [&] { return driver.workspace()->activity_of(driver.workspace()->repository_ids().front()).refresh_pending == false; }, 5s);
    const std::size_t settled { operation_count_of(driver, cyella::operation_kind::refresh) };
    std::this_thread::sleep_for(1100ms);
    regain_focus(driver);
    CHECK(operation_count_of(driver, cyella::operation_kind::refresh) == settled + 1);
    static_cast<void>(refreshes);
    driver.stop_workers();
}

TEST_CASE("cyella_driver refreshes the selected tab when the window regains focus", "[driver][integration][watcher]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");
    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { make_committed_repository(directory.path()) };
    cyella::win32::cyella_driver_options options {};
    options.initial_repository = repo.generic_u8string();
    // 감시가 없는 환경이다. 창 초점만이 바깥 변경을 따라잡는 길이다.
    options.disable_file_watching = true;
    cyella::win32::cyella_driver driver { std::move(options) };
    driver.start();
    REQUIRE(pump_until(driver, [&] { return only_session(driver) != nullptr && only_session(driver)->loaded() && only_session(driver)->log_page_loading() == false; }));

    cyella::test::write_file(repo / "a.txt", "changed\n");
    std::this_thread::sleep_for(700ms);
    driver.handle(luil::app_message { cyella::relay_drain_intent {} });
    CHECK(only_session(driver)->snapshot().unstaged.empty());

    regain_focus(driver);
    REQUIRE(pump_until(driver, [&] { return only_session(driver)->snapshot().unstaged.size() == 1; }, 5s));
    // 곧바로 또 초점을 받아도 한 번만 읽는다.
    const std::size_t runs { operation_count_of(driver, cyella::operation_kind::refresh_status) + operation_count_of(driver, cyella::operation_kind::refresh) };
    regain_focus(driver);
    CHECK(operation_count_of(driver, cyella::operation_kind::refresh_status) + operation_count_of(driver, cyella::operation_kind::refresh) == runs);
    driver.stop_workers();
}

namespace {
    // 설정 파일에 적을 JSON 문자열이다. 경로에는 백슬래시와 따옴표만 이스케이프하면 된다.
    std::string json_string(const std::u8string& text)
    {
        std::string quoted { "\"" };
        for (const char8_t character : text)
        {
            if (character == u8'\\' || character == u8'"')
                quoted.push_back('\\');
            quoted.push_back(static_cast<char>(character));
        }
        quoted.push_back('"');
        return quoted;
    }

    // 설정 파일 하나로 driver를 띄운다. 감시는 이 test가 묻는 것이 아니라 끈다.
    cyella::win32::cyella_driver_options options_with_settings(const std::filesystem::path& settings)
    {
        cyella::win32::cyella_driver_options options {};
        options.settings_path = settings.u8string();
        options.disable_file_watching = true;
        return options;
    }
} // namespace

TEST_CASE("cyella_driver uses the git executable chosen in settings", "[driver]")
{
    const cyella::test::temp_directory directory {};
    const std::filesystem::path settings { directory.path() / "settings.json" };

    // 없는 절대 경로를 지정하면 자동 탐색으로 물러서지 않고 그 경로가 잘못됐다고 알린다.
    const std::filesystem::path missing { directory.path() / "no-git" / "git.exe" };
    cyella::test::write_file(settings, "{ \"git_executable\": " + json_string(missing.u8string()) + " }");
    {
        cyella::win32::cyella_driver driver { options_with_settings(settings) };
        driver.start();
        CHECK(driver.git_tool().availability == cyella::git_tool_availability::path_invalid);
        CHECK(driver.git_tool().manually_configured);
        CHECK(driver.workspace() == nullptr);
        driver.stop_workers();
    }

    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    // 자동 탐색이 찾은 git을 지정하면 그 경로를 직접 지정한 값으로 쓴다.
    std::u8string found {};
    {
        cyella::win32::cyella_driver probe {};
        probe.start();
        found = probe.git_tool().executable;
        probe.stop_workers();
    }
    REQUIRE(found.empty() == false);
    cyella::test::write_file(settings, "{ \"git_executable\": " + json_string(found) + " }");
    cyella::win32::cyella_driver driver { options_with_settings(settings) };
    driver.start();
    CHECK(driver.git_tool().usable());
    CHECK(driver.git_tool().manually_configured);
    CHECK(driver.git_tool().executable == found);
    driver.stop_workers();
}

namespace {
    struct test_close_intent
    {};

    // relay가 보낸 깨우기만 세는 driver다. relay를 비우지 않으므로 결과가 큐에 쌓인 채로 남는다.
    class wake_counting_driver final : public luil::logic_driver
    {
    public:
        void start() override
        {
            started.store(true);
        }

        void handle(luil::app_message message) override
        {
            if (message.get<test_close_intent>() != nullptr)
                closed.store(true);
            else if (message.get<cyella::relay_drain_intent>() != nullptr)
                wakes.fetch_add(1);
        }

        [[nodiscard]] std::shared_ptr<const luil::ui_frame> make_frame() override
        {
            return std::make_shared<luil::ui_frame>();
        }

        [[nodiscard]] luil::app_message make_close_message() override
        {
            return luil::app_message { test_close_intent {} };
        }

        [[nodiscard]] bool shutdown_completed() const override
        {
            return closed.load();
        }

        std::atomic<bool> started { false };
        std::atomic<bool> closed { false };
        std::atomic<int> wakes { 0 };
    };

    template<typename condition_type>
    bool wait_until(const condition_type& condition)
    {
        const auto deadline { std::chrono::steady_clock::now() + 3s };
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (condition())
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return condition();
    }
} // namespace

TEST_CASE("operation_result_relay wakes the logic thread once until it is drained", "[driver][relay]")
{
    wake_counting_driver counter {};
    cyella::win32::operation_result_relay relay {};
    {
        luil::app_host host { luil::app_host::config {}, counter, nullptr };
        REQUIRE(wait_until([&] { return counter.started.load(); }));
        relay.bind(host);

        // 배출하기 전의 결과 여럿에는 깨우기 하나면 된다. post마다 보내면 결과가 쏟아질 때 app inbox가 깨우기로 찬다.
        for (int index { 0 }; index < 5; ++index)
            relay.post(luil::app_message { cyella::window_focus_intent {} });
        REQUIRE(wait_until([&] { return counter.wakes.load() >= 1; }));
        std::this_thread::sleep_for(50ms);
        CHECK(counter.wakes.load() == 1);
        CHECK(relay.pending() == 5);

        // 배출한 뒤의 첫 결과는 다시 깨운다.
        std::size_t drained { 0 };
        relay.drain([&drained](luil::app_message) { ++drained; });
        CHECK(drained == 5);
        relay.post(luil::app_message { cyella::window_focus_intent {} });
        REQUIRE(wait_until([&] { return counter.wakes.load() >= 2; }));
        std::this_thread::sleep_for(50ms);
        CHECK(counter.wakes.load() == 2);

        // host보다 relay가 오래 산다. 풀어 두어야 해제된 host로 깨우기를 보내지 않는다.
        relay.unbind();
    }
}

TEST_CASE("cyella_driver falls back to the text diff once when the diff webview cannot be created", "[driver][webview]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    cyella::win32::cyella_driver driver {};
    driver.start();
    REQUIRE(driver.diff_view() != nullptr);
    REQUIRE(driver.controller() != nullptr);
    const std::size_t before { driver.controller()->state().notifications.size() };

    luil::webview_event event {};
    event.id = std::u8string { cyella::ui::diff_webview_id };
    event.kind = luil::webview_event_kind::creation_failed;
    event.failure = luil::webview_creation_failure::runtime_too_old;
    event.error = u8"found 119, need 120";
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { event } });

    // 이 PC에 Runtime이 있든 없든 실패를 본 뒤로는 웹뷰를 쓰지 않고 frame에도 싣지 않는다. diff를 열지 않은 test라 이 둘은
    // 고치기 전에도 참일 수 있다. 고친 것을 가리는 것은 아래의 알림이다.
    CHECK_FALSE(driver.webview_usable());
    const std::shared_ptr<const luil::win32::ui_frame> frame { driver.make_frame() };
    REQUIRE(frame != nullptr);
    CHECK(frame->webviews.empty());
    // 까닭은 경고 알림 하나로 남는다.
    REQUIRE(driver.controller()->state().notifications.size() == before + 1);
    const cyella::notification* const notice { driver.controller()->state().latest_notification() };
    REQUIRE(notice != nullptr);
    CHECK(notice->severity == cyella::notification_severity::warning);
    CHECK(notice->text.find(u8"runtime_too_old") != std::u8string::npos);
    CHECK(notice->text.find(u8"found 119, need 120") != std::u8string::npos);

    // 같은 실패가 또 와도(luil이 늦게 보낸 것) 다시 알리지 않는다.
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { event } });
    CHECK(driver.controller()->state().notifications.size() == before + 1);

    driver.stop_workers();
}

TEST_CASE("the delegate turns a system appearance change into a wake-up for the logic thread", "[driver][theme]")
{
    // 값은 싣지 않는다. driver가 받으면 luil의 최신 값을 다시 읽는다.
    cyella::win32::cyella_delegate delegate {};
    luil::system_appearance appearance {};
    appearance.prefers_light = true;
    const luil::app_message message { delegate.make_system_appearance_message(appearance) };
    CHECK_FALSE(message.empty());
    CHECK(message.get<cyella::system_appearance_intent>() != nullptr);
}

TEST_CASE("cyella_driver reads the system appearance once and repaints the diff webview only after the change message", "[driver][webview][theme]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    // 표시 설정을 정하는 것은 test다. 호출 수로 frame마다 다시 읽지 않는지 본다.
    luil::system_appearance system {};
    system.prefers_light = false;
    int reads { 0 };
    cyella::win32::cyella_driver_options options {};
    options.disable_file_watching = true;
    options.read_system_appearance = [&system, &reads] {
        ++reads;
        return system;
    };
    cyella::win32::cyella_driver driver { std::move(options) };
    driver.start();
    REQUIRE(driver.diff_view() != nullptr);
    REQUIRE(driver.controller() != nullptr);
    CHECK(reads == 1);

    // 설정 파일이 없으니 테마 선호는 기본(`system`)이다. 그래서 OS의 밝은 모드가 바뀌면 웹뷰의 색이 바뀌어야 한다.
    const cyella::appearance_settings& appearance { driver.controller()->settings().appearance };
    REQUIRE(appearance.theme == cyella::theme_preference::system);
    const auto expected = [&appearance](const luil::system_appearance& value) {
        const luil::ui_color_palette palette { cyella::ui::surface_palette_for(luil::theme_preference::system, value, cyella::ui::effective_accent_id(appearance.accent)) };
        return cyella::ui::make_diff_theme(palette, appearance.ui_font, appearance.code_font);
    };
    const auto shown = [&driver] { return driver.diff_view()->controller().state().theme; };

    REQUIRE(driver.make_frame() != nullptr);
    const cyella::diff_theme dark { expected(system) };
    CHECK(shown() == dark);
    CHECK(dark.mode == cyella::diff_theme_mode::dark);

    // OS가 바뀌어도 알림이 오기 전에는 frame을 몇 번 지어도 다시 읽지 않는다 — frame마다 레지스트리를 묻지 않는다.
    system.prefers_light = true;
    for (int frame { 0 }; frame < 4; ++frame)
        REQUIRE(driver.make_frame() != nullptr);
    CHECK(reads == 1);
    CHECK(shown() == dark);

    // 알림을 받으면 한 번 다시 읽고 다음 frame이 웹뷰에 밝은 테마를 보낸다.
    driver.handle(luil::app_message { cyella::system_appearance_intent {} });
    CHECK(reads == 2);
    REQUIRE(driver.make_frame() != nullptr);
    const cyella::diff_theme light { expected(system) };
    CHECK(light.mode == cyella::diff_theme_mode::light);
    CHECK(shown() == light);
    CHECK_FALSE(shown() == dark);

    // 고대비가 켜지면 창이 쓰는 대비 색 팔레트로 칠한다.
    system.high_contrast = true;
    system.contrast_colors.window_background = luil::make_ui_color(255, 255, 255);
    system.contrast_colors.window_foreground = luil::make_ui_color(0, 0, 0);
    driver.handle(luil::app_message { cyella::system_appearance_intent {} });
    REQUIRE(driver.make_frame() != nullptr);
    CHECK(shown() == cyella::ui::make_diff_theme(luil::high_contrast_palette_for(system.contrast_colors), appearance.ui_font, appearance.code_font));
    CHECK(reads == 3);

    driver.stop_workers();
}

TEST_CASE("cyella_driver shows the text diff while the webview surface cannot present and keeps the webview in the frame", "[driver][webview][webview-presentation]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { make_committed_repository(directory.path()) };
    cyella::test::write_file(repo / "a.txt", "one\ntwo\n");
    cyella::win32::cyella_driver_options options {};
    options.initial_repository = repo.generic_u8string();
    options.disable_file_watching = true;
    // test 실행 파일 옆에는 뷰어 파일이 없어 시작할 때의 확인이 늘 거짓이다. 웹뷰를 싣는 길을 보려면 확인을 통과시킨다.
    options.assume_webview_capable = true;
    cyella::win32::cyella_driver driver { std::move(options) };
    driver.start();
    REQUIRE(driver.workspace() != nullptr);
    REQUIRE(driver.controller() != nullptr);
    REQUIRE(pump_until(driver, [&] { return only_session(driver) != nullptr && only_session(driver)->loaded() && only_session(driver)->snapshot().unstaged.size() == 1; }));
    REQUIRE(driver.webview_usable());

    driver.handle(luil::app_message { cyella::select_file_intent { .request = cyella::diff_file_request { .path = "a.txt", .view = cyella::diff_view::unstaged } } });
    REQUIRE(pump_until(driver, [&] { return only_session(driver)->selected_diff().has_value(); }));

    const luil::ui_element_id slot_id { luil::ui_element_kind::webview, std::u8string { cyella::ui::diff_webview_id } };
    const luil::ui_element_id lines_id { luil::ui_element_kind::virtual_list, u8"diff-lines" };
    const auto next_frame = [&driver] {
        std::shared_ptr<const luil::win32::ui_frame> frame { driver.make_frame() };
        REQUIRE(frame != nullptr);
        REQUIRE(frame->tree != nullptr);
        return frame;
    };

    // 표시할 수 있는 동안(첫 값이 오기 전도 같다)은 본문이 웹뷰 자리표다.
    const std::shared_ptr<const luil::win32::ui_frame> viewer { next_frame() };
    REQUIRE(viewer->webviews.size() == 1);
    CHECK(viewer->webviews.front().report_presentation);
    CHECK(viewer->tree->find(slot_id) != nullptr);
    CHECK(viewer->tree->find(lines_id) == nullptr);
    const std::size_t before { driver.controller()->state().notifications.size() };
    // 떠 있는 같은 알림은 새 알림이 앞의 것을 갈아 끼워 수가 늘지 않는다. 다시 알렸는지는 알림 번호로 본다.
    const auto serial = [&driver] { return driver.controller()->state().notification_serial; };
    const std::uint64_t serial_before { serial() };

    // 페이지를 준비된 상태로 만든다. 준비되지 않은 페이지에는 처음부터 게시하지 않으므로, 아래에서 "표시할 수 없는 동안에도
    // 게시가 이어진다"를 보려면 먼저 `ready`가 지나가야 한다.
    luil::webview_event ready {};
    ready.id = std::u8string { cyella::ui::diff_webview_id };
    ready.kind = luil::webview_event_kind::web_message_received;
    ready.url = viewer->webviews.front().navigate_url;
    ready.message = u8R"({"type":"ready","protocol":1})";
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { ready } });
    const std::shared_ptr<const luil::win32::ui_frame> readied { next_frame() };
    REQUIRE(readied->webviews.size() == 1);
    REQUIRE(readied->webviews.front().post_revision > viewer->webviews.front().post_revision);

    luil::webview_event unpresentable {};
    unpresentable.id = std::u8string { cyella::ui::diff_webview_id };
    unpresentable.kind = luil::webview_event_kind::presentation_changed;
    unpresentable.presentation_available = false;
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { unpresentable } });

    // 다음 frame의 본문은 텍스트이고 웹뷰는 frame에 남는다 — 빼면 luil이 다시 표시할 수 있게 된 것을 알릴 웹뷰가 없다.
    CHECK_FALSE(driver.webview_usable());
    CHECK(driver.webview_mounted());
    const std::shared_ptr<const luil::win32::ui_frame> text { next_frame() };
    REQUIRE(text->webviews.size() == 1);
    CHECK(text->tree->find(slot_id) == nullptr);
    CHECK(text->tree->find(lines_id) != nullptr);
    // 머리 막대는 뷰어가 없어서가 아니라 표면 때문에 텍스트라고 말한다. Runtime을 설치하라는 뜻으로 읽히는 문구가 아니다.
    const luil::ui_element* const note { text->tree->find(luil::ui_element_id { cyella::ui::kind_label, u8"diff-fallback-note" }) };
    REQUIRE(note != nullptr);
    CHECK(note->accessibility().name == cyella::tr(u8"diff.fallback_note_unpresentable"));
    CHECK_FALSE(note->accessibility().name == cyella::tr(u8"diff.fallback_note"));

    // 표시할 수 없는 동안에도 페이지는 준비된 채라 게시가 이어진다 — 돌아갈 때 내용이 최신인 까닭이다. 여기서
    // `page_reloading`을 부르면 페이지는 다시 읽힌 적이 없어 `ready`를 다시 보내지 않으므로 게시가 멈춘다.
    const std::uint64_t hidden_revision { text->webviews.front().post_revision };
    const cyella::diff_layout shown_layout { driver.diff_view()->controller().state().layout };
    const cyella::diff_layout other_layout { shown_layout == cyella::diff_layout::split ? cyella::diff_layout::inline_view : cyella::diff_layout::split };
    driver.handle(luil::app_message { cyella::set_diff_layout_intent { other_layout } });
    const std::shared_ptr<const luil::win32::ui_frame> hidden { next_frame() };
    REQUIRE(hidden->webviews.size() == 1);
    CHECK(hidden->webviews.front().post_revision > hidden_revision);
    CHECK(hidden->webviews.front().post_message.empty() == false);
    // 실패 알림이 아니라 표시할 수 없다는 알림 하나다.
    REQUIRE(driver.controller()->state().notifications.size() == before + 1);
    CHECK(serial() == serial_before + 1);
    const cyella::notification* const notice { driver.controller()->state().latest_notification() };
    REQUIRE(notice != nullptr);
    CHECK(notice->severity == cyella::notification_severity::warning);
    CHECK(notice->text == cyella::tr(u8"diff.viewer_unpresentable"));

    // 같은 값이 또 와도 다시 알리지 않는다.
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { unpresentable } });
    CHECK(serial() == serial_before + 1);

    // 다시 표시할 수 있게 되면 다음 frame부터 뷰어로 돌아간다. 알림은 더 없다.
    luil::webview_event presentable { unpresentable };
    presentable.presentation_available = true;
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { presentable } });
    CHECK(driver.webview_usable());
    const std::shared_ptr<const luil::win32::ui_frame> back { next_frame() };
    CHECK(back->webviews.size() == 1);
    CHECK(back->tree->find(slot_id) != nullptr);
    CHECK(back->tree->find(lines_id) == nullptr);
    CHECK(serial() == serial_before + 1);

    // 다시 잃어도 알림은 실행당 한 번이라 더 없다. 표시할 수 없는 동안 실패하면 실패가 이긴다 — 웹뷰를 빼고 실패 알림을 한 번 낸다.
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { unpresentable } });
    CHECK(serial() == serial_before + 1);
    CHECK_FALSE(driver.webview_usable());
    luil::webview_event failure { unpresentable };
    failure.kind = luil::webview_event_kind::browser_process_failed;
    failure.error = u8"browser process exited";
    driver.handle(luil::app_message { cyella::ui::diff_webview_event_intent { failure } });
    CHECK_FALSE(driver.webview_mounted());
    const std::shared_ptr<const luil::win32::ui_frame> failed { next_frame() };
    CHECK(failed->webviews.empty());
    CHECK(failed->tree->find(lines_id) != nullptr);
    CHECK(serial() == serial_before + 2);
    REQUIRE(driver.controller()->state().latest_notification() != nullptr);
    CHECK(driver.controller()->state().latest_notification()->text.find(u8"browser_process_failed") != std::u8string::npos);

    driver.stop_workers();
}

TEST_CASE("the ref list popup is transparent, reports its placement and maps its dismiss reasons", "[driver][refs]")
{
    using luil::win32::popup_dismiss_reason;
    cyella::ui::built_popup built {};
    built.id = u8"ref-list";
    built.ref_list = true;
    built.transparent = true;
    built.border = false;
    built.x = 40.0f;
    built.y = 60.0f;
    built.width = 300.0f;
    built.height = 200.0f;
    built.placed = [](const luil::rect_f& rect) { return luil::app_message { cyella::ref_list_placed_intent { .serial = 2, .x = rect.x, .y = rect.y } }; };
    const luil::win32::ui_popup popup { cyella::win32::make_ui_popup(std::move(built)) };
    // 꼬리와 그림자는 tree가 그린다. OS 경계·그림자가 서지 않고, 호스트가 민 자리를 알린다.
    CHECK(popup.transparent);
    CHECK_FALSE(popup.border);
    REQUIRE(popup.placed != nullptr);
    const luil::app_message placed { popup.placed(luil::rect_f { 52.0f, 60.0f, 300.0f, 200.0f }) };
    REQUIRE(placed.get<cyella::ref_list_placed_intent>() != nullptr);
    CHECK(placed.get<cyella::ref_list_placed_intent>()->x == 52.0f);

    REQUIRE(popup.dismiss != nullptr);
    const auto close_of = [&popup](const popup_dismiss_reason reason) -> std::optional<cyella::close_ref_list_intent> {
        const luil::input_action action { popup.dismiss(reason) };
        const auto* const message { std::get_if<luil::app_message>(&action) };
        const auto* const intent { message != nullptr ? message->get<cyella::close_ref_list_intent>() : nullptr };
        return intent != nullptr ? std::optional { *intent } : std::nullopt;
    };
    // 바깥 누름은 그 표식을 단 닫기다 (포인터가 "+N" 칸 안이면 controller가 버린다). Esc와 앵커가 사라짐은 닫는다.
    REQUIRE(close_of(popup_dismiss_reason::pointer_press_outside).has_value());
    CHECK(close_of(popup_dismiss_reason::pointer_press_outside)->pressed_outside);
    REQUIRE(close_of(popup_dismiss_reason::escape_key).has_value());
    CHECK_FALSE(close_of(popup_dismiss_reason::escape_key)->pressed_outside);
    CHECK(close_of(popup_dismiss_reason::anchor_closed).has_value());
    // 휠(사이드바를 굴려도 남는다)·창 이동·크기·활성 전환에는 남는다.
    for (const popup_dismiss_reason reason :
        { popup_dismiss_reason::wheel_scrolled, popup_dismiss_reason::surface_moved, popup_dismiss_reason::surface_resized, popup_dismiss_reason::activation_changed })
        CHECK(std::holds_alternative<std::monostate>(popup.dismiss(reason)));

    // 다른 popup은 transparent가 아니고 경계를 둔다 (지금까지의 모양).
    cyella::ui::built_popup menu {};
    menu.id = u8"context-menu";
    const luil::win32::ui_popup shown { cyella::win32::make_ui_popup(std::move(menu)) };
    CHECK_FALSE(shown.transparent);
    CHECK(shown.border);
    CHECK(shown.placed == nullptr);
}
