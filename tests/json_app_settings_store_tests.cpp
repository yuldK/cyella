#include "application/app_settings_service.h"
#include "infrastructure/json_app_settings_store.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

using namespace cyella;

namespace {
    using json = nlohmann::json;

    std::string text_of(const std::u8string_view value)
    {
        return std::string { reinterpret_cast<const char*>(value.data()), value.size() };
    }

    std::u8string u8_of(const std::string_view value)
    {
        return std::u8string { reinterpret_cast<const char8_t*>(value.data()), value.size() };
    }

    // 메모리에 파일을 두는 가짜다. 실패를 주입할 수 있다.
    class fake_file_system final : public settings_file_system
    {
    public:
        [[nodiscard]] settings_file_read_result read(const std::u8string_view path) noexcept override
        {
            if (fail_reads)
                return { settings_file_read_state::failed, {}, 5u };
            const auto found { files.find(std::u8string { path }) };
            if (found == files.end())
                return { settings_file_read_state::not_found, {}, 2u };
            return { settings_file_read_state::available, found->second, std::nullopt };
        }

        [[nodiscard]] settings_file_commit_result atomic_commit(const std::u8string_view path, const std::u8string_view bytes) noexcept override
        {
            ++commit_count;
            if (fail_commits)
                return { settings_file_commit_failure::replace, 32u };
            files[std::u8string { path }] = std::u8string { bytes };
            return {};
        }

        [[nodiscard]] settings_file_rename_result rename(const std::u8string_view from, const std::u8string_view to) noexcept override
        {
            renames.emplace_back(std::u8string { from }, std::u8string { to });
            if (fail_renames || files.contains(std::u8string { to }) || files.contains(std::u8string { from }) == false)
                return { false, 183u };
            files[std::u8string { to }] = std::move(files[std::u8string { from }]);
            files.erase(std::u8string { from });
            return { true, std::nullopt };
        }

        std::map<std::u8string, std::u8string> files {};
        std::vector<std::pair<std::u8string, std::u8string>> renames {};
        std::size_t commit_count { 0 };
        bool fail_reads { false };
        bool fail_commits { false };
        bool fail_renames { false };
    };

    const std::u8string settings_path { u8"C:\\Users\\tester\\AppData\\Roaming\\cyella\\settings.json" };

    // 2026-10-20 09:30:12 UTC
    json_app_settings_store::clock_function fixed_clock()
    {
        return [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1792488612 } }; };
    }

    app_settings filled_settings()
    {
        app_settings settings {};
        settings.recent_repositories.push_back({ u8"C:\\work\\고정 저장소", u8"고정 저장소", u8"2026-10-19T10:00:00Z", true });
        settings.recent_repositories.push_back({ u8"C:\\work\\alpha", u8"alpha", u8"2026-10-20T08:00:00Z", false });
        settings.recent_repositories.push_back({ u8"D:\\beta", u8"beta", u8"2026-10-18T08:00:00Z", false });
        settings.open_tabs = { u8"C:\\work\\alpha", u8"D:\\beta" };
        settings.active_tab = u8"D:\\beta";
        settings.git_executable = u8"C:\\Program Files\\Git\\cmd\\git.exe";
        external_diff_tool tool {};
        tool.extensions = { u8".png", u8".tar.gz" };
        tool.executable = u8"C:\\tools\\imgdiff.exe";
        tool.arguments = u8"--left \"{left}\" --right \"{right}\"";
        settings.external_diff_tools.push_back(tool);
        settings.appearance.theme = theme_preference::dark;
        settings.window = window_placement { -1920, 40, 1600, 900, true };
        return settings;
    }
} // namespace

TEST_CASE("settings survive a serialize and parse round trip", "[json_app_settings]")
{
    const app_settings original { filled_settings() };

    const std::u8string bytes { serialize_app_settings_json(original, u8"") };
    const app_settings_load_result loaded { parse_app_settings_json(bytes) };

    CHECK_FALSE(loaded.corrupt);
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings == original);
    CHECK(loaded.shadow_source_json == bytes);
}

TEST_CASE("default settings round trip and write every known key", "[json_app_settings]")
{
    const std::u8string bytes { serialize_app_settings_json(app_settings {}, u8"") };
    const json document { json::parse(text_of(bytes)) };

    CHECK(document.at("schema_version") == 1);
    CHECK(document.at("recent_repositories").is_array());
    CHECK(document.at("open_tabs").is_array());
    CHECK(document.at("active_tab") == "");
    CHECK(document.at("git_executable") == "");
    CHECK(document.at("external_diff_tools").is_array());
    CHECK(document.at("appearance").at("theme") == "system");
    // Gravatar는 메일 해시를 밖으로 보내므로 기본이 꺼짐이다.
    CHECK(document.at("appearance").at("gravatar") == false);
    CHECK_FALSE(document.contains("window"));
    CHECK(parse_app_settings_json(bytes).settings == app_settings {});
}

