#include "infrastructure/json_window_placement.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>

namespace cyella {
    namespace {
        constexpr std::u8string_view file_name { u8"window-placement.json" };

        std::optional<int> read_int(const nlohmann::json& value, const char* const key)
        {
            const auto found { value.find(key) };
            if (found == value.end() || found->is_number_integer() == false)
                return std::nullopt;
            // 부호 없는 큰 수도 is_number_integer라 int로 줄이기 전에 범위를 본다.
            if (found->is_number_unsigned())
            {
                const auto number { found->get<std::uint64_t>() };
                if (number > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                    return std::nullopt;
                return static_cast<int>(number);
            }
            const auto number { found->get<std::int64_t>() };
            if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
                return std::nullopt;
            return static_cast<int>(number);
        }
    } // namespace

    std::u8string window_placement_path(const std::u8string_view settings_path)
    {
        const std::size_t slash { settings_path.find_last_of(u8"\\/") };
        std::u8string path { slash == std::u8string_view::npos ? std::u8string {} : std::u8string { settings_path.substr(0, slash + 1) } };
        path.append(file_name);
        return path;
    }

    std::optional<saved_window_placement> parse_window_placement_json(const std::u8string_view bytes)
    {
        const nlohmann::json value { nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false) };
        if (value.is_object() == false)
            return std::nullopt;
        const std::optional<int> x { read_int(value, "x") };
        const std::optional<int> y { read_int(value, "y") };
        const std::optional<int> width { read_int(value, "width") };
        const std::optional<int> height { read_int(value, "height") };
        if (x.has_value() == false || y.has_value() == false || width.has_value() == false || height.has_value() == false)
            return std::nullopt;
        const auto maximized { value.find("maximized") };
        const saved_window_placement placement { .x = *x, .y = *y, .width = *width, .height = *height, .maximized = maximized != value.end() && maximized->is_boolean() && maximized->get<bool>() };
        if (placement.valid() == false)
            return std::nullopt;
        return placement;
    }

    std::u8string window_placement_json(const saved_window_placement& placement)
    {
        const nlohmann::ordered_json value { { "x", placement.x }, { "y", placement.y }, { "width", placement.width }, { "height", placement.height }, { "maximized", placement.maximized } };
        const std::string text { value.dump(4) + "\n" };
        return std::u8string { text.begin(), text.end() };
    }

    std::optional<saved_window_placement> load_window_placement(settings_file_system& files, const std::u8string_view settings_path)
    {
        if (settings_path.empty())
            return std::nullopt;
        const settings_file_read_result read { files.read(window_placement_path(settings_path)) };
        if (read.state != settings_file_read_state::available)
            return std::nullopt;
        return parse_window_placement_json(read.bytes);
    }

    bool save_window_placement(settings_file_system& files, const std::u8string_view settings_path, const saved_window_placement& placement)
    {
        if (settings_path.empty() || placement.valid() == false)
            return false;
        return files.atomic_commit(window_placement_path(settings_path), window_placement_json(placement)).succeeded();
    }
} // namespace cyella
