#include "application/watch_messages.h"
#include "platform/win32/askpass_helper_path.h"
#include "platform/win32/cyella_driver.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
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
        REQUIRE(driver.controller()->state().notice.has_value());
        CHECK(driver.controller()->state().notice->text == driver.askpass_diagnostic()->message);
        CHECK(driver.controller()->state().notice->warning);
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
    CHECK(std::chrono::steady_clock::now() - edited < 3s);
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
    const std::string id { driver.workspace()->repository_ids().front() };

    driver.handle(luil::app_message { cyella::close_repository_intent { id } });
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
    // 시작할 때의 안내(예: 인증 도우미 없음)는 남아 있을 수 있다. 감시 끊김 안내만 본다.
    const auto lost_notice = [&driver] {
        const auto& notice { driver.controller()->state().notice };
        return notice.has_value() && notice->text.find(u8"감시가 끊겨") != std::u8string::npos;
    };
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
    CHECK(driver.controller()->state().notice->warning);
    CHECK(driver.controller()->state().notice->text.find(u8"감시가 끊겨") != std::u8string::npos);
    CHECK(driver.controller()->state().notice->text.find(u8"창을 다시 누를 때") != std::u8string::npos);

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