TEST_CASE("serialized settings use CRLF and keep Korean text readable", "[json_app_settings]")
{
    const std::u8string bytes { serialize_app_settings_json(filled_settings(), u8"") };
    CHECK(bytes.ends_with(u8"\r\n"));
    CHECK(bytes.find(u8"고정 저장소") != std::u8string::npos);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        if (bytes[index] == u8'\n')
            REQUIRE((index > 0 && bytes[index - 1] == u8'\r'));
}

TEST_CASE("pinned is written only when true", "[json_app_settings]")
{
    const json document { json::parse(text_of(serialize_app_settings_json(filled_settings(), u8""))) };
    const json& recent { document.at("recent_repositories") };
    CHECK(recent.at(0).at("pinned") == true);
    CHECK_FALSE(recent.at(1).contains("pinned"));
}

TEST_CASE("unknown keys survive a load and save cycle", "[json_app_settings]")
{
    const std::u8string source {
        u8R"({
        "schema_version": 1,
        "future_feature": { "enabled": true, "list": [1, 2, 3] },
        "appearance": { "theme": "light", "accent": "teal" },
        "window": { "x": 1, "y": 2, "width": 300, "height": 200, "maximized": false, "dpi": 144 },
        "recent_repositories": []
    })",
    };

    app_settings_load_result loaded { parse_app_settings_json(source) };
    REQUIRE_FALSE(loaded.corrupt);
    loaded.settings.appearance.theme = theme_preference::dark;
    loaded.settings.window->width = 640;

    const json saved { json::parse(text_of(serialize_app_settings_json(loaded.settings, loaded.shadow_source_json))) };

    CHECK(saved.at("future_feature").at("enabled") == true);
    CHECK(saved.at("future_feature").at("list").size() == 3);
    // 객체 안의 모르는 키도 남고 아는 값은 새 값이다.
    CHECK(saved.at("appearance").at("accent") == "teal");
    CHECK(saved.at("appearance").at("theme") == "dark");
    CHECK(saved.at("window").at("dpi") == 144);
    CHECK(saved.at("window").at("width") == 640);
}

TEST_CASE("unknown keys survive through the store on disk", "[json_app_settings]")
{
    fake_file_system file_system {};
    file_system.files[settings_path] = u8"{\"schema_version\":1,\"keep_me\":\"yes\",\"open_tabs\":[\"C:\\\\a\"]}";
    json_app_settings_store store { file_system, fixed_clock() };

    app_settings_load_result loaded { store.load(settings_path) };
    REQUIRE(loaded.settings.open_tabs.size() == 1);
    loaded.settings.git_executable = u8"C:\\git\\git.exe";

    const app_settings_save_result saved { store.save(settings_path, loaded.settings, loaded.shadow_source_json) };
    REQUIRE(saved.succeeded);

    const json document { json::parse(text_of(file_system.files[settings_path])) };
    CHECK(document.at("keep_me") == "yes");
    CHECK(document.at("git_executable") == "C:\\git\\git.exe");
}

TEST_CASE("a shadow that is not an object is ignored", "[json_app_settings]")
{
    const json document { json::parse(text_of(serialize_app_settings_json(filled_settings(), u8"[1, 2"))) };
    CHECK(document.at("schema_version") == 1);
    const json from_array { json::parse(text_of(serialize_app_settings_json(filled_settings(), u8"[1, 2]"))) };
    CHECK(from_array.is_object());
}

TEST_CASE("a missing file gives defaults without diagnostics", "[json_app_settings]")
{
    fake_file_system file_system {};
    json_app_settings_store store { file_system, fixed_clock() };

    const app_settings_load_result loaded { store.load(settings_path) };

    CHECK(loaded.settings == app_settings {});
    CHECK(loaded.diagnostics.empty());
    CHECK_FALSE(loaded.corrupt);
    CHECK_FALSE(loaded.read_failed);
    CHECK(file_system.renames.empty());
}

TEST_CASE("an empty file is not treated as corrupt", "[json_app_settings]")
{
    fake_file_system file_system {};
    file_system.files[settings_path] = u8" \r\n";
    json_app_settings_store store { file_system, fixed_clock() };

    const app_settings_load_result loaded { store.load(settings_path) };

    CHECK(loaded.settings == app_settings {});
    CHECK(loaded.diagnostics.empty());
    CHECK(file_system.renames.empty());
}

TEST_CASE("an unreadable file gives defaults and one diagnostic", "[json_app_settings]")
{
    fake_file_system file_system {};
    file_system.fail_reads = true;
    json_app_settings_store store { file_system, fixed_clock() };

    const app_settings_load_result loaded { store.load(settings_path) };

    CHECK(loaded.settings == app_settings {});
    CHECK(loaded.read_failed);
    CHECK_FALSE(loaded.corrupt);
    REQUIRE(loaded.diagnostics.size() == 1);
    CHECK(loaded.diagnostics[0].code == diagnostic_code::app_settings_read_failed);
    CHECK(loaded.diagnostics[0].native_error == 5u);
    // 읽지 못한 파일은 옮기지도 않는다.
    CHECK(file_system.renames.empty());
}

