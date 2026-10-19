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
