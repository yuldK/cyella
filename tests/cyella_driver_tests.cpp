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
        CHECK(driver.askpass_diagnostic()->code == cyella::diagnostic_code::askpass_helper_missing);

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
