// macOS의 OS 어댑터(src/platform/macos)와 이 OS의 경로 규칙을 본다. Windows 판(win32_settings_tests.cpp)과 같은 계약을 POSIX 위에서
// 확인하는 것이다. 실제 사용자 폴더에는 아무것도 만들지 않는다 (임시 폴더만 쓴다).
#include "application/app_settings_service.h"
#include "application/diff_view_protocol.h"
#include "application/repository_change_classifier.h"
#include "domain/external_diff_tool.h"
#include "domain/path_syntax.h"
#include "domain/repository_path.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_tool_discovery.h"
#include "infrastructure/json_app_settings_store.h"
#include "infrastructure/json_text_catalog.h"
#include "platform/macos/macos_resources.h"
#include "platform/win32/askpass_helper_path.h"
#include "platform/win32/win32_app_settings_path.h"
#include "platform/win32/win32_diff_tool_launcher.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_locale.h"
#include "platform/win32/win32_settings_file_system.h"
#include "platform/win32/win32_sha256.h"
#include "platform/win32/win32_webview_support.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <thread>

using namespace cyella;

namespace {
    std::u8string path_text(const std::filesystem::path& path)
    {
        return path.u8string();
    }

    std::set<std::u8string> placeholders_of(const std::u8string& pattern)
    {
        const std::optional<std::set<std::u8string, std::less<>>> found { text_placeholders(pattern) };
        return found.has_value() ? std::set<std::u8string> { found->begin(), found->end() } : std::set<std::u8string> {};
    }
} // namespace

TEST_CASE("posix paths are absolute only from the root", "[macos][path]")
{
    CHECK(is_absolute_path(u8"/Users/me/repo"));
    CHECK(is_absolute_path(u8"/"));
    CHECK_FALSE(is_absolute_path(u8"relative/repo"));
    CHECK_FALSE(is_absolute_path(u8""));
    CHECK_FALSE(is_absolute_path(u8"C:\\work\\repo"));
    CHECK(native_path_separator == u8'/');
    CHECK(path_list_separator == u8':');
    CHECK(executable_suffix.empty());
}

TEST_CASE("posix paths keep backslashes as file name characters", "[macos][path]")
{
    CHECK(normalize_repository_path(u8"/Users/me/repo/") == u8"/Users/me/repo");
    CHECK(normalize_repository_path(u8"/") == u8"/");
    CHECK(normalize_repository_path(u8"/Users/me/a\\b") == u8"/Users/me/a\\b");
    CHECK(repository_path_key(u8"/Users/Me/Repo") == u8"/users/me/repo");
    CHECK(repository_display_name(u8"/Users/me/repo") == u8"repo");
    CHECK(repository_display_name(u8"/") == u8"/");
    CHECK(repository_parent_folder(u8"/Users/me/repo") == u8"/Users/me");
    CHECK(repository_parent_folder(u8"/repo") == u8"/");
    CHECK(repository_parent_folder(u8"/") == u8"");
    CHECK(join_native_path(u8"/Users/me", u8"repo") == u8"/Users/me/repo");
    CHECK(join_native_path(u8"/", u8"repo") == u8"/repo");
    CHECK(normalize_path_key(u8"/Users/Me/") == u8"/users/me");
}

TEST_CASE("workspace children are joined with slashes under a posix root", "[macos][path]")
{
    CHECK(resolve_workspace_child(u8"/Users/me/repo", "src/main.cpp") == std::optional<std::u8string> { u8"/Users/me/repo/src/main.cpp" });
    CHECK(resolve_workspace_child(u8"/Users/me/repo/", "a\\b.txt") == std::optional<std::u8string> { u8"/Users/me/repo/a\\b.txt" });
    CHECK_FALSE(resolve_workspace_child(u8"/Users/me/repo", "../escape").has_value());
    CHECK_FALSE(resolve_workspace_child(u8"/Users/me/repo", ".git/config").has_value());
    CHECK_FALSE(resolve_workspace_child(u8"relative", "a.txt").has_value());
}

TEST_CASE("settings live under Application Support", "[macos][settings]")
{
    CHECK(win32::app_settings_file_path_under(u8"/Users/tester/Library/Application Support") == u8"/Users/tester/Library/Application Support/cyella/settings.json");
    CHECK(win32::app_settings_file_path_under(u8"/Users/한글/Library/Application Support/") == u8"/Users/한글/Library/Application Support/cyella/settings.json");
    CHECK(win32::app_settings_file_path_under(u8"") == u8"");

    const std::u8string roaming { win32::roaming_app_data_folder() };
    REQUIRE_FALSE(roaming.empty());
    CHECK(roaming.ends_with(u8"/Library/Application Support"));
}