TEST_CASE("a corrupt file is backed up and replaced with defaults", "[json_app_settings]")
{
    const std::vector<std::u8string> corrupt_contents {
        u8"{ \"recent_repositories\": [ ",
        u8"not json at all",
        u8"[1, 2, 3]",
        u8"\"just a string\"",
        std::u8string { u8"{\"open_tabs\":[\"" } + std::u8string { 0xFF, 0xFE } + u8"\"]}",
    };

    for (const std::u8string& contents : corrupt_contents)
    {
        fake_file_system file_system {};
        file_system.files[settings_path] = contents;
        json_app_settings_store store { file_system, fixed_clock() };

        const app_settings_load_result loaded { store.load(settings_path) };

        CHECK(loaded.corrupt);
        CHECK(loaded.settings == app_settings {});
        CHECK(loaded.shadow_source_json.empty());
        REQUIRE(loaded.diagnostics.size() == 1);
        CHECK(loaded.diagnostics[0].severity == diagnostic_severity::warning);

        const std::u8string backup { settings_path + u8".bad-20261020T093012Z" };
        CHECK(file_system.files.contains(backup));
        CHECK(file_system.files.at(backup) == contents);
        CHECK_FALSE(file_system.files.contains(settings_path));
    }
}

TEST_CASE("backup name does not overwrite an earlier backup", "[json_app_settings]")
{
    fake_file_system file_system {};
    const std::u8string first_backup { settings_path + u8".bad-20261020T093012Z" };
    file_system.files[first_backup] = u8"older backup";
    file_system.files[settings_path] = u8"{ broken";
    json_app_settings_store store { file_system, fixed_clock() };

    const app_settings_load_result loaded { store.load(settings_path) };

    CHECK(loaded.corrupt);
    CHECK(file_system.files.at(first_backup) == u8"older backup");
    CHECK(file_system.files.at(settings_path + u8".bad-20261020T093012Z-1") == u8"{ broken");
}

TEST_CASE("a failed backup is reported but loading still gives defaults", "[json_app_settings]")
{
    fake_file_system file_system {};
    file_system.files[settings_path] = u8"{ broken";
    file_system.fail_renames = true;
    json_app_settings_store store { file_system, fixed_clock() };

    const app_settings_load_result loaded { store.load(settings_path) };

    CHECK(loaded.corrupt);
    CHECK(loaded.settings == app_settings {});
    REQUIRE(loaded.diagnostics.size() == 1);
    CHECK(loaded.diagnostics[0].code == diagnostic_code::app_settings_backup_failed);
}

TEST_CASE("a UTF-8 BOM before the JSON is accepted", "[json_app_settings]")
{
    const std::u8string source { std::u8string { 0xEF, 0xBB, 0xBF } + u8"{\"git_executable\":\"C:\\\\git\\\\git.exe\"}" };
    const app_settings_load_result loaded { parse_app_settings_json(source) };
    CHECK_FALSE(loaded.corrupt);
    CHECK(loaded.settings.git_executable == u8"C:\\git\\git.exe");
}

TEST_CASE("schema version handling", "[json_app_settings]")
{
    SECTION("버전이 없으면 현재 버전이고 경고가 없다")
    {
        const app_settings_load_result loaded { parse_app_settings_json(u8"{}") };
        CHECK(loaded.settings.schema_version == current_app_settings_schema_version);
        CHECK(loaded.diagnostics.empty());
    }

    SECTION("현재보다 낮은 값은 현재 버전으로 읽는다")
    {
        const app_settings_load_result loaded { parse_app_settings_json(u8"{\"schema_version\":0}") };
        CHECK(loaded.settings.schema_version == current_app_settings_schema_version);
        CHECK(loaded.diagnostics.empty());
    }

    SECTION("정수가 아니면 경고하고 현재 버전으로 읽는다")
    {
        const app_settings_load_result loaded { parse_app_settings_json(u8"{\"schema_version\":\"one\"}") };
        CHECK(loaded.settings.schema_version == current_app_settings_schema_version);
        CHECK(loaded.diagnostics.size() == 1);
    }

    SECTION("더 새 버전은 경고하고 아는 필드를 읽으며 번호를 지킨다")
    {
        const std::u8string source { u8"{\"schema_version\":7,\"git_executable\":\"C:\\\\git\\\\git.exe\",\"brand_new\":{\"a\":1}}" };
        const app_settings_load_result loaded { parse_app_settings_json(source) };

        CHECK_FALSE(loaded.corrupt);
        REQUIRE(loaded.diagnostics.size() == 1);
        CHECK(loaded.diagnostics[0].severity == diagnostic_severity::warning);
        CHECK(loaded.settings.schema_version == 7);
        CHECK(loaded.settings.git_executable == u8"C:\\git\\git.exe");

        const json saved { json::parse(text_of(serialize_app_settings_json(loaded.settings, loaded.shadow_source_json))) };
        // 번호를 낮추지 않고 모르는 키도 남긴다.
        CHECK(saved.at("schema_version") == 7);
        CHECK(saved.at("brand_new").at("a") == 1);
    }

    SECTION("새로 쓰는 파일은 현재 버전이다")
    {
        const json saved { json::parse(text_of(serialize_app_settings_json(app_settings {}, u8""))) };
        CHECK(saved.at("schema_version") == current_app_settings_schema_version);
    }
}

