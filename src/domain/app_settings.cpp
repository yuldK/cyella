#include "domain/app_settings.h"

#include "domain/repository_path.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <optional>
#include <unordered_set>
#include <utility>

namespace cyella {
    namespace {
        // 항목 하나를 자기 무리의 맨 앞에 끼워 넣는다. 고정 항목은 목록 앞쪽에 모여 있으므로
        // 비고정은 고정 무리 **뒤**가 자기 자리의 맨 앞이다.
        void insert_at_group_front(std::vector<recent_repository>& repositories, recent_repository entry)
        {
            std::size_t position { 0 };
            if (entry.pinned == false)
                while (position < repositories.size() && repositories[position].pinned)
                    ++position;
            repositories.insert(repositories.begin() + static_cast<std::ptrdiff_t>(position), std::move(entry));
        }

        // 비고정 항목만 상한에 걸린다. 고정은 사용자가 고른 것이라 버리지 않는다.
        void trim_unpinned(std::vector<recent_repository>& repositories)
        {
            std::size_t unpinned { 0 };
            std::erase_if(repositories, [&unpinned](const recent_repository& value) {
                if (value.pinned)
                    return false;
                ++unpinned;
                return unpinned > recent_repository_capacity;
            });
        }

        // 목록에서 저장소를 떼어 낸다. 있었으면 그 항목을, 없었으면 값이 없다.
        std::optional<recent_repository> detach(std::vector<recent_repository>& repositories, const std::u8string_view path)
        {
            const std::u8string key { repository_path_key(path) };
            for (std::size_t index = 0; index < repositories.size(); ++index)
                if (repository_path_key(repositories[index].path) == key)
                {
                    recent_repository entry { std::move(repositories[index]) };
                    repositories.erase(repositories.begin() + static_cast<std::ptrdiff_t>(index));
                    return entry;
                }
            return std::nullopt;
        }
    } // namespace

    bool window_placement::valid() const noexcept
    {
        return width > 0 && height > 0;
    }

    void record_command_use(app_settings& settings, const std::string_view command)
    {
        if (command.empty())
            return;
        std::erase_if(settings.command_history, [command](const std::string& value) { return value == command; });
        settings.command_history.insert(settings.command_history.begin(), std::string { command });
        if (settings.command_history.size() > command_history_capacity)
            settings.command_history.resize(command_history_capacity);
    }

    void touch_recent_repository(app_settings& settings, const std::u8string_view path, std::u8string opened_at)
    {
        if (path.empty())
            return;

        // 고정 여부는 이전 항목에서 물려받는다. 다시 열었다고 고정이 풀리면 놀랍다.
        const std::optional<recent_repository> previous { detach(settings.recent_repositories, path) };

        recent_repository entry {};
        entry.path = normalize_repository_path(path);
        entry.display_name = repository_display_name(entry.path);
        entry.opened_at = std::move(opened_at);
        entry.pinned = previous.has_value() && previous->pinned;
        insert_at_group_front(settings.recent_repositories, std::move(entry));
        trim_unpinned(settings.recent_repositories);
    }

    bool remove_recent_repository(app_settings& settings, const std::u8string_view path)
    {
        const std::u8string key { repository_path_key(path) };
        const std::size_t removed { std::erase_if(settings.recent_repositories, [&key](const recent_repository& value) { return repository_path_key(value.path) == key; }) };
        return removed > 0;
    }

    bool set_recent_repository_pinned(app_settings& settings, const std::u8string_view path, const bool pinned)
    {
        // 이미 그 상태면 자리도 건드리지 않는다. 저장을 부르지 않게 하는 것이 이 반환값의 몫이다.
        const std::u8string key { repository_path_key(path) };
        const auto found { std::ranges::find_if(settings.recent_repositories, [&key](const recent_repository& value) { return repository_path_key(value.path) == key; }) };
        if (found == settings.recent_repositories.end() || found->pinned == pinned)
            return false;

        std::optional<recent_repository> entry { detach(settings.recent_repositories, path) };
        entry->pinned = pinned;
        insert_at_group_front(settings.recent_repositories, std::move(*entry));
        trim_unpinned(settings.recent_repositories);
        return true;
    }