TEST_CASE("the settings path creates its cyella folder on first use under a temporary root", "[macos][settings]")
{
    test::temporary_directory directory { "macos-settings-path" };
    const std::filesystem::path roaming { directory.path() / "Application Support" };
    REQUIRE_FALSE(std::filesystem::exists(roaming));

    const std::u8string path { win32::prepared_app_settings_file_path_under(path_text(roaming)) };

    REQUIRE_FALSE(path.empty());
    CHECK(std::filesystem::is_directory(roaming / "cyella"));
    CHECK_FALSE(std::filesystem::exists(roaming / "cyella" / "settings.json"));
}

TEST_CASE("the posix settings file system commits atomically and renames without overwriting", "[macos][settings]")
{
    test::temporary_directory directory { "macos-settings-fs" };
    const auto files { win32::make_settings_file_system() };
    const std::filesystem::path target { directory.path() / "deep" / "folder" / "settings.json" };
    const std::u8string target_text { path_text(target) };

    CHECK(files->read(target_text).state == settings_file_read_state::not_found);
    REQUIRE(files->atomic_commit(target_text, u8"first").succeeded());
    REQUIRE(files->atomic_commit(target_text, u8"second").succeeded());
    const settings_file_read_result read { files->read(target_text) };
    CHECK(read.state == settings_file_read_state::available);
    CHECK(read.bytes == u8"second");
    REQUIRE(files->atomic_commit(target_text, u8"").succeeded());
    CHECK(files->read(target_text).bytes.empty());

    // 임시 파일이 남지 않는다.
    std::size_t entries { 0 };
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator { target.parent_path() })
        ++entries;
    CHECK(entries == 1);

    const std::filesystem::path backup { target.parent_path() / "settings.json.bak" };
    CHECK(files->rename(target_text, path_text(backup)).succeeded);
    REQUIRE(files->atomic_commit(target_text, u8"third").succeeded());
    const settings_file_rename_result refused { files->rename(target_text, path_text(backup)) };
    CHECK_FALSE(refused.succeeded);
    CHECK(refused.native_error.has_value());
    CHECK(test::read_file(backup).empty());
}

TEST_CASE("the store works end to end on the real file system", "[macos][settings]")
{
    test::temporary_directory directory { "macos-settings-store" };
    const auto files { win32::make_settings_file_system() };
    json_app_settings_store store { *files };
    const std::u8string path { path_text(directory.path() / "cyella" / "settings.json") };
    app_settings_service first { store, path };
    static_cast<void>(first.load());
    CHECK(first.update([](app_settings& settings) { settings.git_executable = u8"/opt/homebrew/bin/git"; }).empty());

    app_settings_service second { store, path };
    static_cast<void>(second.load());
    CHECK(second.settings().git_executable == u8"/opt/homebrew/bin/git");
}

TEST_CASE("helpers sit beside the app executable without an extension", "[macos][askpass]")
{
    CHECK(win32::askpass_helper_path_beside("/Applications/Cyella.app/Contents/MacOS") == std::filesystem::path { "/Applications/Cyella.app/Contents/MacOS/cyella-askpass" });
}