TEST_CASE("wrong field types fall back per field with warnings", "[json_app_settings]")
{
    const std::u8string source {
        u8R"({
        "recent_repositories": "nope",
        "open_tabs": { "a": 1 },
        "active_tab": 5,
        "git_executable": "git.exe",
        "external_diff_tools": [ { "extensions": [], "executable": "x.exe" }, 7, { "extensions": [".psd"], "executable": "" } ],
        "appearance": { "theme": "purple" },
        "window": { "x": 0, "y": 0, "width": 0, "height": 10 }
    })",
    };

    const app_settings_load_result loaded { parse_app_settings_json(source) };

    CHECK_FALSE(loaded.corrupt);
    CHECK(loaded.settings == app_settings {});
    // 필드마다 경고 하나다: 최근 목록, open_tabs, 상대 git 경로, 도구 묶음, 테마, 창 배치.
    CHECK(loaded.diagnostics.size() == 6);
    for (const diagnostic& value : loaded.diagnostics)
        CHECK(value.code == diagnostic_code::app_settings_invalid);
}

TEST_CASE("valid entries are kept next to broken ones", "[json_app_settings]")
{
    const std::u8string source {
        u8R"({
        "recent_repositories": [ 5, { "display_name": "no path" }, { "path": "C:/work/ok/", "opened_at": "2026-10-20T00:00:00Z" } ],
        "open_tabs": [ "C:/work/ok/", "", 7 ],
        "external_diff_tools": [ { "extensions": ["PNG", 3, "", "*.TAR.GZ"], "executable": "x.exe", "arguments": "{left} {right}" } ]
    })",
    };

    const app_settings_load_result loaded { parse_app_settings_json(source) };

    REQUIRE(loaded.settings.recent_repositories.size() == 1);
    CHECK(loaded.settings.recent_repositories[0].path == u8"C:\\work\\ok");
    CHECK(loaded.settings.recent_repositories[0].display_name == u8"ok");
    CHECK(loaded.settings.open_tabs == std::vector<std::u8string> { u8"C:\\work\\ok" });
    REQUIRE(loaded.settings.external_diff_tools.size() == 1);
    CHECK(loaded.settings.external_diff_tools[0].extensions == std::vector<std::u8string> { u8".png", u8".tar.gz" });
    CHECK(loaded.settings.external_diff_tools[0].arguments == u8"{left} {right}");
    CHECK(loaded.diagnostics.empty());
}

TEST_CASE("loading keeps pinned entries even beyond the cap and restores the invariants", "[json_app_settings]")
{
    json document { json::object() };
    json recent { json::array() };
    // 비고정 25개 뒤에 고정 하나가 있는 손으로 고친 파일이다.
    for (int number = 0; number < 25; ++number)
        recent.push_back({ { "path", "C:\\work\\repo" + std::to_string(number) } });
    recent.push_back({ { "path", "C:\\work\\pinned" }, { "pinned", true } });
    document["recent_repositories"] = recent;

    const app_settings_load_result loaded { parse_app_settings_json(u8_of(document.dump())) };

    REQUIRE(loaded.settings.recent_repositories.size() == recent_repository_capacity + 1);
    CHECK(loaded.settings.recent_repositories.front().path == u8"C:\\work\\pinned");
    CHECK(loaded.settings.recent_repositories.front().pinned);
    CHECK(loaded.settings.recent_repositories[1].path == u8"C:\\work\\repo0");
}

TEST_CASE("a failed save reports exactly one diagnostic", "[json_app_settings]")
{
    fake_file_system file_system {};
    file_system.fail_commits = true;
    json_app_settings_store store { file_system, fixed_clock() };

    const app_settings_save_result saved { store.save(settings_path, filled_settings(), u8"") };

    CHECK_FALSE(saved.succeeded);
    REQUIRE(saved.diagnostics.size() == 1);
    CHECK(saved.diagnostics[0].code == diagnostic_code::app_settings_write_failed);
    CHECK(saved.diagnostics[0].native_error == 32u);
    CHECK(file_system.files.empty());
}

TEST_CASE("the service keeps memory state when saving fails and retries later", "[json_app_settings]")
{
    fake_file_system file_system {};
    json_app_settings_store store { file_system, fixed_clock() };
    app_settings_service service { store, settings_path };
    CHECK(service.load().empty());

    file_system.fail_commits = true;
    const std::vector<diagnostic> failed { service.update([](app_settings& settings) { touch_recent_repository(settings, u8"C:\\work\\alpha", u8"2026-10-20T00:00:00Z"); }) };

    REQUIRE(failed.size() == 1);
    CHECK(failed[0].code == diagnostic_code::app_settings_write_failed);
    CHECK(service.has_unsaved_changes());
    // 저장이 실패해도 방금 한 일은 메모리에 남아 있다.
    REQUIRE(service.settings().recent_repositories.size() == 1);
    CHECK(service.settings().recent_repositories[0].path == u8"C:\\work\\alpha");
    CHECK(file_system.files.empty());

    file_system.fail_commits = false;
    const std::vector<diagnostic> recovered { service.update([](app_settings& settings) { settings.appearance.theme = theme_preference::light; }) };

    CHECK(recovered.empty());
    CHECK_FALSE(service.has_unsaved_changes());
    // 다음 저장이 그동안 밀린 변경까지 함께 쓴다.
    const app_settings_load_result reloaded { parse_app_settings_json(file_system.files.at(settings_path)) };
    CHECK(reloaded.settings == service.settings());
    CHECK(reloaded.settings.recent_repositories.size() == 1);
}

