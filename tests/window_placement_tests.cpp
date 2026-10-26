#include "infrastructure/json_window_placement.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

using namespace cyella;

namespace {
    // 메모리에 파일을 두는 가짜다.
    class memory_file_system final : public settings_file_system
    {
    public:
        [[nodiscard]] settings_file_read_result read(const std::u8string_view path) noexcept override
        {
            const auto found { files.find(std::u8string { path }) };
            if (found == files.end())
                return { settings_file_read_state::not_found, {}, 2u };
            return { settings_file_read_state::available, found->second, std::nullopt };
        }

        [[nodiscard]] settings_file_commit_result atomic_commit(const std::u8string_view path, const std::u8string_view bytes) noexcept override
        {
            ++commit_count;
            files[std::u8string { path }] = std::u8string { bytes };
            return {};
        }

        [[nodiscard]] settings_file_rename_result rename(const std::u8string_view, const std::u8string_view) noexcept override
        {
            return { false, 183u };
        }

        std::map<std::u8string, std::u8string> files {};
        int commit_count { 0 };
    };
} // namespace

TEST_CASE("the window placement file sits next to the settings file", "[settings][window]")
{
    CHECK(window_placement_path(u8"C:\\Users\\me\\AppData\\Roaming\\cyella\\settings.json") == u8"C:\\Users\\me\\AppData\\Roaming\\cyella\\window-placement.json");
    CHECK(window_placement_path(u8"D:/scratch/settings.json") == u8"D:/scratch/window-placement.json");
    CHECK(window_placement_path(u8"settings.json") == u8"window-placement.json");
}

TEST_CASE("a saved window placement round-trips including negative coordinates and maximized", "[settings][window]")
{
    memory_file_system files {};
    const std::u8string settings { u8"C:\\cyella\\settings.json" };
    CHECK_FALSE(load_window_placement(files, settings).has_value());

    const saved_window_placement placement { .x = -1600, .y = 40, .width = 1400, .height = 900, .maximized = true };
    REQUIRE(save_window_placement(files, settings, placement));
    CHECK(files.files.contains(u8"C:\\cyella\\window-placement.json"));
    const std::optional<saved_window_placement> loaded { load_window_placement(files, settings) };
    REQUIRE(loaded.has_value());
    CHECK(*loaded == placement);

    // 설정 경로가 없으면(test·메모리 설정) 읽지도 쓰지도 않는다.
    CHECK_FALSE(save_window_placement(files, {}, placement));
    CHECK_FALSE(load_window_placement(files, {}).has_value());
    CHECK(files.commit_count == 1);
}

TEST_CASE("a broken or out-of-range window placement file is ignored", "[settings][window]")
{
    CHECK_FALSE(parse_window_placement_json(u8"").has_value());
    CHECK_FALSE(parse_window_placement_json(u8"{ not json").has_value());
    CHECK_FALSE(parse_window_placement_json(u8"[1, 2, 3, 4]").has_value());
    CHECK_FALSE(parse_window_placement_json(u8R"({ "x": 0, "y": 0, "width": 800 })").has_value());
    CHECK_FALSE(parse_window_placement_json(u8R"({ "x": 0, "y": 0, "width": 0, "height": 600 })").has_value());
    CHECK_FALSE(parse_window_placement_json(u8R"({ "x": 0, "y": 0, "width": -800, "height": 600 })").has_value());
    CHECK_FALSE(parse_window_placement_json(u8R"({ "x": 0.5, "y": 0, "width": 800, "height": 600 })").has_value());
    CHECK_FALSE(parse_window_placement_json(u8R"({ "x": "0", "y": 0, "width": 800, "height": 600 })").has_value());
    CHECK_FALSE(parse_window_placement_json(u8R"({ "x": 4294967296, "y": 0, "width": 800, "height": 600 })").has_value());
    CHECK_FALSE(parse_window_placement_json(u8R"({ "x": -4294967296, "y": 0, "width": 800, "height": 600 })").has_value());

    // maximized가 없거나 불리언이 아니면 일반 창이다.
    const std::optional<saved_window_placement> plain { parse_window_placement_json(u8R"({ "x": 10, "y": 20, "width": 800, "height": 600, "maximized": "yes" })") };
    REQUIRE(plain.has_value());
    CHECK(*plain == saved_window_placement { .x = 10, .y = 20, .width = 800, .height = 600, .maximized = false });
}