    void normalize_recent_repositories(std::vector<recent_repository>& repositories)
    {
        std::unordered_set<std::u8string> seen {};
        std::erase_if(repositories, [&seen](const recent_repository& value) { return seen.insert(repository_path_key(value.path)).second == false; });
        // 파일이 두 무리를 섞어 두었어도 불변(고정이 앞)은 지킨다. 무리 안의 순서는 그대로다.
        static_cast<void>(std::stable_partition(repositories.begin(), repositories.end(), [](const recent_repository& value) { return value.pinned; }));
        trim_unpinned(repositories);
    }

    bool relocate_recent_repository(app_settings& settings, const std::u8string_view old_path, std::u8string new_path)
    {
        const std::u8string old_key { repository_path_key(old_path) };
        const std::u8string normalized { normalize_repository_path(new_path) };
        if (normalized.empty())
            return false;
        const std::u8string new_key { repository_path_key(normalized) };
        const bool listed { std::ranges::any_of(settings.recent_repositories, [&old_key](const recent_repository& value) { return repository_path_key(value.path) == old_key; }) };
        if (listed == false)
            return false;

        std::vector<recent_repository> result {};
        result.reserve(settings.recent_repositories.size());
        for (recent_repository& value : settings.recent_repositories)
        {
            const std::u8string key { repository_path_key(value.path) };
            if (key == old_key)
            {
                value.path = normalized;
                value.display_name = repository_display_name(normalized);
                result.push_back(std::move(value));
            }
            // 새 경로를 이미 가진 다른 항목은 지운다. 같은 저장소가 둘이 되면 안 된다.
            else if (key != new_key)
                result.push_back(std::move(value));
        }
        settings.recent_repositories = std::move(result);
        return true;
    }

    std::u8string_view theme_preference_name(const theme_preference theme) noexcept
    {
        switch (theme)
        {
        case theme_preference::system:
            return u8"system";
        case theme_preference::light:
            return u8"light";
        case theme_preference::dark:
            return u8"dark";
        }
        return u8"system";
    }

    std::optional<theme_preference> parse_theme_preference(const std::u8string_view name) noexcept
    {
        if (name == u8"system")
            return theme_preference::system;
        if (name == u8"light")
            return theme_preference::light;
        if (name == u8"dark")
            return theme_preference::dark;
        return std::nullopt;
    }

    std::u8string_view diff_layout_preference_name(const diff_layout_preference layout) noexcept
    {
        return layout == diff_layout_preference::split ? u8"split" : u8"inline";
    }

    std::optional<diff_layout_preference> parse_diff_layout_preference(const std::u8string_view name) noexcept
    {
        if (name == u8"inline")
            return diff_layout_preference::inline_view;
        if (name == u8"split")
            return diff_layout_preference::split;
        return std::nullopt;
    }

    std::u8string_view tab_shape_name(const tab_shape shape) noexcept
    {
        return shape == tab_shape::flat ? u8"flat" : u8"connected";
    }

    std::optional<tab_shape> parse_tab_shape(const std::u8string_view name) noexcept
    {
        if (name == u8"connected")
            return tab_shape::connected;
        if (name == u8"flat")
            return tab_shape::flat;
        return std::nullopt;
    }

    std::u8string_view tab_marker_place_name(const tab_marker_place place) noexcept
    {
        switch (place)
        {
        case tab_marker_place::icon_corner:
            return u8"icon_corner";
        case tab_marker_place::close_slot:
            return u8"close_slot";
        case tab_marker_place::badge:
            break;
        }
        return u8"badge";
    }

    std::optional<tab_marker_place> parse_tab_marker_place(const std::u8string_view name) noexcept
    {
        if (name == u8"icon_corner")
            return tab_marker_place::icon_corner;
        if (name == u8"badge")
            return tab_marker_place::badge;
        if (name == u8"close_slot")
            return tab_marker_place::close_slot;
        return std::nullopt;
    }

    std::u8string format_utc_timestamp(const std::chrono::system_clock::time_point time)
    {
        const std::time_t seconds { std::chrono::system_clock::to_time_t(time) };
        std::tm parts {};
        if (gmtime_s(&parts, &seconds) != 0)
            return {};

        char text[32] {};
        const int written {
            std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02dZ", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec),
        };

        if (written != 20)
            return {};
        return std::u8string { reinterpret_cast<const char8_t*>(text), 20 };
    }
} // namespace cyella