TEST_CASE("the service round trips through a store and keeps unknown keys across saves", "[json_app_settings]")
{
    fake_file_system file_system {};
    file_system.files[settings_path] = u8"{\"from_future\":42}";
    json_app_settings_store store { file_system, fixed_clock() };

    {
        app_settings_service service { store, settings_path };
        static_cast<void>(service.load());
        static_cast<void>(service.update([](app_settings& settings) { settings = filled_settings(); }));
        static_cast<void>(service.update([](app_settings& settings) { settings.active_tab = u8"C:\\work\\alpha"; }));
    }

    app_settings_service restarted { store, settings_path };
    CHECK(restarted.load().empty());
    CHECK(restarted.settings().active_tab == u8"C:\\work\\alpha");
    CHECK(restarted.settings().recent_repositories == filled_settings().recent_repositories);
    CHECK(json::parse(text_of(file_system.files.at(settings_path))).at("from_future") == 42);
}

TEST_CASE("the service never overwrites a file it could not read", "[json_app_settings]")
{
    const std::u8string original { u8"{\"git_executable\":\"C:\\\\Program Files\\\\Git\\\\cmd\\\\git.exe\"}" };
    fake_file_system file_system {};
    file_system.files[settings_path] = original;
    file_system.fail_reads = true;
    json_app_settings_store store { file_system, fixed_clock() };
    app_settings_service service { store, settings_path };

    const std::vector<diagnostic> loaded { service.load() };

    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].code == diagnostic_code::app_settings_read_failed);
    CHECK(service.is_read_only());

    // 잠금이 풀려도 이번 실행에서는 이미 기본값으로 시작했으니 저장하지 않는다.
    file_system.fail_reads = false;
    const std::vector<diagnostic> first { service.update([](app_settings& settings) { touch_recent_repository(settings, u8"C:\\work\\alpha", u8"2026-10-20T00:00:00Z"); }) };
    const std::vector<diagnostic> second { service.save() };

    REQUIRE(first.size() == 1);
    CHECK(first[0].code == diagnostic_code::app_settings_write_failed);
    CHECK(first[0].native_error == 5u);
    REQUIRE(second.size() == 1);
    CHECK(file_system.commit_count == 0);
    CHECK(file_system.renames.empty());
    CHECK(file_system.files.at(settings_path) == original);
    CHECK(service.has_unsaved_changes());
    // 메모리 상태는 평소처럼 바뀐다.
    REQUIRE(service.settings().recent_repositories.size() == 1);
    CHECK(service.settings().recent_repositories[0].path == u8"C:\\work\\alpha");
}

TEST_CASE("loading again after a read failure lifts the read only state", "[json_app_settings]")
{
    fake_file_system file_system {};
    file_system.files[settings_path] = u8"{\"appearance\":{\"theme\":\"dark\"}}";
    file_system.fail_reads = true;
    json_app_settings_store store { file_system, fixed_clock() };
    app_settings_service service { store, settings_path };
    static_cast<void>(service.load());
    REQUIRE(service.is_read_only());

    file_system.fail_reads = false;
    CHECK(service.load().empty());
    CHECK_FALSE(service.is_read_only());
    CHECK(service.settings().appearance.theme == theme_preference::dark);

    CHECK(service.update([](app_settings& settings) { settings.active_tab = u8"C:\\work\\alpha"; }).empty());
    CHECK(file_system.commit_count == 1);
    CHECK_FALSE(service.has_unsaved_changes());
}

TEST_CASE("a missing or corrupt file does not make the service read only", "[json_app_settings]")
{
    SECTION("missing")
    {
        fake_file_system file_system {};
        json_app_settings_store store { file_system, fixed_clock() };
        app_settings_service service { store, settings_path };

        CHECK(service.load().empty());
        CHECK_FALSE(service.is_read_only());
        CHECK(service.update([](app_settings& settings) { settings.active_tab = u8"C:\\work\\alpha"; }).empty());
        CHECK(file_system.commit_count == 1);
        CHECK(file_system.files.contains(settings_path));
    }

    SECTION("corrupt")
    {
        fake_file_system file_system {};
        file_system.files[settings_path] = u8"not json at all";
        json_app_settings_store store { file_system, fixed_clock() };
        app_settings_service service { store, settings_path };

        // 깨진 파일은 `.bad-<시각>`으로 옮겨 두었으니 새로 쓰는 것이 안전하다.
        REQUIRE(service.load().size() == 1);
        CHECK_FALSE(service.is_read_only());
        CHECK(service.update([](app_settings& settings) { settings.active_tab = u8"C:\\work\\alpha"; }).empty());
        CHECK(file_system.commit_count == 1);
        CHECK(file_system.files.contains(settings_path + u8".bad-20261020T093012Z"));
    }
}

