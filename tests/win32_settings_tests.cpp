#include "application/app_settings_service.h"
#include "infrastructure/json_app_settings_store.h"
#include "platform/win32/win32_app_settings_path.h"
#include "platform/win32/win32_settings_file_system.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace cyella;

namespace {
    // 구분자가 `\`인 UTF-8 경로 문자열이다.
    std::u8string path_text(const std::filesystem::path& path)
    {
        return path.u8string();
    }
} // namespace

TEST_CASE("settings path is composed under the roaming app data folder", "[win32_settings]")
{
    CHECK(win32::app_settings_file_path_under(u8"C:\\Users\\tester\\AppData\\Roaming") == u8"C:\\Users\\tester\\AppData\\Roaming\\cyella\\settings.json");
    CHECK(win32::app_settings_file_path_under(u8"C:\\Users\\한글\\AppData\\Roaming\\") == u8"C:\\Users\\한글\\AppData\\Roaming\\cyella\\settings.json");
    CHECK(win32::app_settings_file_path_under(u8"") == u8"");
}

TEST_CASE("the real settings path is an existing folder under the roaming profile", "[win32_settings]")
{
    const std::u8string path { win32::app_settings_file_path() };

    REQUIRE_FALSE(path.empty());
    const std::filesystem::path file { path };
    CHECK(file.filename() == "settings.json");
    CHECK(file.parent_path().filename() == "cyella");
    CHECK(std::filesystem::is_directory(file.parent_path()));
    CHECK(file.is_absolute());
}

TEST_CASE("atomic commit creates missing folders and replaces content", "[win32_settings]")
{
    test::temporary_directory directory { "settings-commit" };
    const auto file_system { win32::make_settings_file_system() };
    const std::filesystem::path target { directory.path() / "nested" / "deeper" / "settings.json" };
    const std::u8string target_text { path_text(target) };

    // 아직 없다.
    CHECK(file_system->read(target_text).state == settings_file_read_state::not_found);

    REQUIRE(file_system->atomic_commit(target_text, u8"first\r\n").succeeded());
    const settings_file_read_result first { file_system->read(target_text) };
    REQUIRE(first.state == settings_file_read_state::available);
    CHECK(first.bytes == u8"first\r\n");

    REQUIRE(file_system->atomic_commit(target_text, u8"둘째 내용 with 한글\r\n").succeeded());
    CHECK(file_system->read(target_text).bytes == u8"둘째 내용 with 한글\r\n");

    // 임시 파일이 남지 않는다.
    std::size_t entries { 0 };
    for (const auto& entry : std::filesystem::directory_iterator { target.parent_path() })
    {
        static_cast<void>(entry);
        ++entries;
    }
    CHECK(entries == 1);
}

TEST_CASE("atomic commit of an empty file works", "[win32_settings]")
{
    test::temporary_directory directory { "settings-empty" };
    const auto file_system { win32::make_settings_file_system() };
    const std::u8string target { path_text(directory.path() / "empty.json") };

    REQUIRE(file_system->atomic_commit(target, u8"").succeeded());
    const settings_file_read_result read { file_system->read(target) };
    CHECK(read.state == settings_file_read_state::available);
    CHECK(read.bytes.empty());
}

TEST_CASE("a failed replace reports the replace step and leaves the target alone", "[win32_settings]")
{
    test::temporary_directory directory { "settings-replace-fail" };
    const auto file_system { win32::make_settings_file_system() };
    // 도착 이름이 폴더이면 교체가 실패한다.
    const std::filesystem::path blocked { directory.path() / "settings.json" };
    std::filesystem::create_directories(blocked);

    const settings_file_commit_result result { file_system->atomic_commit(path_text(blocked), u8"x") };

    CHECK_FALSE(result.succeeded());
    CHECK(result.failure == settings_file_commit_failure::replace);
    CHECK(result.native_error.has_value());
    CHECK(std::filesystem::is_directory(blocked));
    std::size_t entries { 0 };
    for (const auto& entry : std::filesystem::directory_iterator { directory.path() })
    {
        static_cast<void>(entry);
        ++entries;
    }
    // 실패한 시도의 임시 파일이 남지 않는다.
    CHECK(entries == 1);
}