TEST_CASE("sha256 uses CommonCrypto", "[macos][hash]")
{
    CHECK(win32::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(win32::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("tools are found by name in the search folders", "[macos][tools]")
{
    const auto probe { win32::make_file_probe() };
    const std::vector<std::u8string> folders { win32::tool_search_directories(u8"/nonexistent/app", u8"/usr/bin", {}, u8"relative:/bin:/usr/bin") };
    // 앞의 세 자리(앱·시스템·Windows 폴더) 다음에 PATH의 절대 항목이 순서대로 온다. 상대 항목은 버린다.
    CHECK(folders == std::vector<std::u8string> { u8"/nonexistent/app", u8"/usr/bin", u8"/bin", u8"/usr/bin" });
    CHECK(win32::resolve_tool_executable(u8"true", folders, *probe) == std::optional<std::u8string> { u8"/usr/bin/true" });
    CHECK(win32::resolve_tool_executable(u8"/usr/bin/env", folders, *probe) == std::optional<std::u8string> { u8"/usr/bin/env" });
    CHECK_FALSE(win32::resolve_tool_executable(u8"no-such-tool-for-cyella", folders, *probe).has_value());
}

TEST_CASE("the file probe tells files from folders", "[macos][probe]")
{
    const auto probe { win32::make_file_probe() };
    CHECK(probe->probe(u8"/usr/bin/true") == path_kind::file);
    CHECK(probe->probe(u8"/usr/bin") == path_kind::directory);
    CHECK(probe->probe(u8"/no/such/path") == path_kind::missing);
    CHECK(probe->probe(u8"") == path_kind::missing);
}

TEST_CASE("local data lives under the user caches", "[macos][webview]")
{
    const std::u8string avatars { win32::local_data_directory(L"avatars") };
    CHECK(avatars.ends_with(u8"/Library/Caches/cyella/avatars"));
    CHECK(win32::webview_runtime_installed());
}

TEST_CASE("the macOS wording overlays only replace existing keys with the same placeholders", "[macos][locale]")
{
    for (const auto& [base_resource, overlay_resource, language] : {
             std::tuple { macos::embedded_resource::locale_korean, macos::embedded_resource::locale_korean_macos, ui_language::korean },
             std::tuple { macos::embedded_resource::locale_english, macos::embedded_resource::locale_english_macos, ui_language::english },
         })
    {
        const text_catalog_parse_result base { parse_text_catalog_json(macos::embedded_bytes(base_resource), language) };
        const text_catalog_parse_result overlay { parse_text_catalog_json(macos::embedded_bytes(overlay_resource), language) };
        REQUIRE(overlay.catalog.entries.empty() == false);
        for (const auto& [key, value] : overlay.catalog.entries)
        {
            const std::string key_text(key.begin(), key.end());
            INFO(key_text);
            const std::u8string* const original { base.catalog.find(key) };
            REQUIRE(original != nullptr);
            CHECK(placeholders_of(value) == placeholders_of(*original));
        }
    }
}

TEST_CASE("integration: the macOS launcher starts a detached process with arguments and a working directory", "[macos][diff][integration][launcher]")
{
    test::temporary_directory scratch { "macos-launcher" };
    const std::unique_ptr<diff_tool_launcher> launcher { win32::make_diff_tool_launcher() };

    // 이름만 적은 `sh`를 찾아 띄운다. 인자 하나에 든 공백이 그대로 가는지, 작업 폴더가 맞는지 본다.
    const std::filesystem::path output { scratch.path() / "launched file.txt" };
    tool_launch_request request {};
    request.executable = u8"sh";
    request.working_directory = path_text(scratch.path());
    request.arguments = { u8"-c", u8"pwd > \"$1\"", u8"sh", path_text(output) };
    const tool_launch_result result { launcher->launch(request) };
    REQUIRE(result.launched);

    for (int attempt { 0 }; attempt < 100 && std::filesystem::exists(output) == false; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
    REQUIRE(std::filesystem::exists(output));
    std::string written { test::read_file(output) };
    while (written.empty() == false && written.back() == '\n')
        written.pop_back();
    CHECK(std::filesystem::path { written } == scratch.path());

    tool_launch_request missing {};
    missing.executable = u8"no-such-tool-for-cyella";
    CHECK_FALSE(launcher->launch(missing).launched);
}

TEST_CASE("file urls of posix paths start with three slashes and escape what a browser would", "[macos][diff]")
{
    CHECK(make_file_url(u8"/Users/me/Library/Caches/cyella/left/a.png") == "file:///Users/me/Library/Caches/cyella/left/a.png");
    CHECK(make_file_url(u8"/tmp/a b#c%.png") == "file:///tmp/a%20b%23c%25.png");
    CHECK(make_file_url(u8"/tmp/한글.png") == "file:///tmp/%ED%95%9C%EA%B8%80.png");
}

TEST_CASE("gitdir pointers of posix worktrees and submodules are resolved", "[macos][repository_watch]")
{
    CHECK(resolve_gitdir_pointer(u8"gitdir: /Users/me/main/.git/worktrees/wt\n", u8"/Users/me/wt") == std::optional<std::u8string> { u8"/Users/me/main/.git/worktrees/wt" });
    CHECK(resolve_gitdir_pointer(u8"gitdir: ../.git/modules/sub\n", u8"/Users/me/super/sub") == std::optional<std::u8string> { u8"/Users/me/super/sub/../.git/modules/sub" });
    CHECK_FALSE(resolve_gitdir_pointer(u8"ref: refs/heads/main", u8"/Users/me/x").has_value());
    CHECK(resolve_commondir_pointer(u8"../..\n", u8"/Users/me/main/.git/worktrees/wt") == std::optional<std::u8string> { u8"/Users/me/main/.git/worktrees/wt/../.." });
}

TEST_CASE("git candidates follow PATH and then the Homebrew prefixes", "[macos][git_tool]")
{
    git_tool_environment environment {};
    environment.path_environment = u8"/usr/bin:relative:/opt/homebrew/bin";
    environment.program_files_directories = { u8"/opt/homebrew", u8"/usr/local" };
    const std::vector<git_tool_candidate> candidates { git_tool_candidates(u8"", environment) };
    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0].executable == u8"/usr/bin/git");
    CHECK(candidates[1].executable == u8"/opt/homebrew/bin/git");
    CHECK(candidates[2].executable == u8"/usr/local/bin/git");
    CHECK(executable_directory(u8"/usr/bin/git") == u8"/usr/bin");
    CHECK(executable_directory(u8"/git") == u8"/");
}

TEST_CASE("the default open-with apps are Finder and VS Code through open", "[macos][external_diff]")
{
    const std::vector<external_file_app> apps { repository_open_apps({}) };
    REQUIRE(apps.size() == 2);
    CHECK(apps[0].executable == u8"/usr/bin/open");
    CHECK(apps[0].arguments == u8"{repository}");
    CHECK(apps[1].arguments == u8"-b com.microsoft.VSCode {repository}");
    CHECK(default_external_file_apps()[0].arguments == u8"-R {file}");
}