TEST_CASE("backup timestamp has no characters that are invalid in file names", "[json_app_settings]")
{
    const std::chrono::system_clock::time_point time { std::chrono::seconds { 1792488612 } };
    CHECK(format_backup_timestamp(time) == u8"20261020T093012Z");
}

TEST_CASE("pane widths are written under ui and absent widths are left out", "[json_app_settings]")
{
    CHECK_FALSE(json::parse(text_of(serialize_app_settings_json(app_settings {}, u8""))).contains("ui"));

    app_settings settings {};
    settings.ui.pane_widths.sidebar = 300.0f;
    const json one { json::parse(text_of(serialize_app_settings_json(settings, u8""))) };
    CHECK(one.at("ui").at("pane_widths").at("sidebar") == 300.0);
    CHECK_FALSE(one.at("ui").at("pane_widths").contains("detail"));

    settings.ui.pane_widths.detail = 412.5f;
    const std::u8string bytes { serialize_app_settings_json(settings, u8"") };
    const app_settings_load_result loaded { parse_app_settings_json(bytes) };
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings.ui.pane_widths.sidebar == 300.0f);
    CHECK(loaded.settings.ui.pane_widths.detail == 412.5f);
    CHECK(loaded.settings == settings);
}

TEST_CASE("the commit files view is written under ui once the tree is chosen", "[json_app_settings]")
{
    app_settings settings {};
    settings.ui.commit_files_tree = true;
    const std::u8string bytes { serialize_app_settings_json(settings, u8"") };
    CHECK(json::parse(text_of(bytes)).at("ui").at("commit_files_view") == "tree");
    const app_settings_load_result loaded { parse_app_settings_json(bytes) };
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings == settings);

    // 목록으로 되돌리면 같은 키에 "list"가 남는다. 처음부터 목록이면 ui를 쓰지 않는다.
    app_settings back { loaded.settings };
    back.ui.commit_files_tree = false;
    const json saved { json::parse(text_of(serialize_app_settings_json(back, loaded.shadow_source_json))) };
    CHECK(saved.at("ui").at("commit_files_view") == "list");
    CHECK_FALSE(parse_app_settings_json(serialize_app_settings_json(back, loaded.shadow_source_json)).settings.ui.commit_files_tree);

    const app_settings_load_result wrong { parse_app_settings_json(u8R"({"ui":{"commit_files_view":"grid"}})") };
    CHECK_FALSE(wrong.settings.ui.commit_files_tree);
    CHECK(wrong.diagnostics.size() == 1);
}

TEST_CASE("the gravatar switch round trips and a wrong value stays off", "[json_app_settings]")
{
    app_settings settings {};
    settings.appearance.gravatar = true;
    const app_settings_load_result loaded { parse_app_settings_json(serialize_app_settings_json(settings, u8"")) };
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings.appearance.gravatar);

    const app_settings_load_result wrong { parse_app_settings_json(u8R"({"appearance":{"theme":"dark","gravatar":"yes"}})") };
    CHECK_FALSE(wrong.settings.appearance.gravatar);
    CHECK(wrong.settings.appearance.theme == theme_preference::dark);
    CHECK(wrong.diagnostics.size() == 1);
}

TEST_CASE("the key color round trips, keeps an unknown id and drops a wrong type", "[json_app_settings]")
{
    CHECK(app_settings {}.appearance.accent.empty());
    app_settings settings {};
    settings.appearance.accent = u8"violet";
    const app_settings_load_result loaded { parse_app_settings_json(serialize_app_settings_json(settings, u8"")) };
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings.appearance.accent == u8"violet");

    // 목록에 없는 id도 남긴다 (다른 버전의 목록에서 고른 값일 수 있다). 표시만 기본 키 컬러다.
    const app_settings_load_result unknown { parse_app_settings_json(u8R"({"appearance":{"accent":"ultramarine"}})") };
    CHECK(unknown.diagnostics.empty());
    CHECK(unknown.settings.appearance.accent == u8"ultramarine");

    const app_settings_load_result wrong { parse_app_settings_json(u8R"({"appearance":{"theme":"dark","accent":3}})") };
    CHECK(wrong.settings.appearance.accent.empty());
    CHECK(wrong.settings.appearance.theme == theme_preference::dark);
    CHECK(wrong.diagnostics.size() == 1);
}

TEST_CASE("the language choice round trips and a wrong value follows the system", "[json_app_settings]")
{
    app_settings settings {};
    CHECK(settings.language == language_preference::system);
    settings.language = language_preference::english;
    const std::u8string bytes { serialize_app_settings_json(settings, u8"") };
    CHECK(json::parse(text_of(bytes)).at("language") == "en");
    const app_settings_load_result loaded { parse_app_settings_json(bytes) };
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings.language == language_preference::english);

    const app_settings_load_result wrong { parse_app_settings_json(u8R"({"language":"fr"})") };
    CHECK(wrong.settings.language == language_preference::system);
    CHECK(wrong.diagnostics.size() == 1);
}