TEST_CASE("commit fails cleanly when the folder cannot be created", "[win32_settings]")
{
    test::temporary_directory directory { "settings-blocked-folder" };
    const auto file_system { win32::make_settings_file_system() };
    // 폴더 자리에 파일이 있다.
    test::write_file(directory.path() / "cyella", "i am a file");

    const settings_file_commit_result result { file_system->atomic_commit(path_text(directory.path() / "cyella" / "settings.json"), u8"x") };

    CHECK_FALSE(result.succeeded());
    CHECK(result.native_error.has_value());
}

TEST_CASE("rename moves a file and refuses to overwrite", "[win32_settings]")
{
    test::temporary_directory directory { "settings-rename" };
    const auto file_system { win32::make_settings_file_system() };
    const std::filesystem::path from { directory.path() / "settings.json" };
    const std::filesystem::path to { directory.path() / "settings.json.bad-20261020T093012Z" };
    test::write_file(from, "broken");
    test::write_file(to, "older backup");

    const settings_file_rename_result blocked { file_system->rename(path_text(from), path_text(to)) };
    CHECK_FALSE(blocked.succeeded);
    CHECK(blocked.native_error.has_value());
    CHECK(test::read_file(to) == "older backup");
    CHECK(test::read_file(from) == "broken");

    std::filesystem::remove(to);
    CHECK(file_system->rename(path_text(from), path_text(to)).succeeded);
    CHECK(test::read_file(to) == "broken");
    CHECK_FALSE(std::filesystem::exists(from));

    CHECK_FALSE(file_system->rename(path_text(from), path_text(directory.path() / "x")).succeeded);
}

TEST_CASE("a long path beyond MAX_PATH can be written and read", "[win32_settings]")
{
    test::temporary_directory directory { "settings-long" };
    const auto file_system { win32::make_settings_file_system() };
    std::filesystem::path deep { directory.path() };
    for (int level = 0; level < 12; ++level)
        deep /= std::string(30, 'd');
    const std::u8string target { path_text(deep / "settings.json") };
    REQUIRE(target.size() > 260);

    REQUIRE(file_system->atomic_commit(target, u8"long").succeeded());
    CHECK(file_system->read(target).bytes == u8"long");
}

TEST_CASE("the store works end to end on the real file system", "[win32_settings]")
{
    test::temporary_directory directory { "settings-store" };
    const auto file_system { win32::make_settings_file_system() };
    json_app_settings_store store { *file_system };
    const std::filesystem::path file { directory.path() / "cyella" / "settings.json" };
    const std::u8string file_text { path_text(file) };

    {
        app_settings_service service { store, file_text };
        CHECK(service.load().empty());
        const auto change = [](app_settings& settings) {
            touch_recent_repository(settings, u8"D:\\작업\\저장소", u8"2026-10-20T00:00:00Z");
            settings.open_tabs = { u8"D:\\작업\\저장소" };
            settings.active_tab = u8"D:\\작업\\저장소";
            settings.appearance.theme = theme_preference::dark;
        };
        CHECK(service.update(change).empty());
    }

    app_settings_service restarted { store, file_text };
    CHECK(restarted.load().empty());
    REQUIRE(restarted.settings().recent_repositories.size() == 1);
    CHECK(restarted.settings().recent_repositories[0].display_name == u8"저장소");
    CHECK(restarted.settings().open_tabs == std::vector<std::u8string> { u8"D:\\작업\\저장소" });
    CHECK(restarted.settings().appearance.theme == theme_preference::dark);

    // 파일을 깨뜨리고 다시 시작한다.
    test::write_file(file, "{ this is not json");
    app_settings_service after_corruption { store, file_text };
    const std::vector<diagnostic> diagnostics { after_corruption.load() };
    CHECK(diagnostics.size() == 1);
    CHECK(after_corruption.settings() == app_settings {});

    std::size_t backups { 0 };
    for (const auto& entry : std::filesystem::directory_iterator { file.parent_path() })
        if (entry.path().filename().string().starts_with("settings.json.bad-"))
        {
            ++backups;
            CHECK(test::read_file(entry.path()) == "{ this is not json");
        }
    CHECK(backups == 1);
    CHECK_FALSE(std::filesystem::exists(file));

    // 깨진 뒤 첫 저장이 새 파일을 만든다.
    CHECK(after_corruption.update([](app_settings& settings) { settings.git_executable = u8"C:\\git\\git.exe"; }).empty());
    CHECK(std::filesystem::exists(file));
}