TEST_CASE("a dragged pane width is written with two decimals", "[json_app_settings]")
{
    app_settings settings {};
    settings.ui.pane_widths.sidebar = 237.33334350585938f;
    const std::u8string bytes { serialize_app_settings_json(settings, u8"") };
    CHECK(text_of(bytes).find("237.33\r") != std::string::npos);
    CHECK(text_of(bytes).find("237.333") == std::string::npos);
}

TEST_CASE("unknown keys inside ui survive saving the pane widths", "[json_app_settings]")
{
    const std::u8string source {
        u8R"({"schema_version":1,"ui":{"future_density":"compact","pane_widths":{"sidebar":250,"graph":1}}})",
    };
    app_settings_load_result loaded { parse_app_settings_json(source) };
    REQUIRE(loaded.diagnostics.empty());
    CHECK(loaded.settings.ui.pane_widths.sidebar == 250.0f);
    loaded.settings.ui.pane_widths.detail = 333.0f;

    const json saved { json::parse(text_of(serialize_app_settings_json(loaded.settings, loaded.shadow_source_json))) };
    CHECK(saved.at("ui").at("future_density") == "compact");
    CHECK(saved.at("ui").at("pane_widths").at("graph") == 1);
    CHECK(saved.at("ui").at("pane_widths").at("sidebar") == 250.0);
    CHECK(saved.at("ui").at("pane_widths").at("detail") == 333.0);
}

TEST_CASE("invalid pane widths fall back to the default with a warning", "[json_app_settings]")
{
    const app_settings_load_result negative { parse_app_settings_json(u8R"({"ui":{"pane_widths":{"sidebar":-4,"detail":300}}})") };
    CHECK_FALSE(negative.settings.ui.pane_widths.sidebar.has_value());
    CHECK(negative.settings.ui.pane_widths.detail == 300.0f);
    CHECK(negative.diagnostics.size() == 1);

    const app_settings_load_result text { parse_app_settings_json(u8R"({"ui":{"pane_widths":{"sidebar":"wide"}}})") };
    CHECK_FALSE(text.settings.ui.pane_widths.sidebar.has_value());
    CHECK(text.diagnostics.size() == 1);

    CHECK(parse_app_settings_json(u8R"({"ui":[1]})").diagnostics.size() == 1);
    CHECK(parse_app_settings_json(u8R"({"ui":{"pane_widths":5}})").diagnostics.size() == 1);
    CHECK(parse_app_settings_json(u8R"({"ui":null})").diagnostics.empty());
}
TEST_CASE("diff settings are written under diff and survive a round trip", "[json_app_settings][diff]")
{
    app_settings settings {};
    settings.diff.layout = diff_layout_preference::split;
    settings.diff.context_lines = 0;
    const std::u8string bytes { serialize_app_settings_json(settings, u8"") };
    const json document { json::parse(text_of(bytes)) };
    CHECK(document.at("diff").at("layout") == "split");
    CHECK(document.at("diff").at("context_lines") == 0);

    const app_settings_load_result loaded { parse_app_settings_json(bytes) };
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings.diff == settings.diff);

    // 기본값은 inline과 3줄이다.
    const app_settings_load_result defaults { parse_app_settings_json(serialize_app_settings_json(app_settings {}, u8"")) };
    CHECK(defaults.settings.diff.layout == diff_layout_preference::inline_view);
    CHECK(defaults.settings.diff.context_lines == 3);
}

TEST_CASE("invalid diff settings fall back per field with one warning each and keep the valid field", "[json_app_settings][diff]")
{
    const app_settings_load_result bad_layout { parse_app_settings_json(u8R"({ "diff": { "layout": "sideways", "context_lines": 7 } })") };
    CHECK(bad_layout.settings.diff.layout == diff_layout_preference::inline_view);
    CHECK(bad_layout.settings.diff.context_lines == 7);
    CHECK(bad_layout.diagnostics.size() == 1);

    for (const char8_t* const source :
        { u8R"({ "diff": { "context_lines": -1 } })", u8R"({ "diff": { "context_lines": 26 } })", u8R"({ "diff": { "context_lines": 2.5 } })", u8R"({ "diff": { "context_lines": "3" } })" })
    {
        const app_settings_load_result loaded { parse_app_settings_json(source) };
        CHECK(loaded.settings.diff.context_lines == 3);
        CHECK(loaded.diagnostics.size() == 1);
    }

    const app_settings_load_result not_object { parse_app_settings_json(u8R"({ "diff": [] })") };
    CHECK(not_object.settings.diff == diff_settings {});
    CHECK(not_object.diagnostics.size() == 1);

    // 경계값 25는 받는다.
    CHECK(parse_app_settings_json(u8R"({ "diff": { "context_lines": 25, "layout": "split" } })").settings.diff == diff_settings { diff_layout_preference::split, 25 });
}

TEST_CASE("unknown keys inside diff survive saving", "[json_app_settings][diff]")
{
    const std::u8string source { u8R"({ "diff": { "layout": "inline", "future_option": true } })" };
    app_settings settings { parse_app_settings_json(source).settings };
    settings.diff.layout = diff_layout_preference::split;
    const json document { json::parse(text_of(serialize_app_settings_json(settings, source))) };
    CHECK(document.at("diff").at("layout") == "split");
    CHECK(document.at("diff").at("future_option") == true);
}

TEST_CASE("external file apps have defaults and persist custom entries and explicit removal", "[json_app_settings][file_app]")
{
    CHECK(parse_app_settings_json(u8"{}").settings.external_file_apps == default_external_file_apps());
    CHECK(parse_app_settings_json(u8R"({"external_file_apps":[]})").settings.external_file_apps.empty());
    app_settings settings {};
    settings.external_file_apps = { { u8"내 편집기", u8"C:\\Tools\\editor.exe", u8"--open {file} --cwd {repository}" } };
    const auto loaded { parse_app_settings_json(serialize_app_settings_json(settings, u8"")) };
    CHECK(loaded.settings.external_file_apps == settings.external_file_apps);
    const auto invalid { parse_app_settings_json(u8R"({"external_file_apps":[{},3,{"name":"Test","executable":"test.exe","arguments":"{file}"}]})") };
    CHECK(invalid.settings.external_file_apps.size() == 1);
    CHECK(invalid.diagnostics.size() == 2);
}

TEST_CASE("font families and working file view round trip while unknown settings survive", "[json_app_settings][fonts]")
{
    const std::u8string source { u8R"({"appearance":{"ui_font":"맑은 고딕","code_font":"Font on another PC","future":true},"ui":{"working_files_view":"tree","future":7}})" };
    const app_settings_load_result loaded { parse_app_settings_json(source) };
    CHECK(loaded.settings.appearance.ui_font == u8"맑은 고딕");
    CHECK(loaded.settings.appearance.code_font == u8"Font on another PC");
    CHECK(loaded.settings.ui.working_files_tree);
    const std::u8string saved { serialize_app_settings_json(loaded.settings, source) };
    CHECK(parse_app_settings_json(saved).settings == loaded.settings);
    const json document { json::parse(text_of(saved)) };
    CHECK(document.at("appearance").at("future") == true);
    CHECK(document.at("ui").at("future") == 7);
    CHECK(document.at("ui").at("working_files_view") == "tree");
    const app_settings_load_result invalid { parse_app_settings_json(u8R"({"appearance":{"ui_font":false,"code_font":123}})") };
    CHECK(invalid.settings.appearance.ui_font.empty());
    CHECK(invalid.settings.appearance.code_font.empty());
    CHECK(invalid.diagnostics.size() == 2);
}

TEST_CASE("repository layout defaults to graph and round trips both choices without losing other UI settings", "[json_app_settings][layout]")
{
    CHECK(parse_app_settings_json(u8"{}").settings.ui.graph_layout);
    const std::u8string source { u8R"({"ui":{"repository_layout":"review","working_files_view":"tree","future":7}})" };
    app_settings settings { parse_app_settings_json(source).settings };
    CHECK_FALSE(settings.ui.graph_layout);
    CHECK(settings.ui.working_files_tree);
    CHECK_FALSE(parse_app_settings_json(serialize_app_settings_json(settings, source)).settings.ui.graph_layout);
    settings.ui.graph_layout = true;
    const auto saved { serialize_app_settings_json(settings, source) };
    CHECK(parse_app_settings_json(saved).settings.ui.graph_layout);
    const json document { json::parse(text_of(saved)) };
    CHECK(document.at("ui").at("repository_layout") == "graph");
    CHECK(document.at("ui").at("future") == 7);
    CHECK(document.at("ui").at("working_files_view") == "tree");
    for (const char8_t* const bad : { u8R"({"ui":{"repository_layout":false}})", u8R"({"ui":{"repository_layout":"unknown"}})" })
    {
        const auto loaded { parse_app_settings_json(bad) };
        CHECK(loaded.settings.ui.graph_layout);
        CHECK(loaded.diagnostics.size() == 1);
    }
}

TEST_CASE("the tab shape and tab status spot round trip and fall back on unknown values", "[json_app_settings]")
{
    CHECK(app_settings {}.appearance.tabs == tab_shape::connected);
    CHECK(app_settings {}.appearance.tab_marker == tab_marker_place::badge);
    app_settings settings {};
    settings.appearance.tabs = tab_shape::flat;
    settings.appearance.tab_marker = tab_marker_place::close_slot;
    const app_settings_load_result loaded { parse_app_settings_json(serialize_app_settings_json(settings, u8"")) };
    CHECK(loaded.diagnostics.empty());
    CHECK(loaded.settings.appearance.tabs == tab_shape::flat);
    CHECK(loaded.settings.appearance.tab_marker == tab_marker_place::close_slot);

    const app_settings_load_result wrong { parse_app_settings_json(u8R"({"appearance":{"tabs":"round","tab_marker":3}})") };
    CHECK(wrong.settings.appearance.tabs == tab_shape::connected);
    CHECK(wrong.settings.appearance.tab_marker == tab_marker_place::badge);
    CHECK(wrong.diagnostics.size() == 2);
}
