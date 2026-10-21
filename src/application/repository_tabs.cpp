#include "application/repository_tabs.h"

#include "domain/repository_path.h"

#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace cyella {
    repository_tabs::repository_tabs()
    {
        tabs_.push_back(make_start_tab());
    }

    repository_tab repository_tabs::make_start_tab()
    {
        repository_tab tab {};
        tab.id = next_id_++;
        tab.kind = repository_tab_kind::start;
        return tab;
    }

    repository_tab repository_tabs::make_repository_tab(const std::u8string_view path)
    {
        repository_tab tab {};
        tab.id = next_id_++;
        tab.kind = repository_tab_kind::repository;
        tab.path = normalize_repository_path(path);
        tab.display_name = repository_display_name(tab.path);
        return tab;
    }

    const std::vector<repository_tab>& repository_tabs::tabs() const noexcept
    {
        return tabs_;
    }

    std::size_t repository_tabs::selected_index() const noexcept
    {
        return selected_;
    }

    const repository_tab& repository_tabs::selected() const noexcept
    {
        return tabs_[selected_];
    }

    std::optional<std::size_t> repository_tabs::index_of(const repository_tab_id id) const noexcept
    {
        for (std::size_t index = 0; index < tabs_.size(); ++index)
            if (tabs_[index].id == id)
                return index;
        return std::nullopt;
    }

    std::optional<std::size_t> repository_tabs::find_repository(const std::u8string_view path) const
    {
        const std::u8string key { repository_path_key(path) };
        for (std::size_t index = 0; index < tabs_.size(); ++index)
            if (tabs_[index].kind == repository_tab_kind::repository && repository_path_key(tabs_[index].path) == key)
                return index;
        return std::nullopt;
    }

    open_repository_result repository_tabs::open_repository(const std::u8string_view path)
    {
        open_repository_result result {};
        if (path.empty())
        {
            result.id = selected().id;
            return result;
        }

        if (const std::optional<std::size_t> existing { find_repository(path) }; existing.has_value())
        {
            selected_ = *existing;
            result.id = tabs_[selected_].id;
            return result;
        }

        repository_tab tab { make_repository_tab(path) };
        result.id = tab.id;
        result.created = true;
        if (selected().kind == repository_tab_kind::start)
        {
            tabs_[selected_] = std::move(tab);
            result.replaced_start_tab = true;
            return result;
        }

        const std::size_t position { selected_ + 1 };
        tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(position), std::move(tab));
        selected_ = position;
        return result;
    }

    repository_tab_id repository_tabs::new_tab()
    {
        repository_tab tab { make_start_tab() };
        const repository_tab_id id { tab.id };
        tabs_.push_back(std::move(tab));
        selected_ = tabs_.size() - 1;
        return id;
    }

    bool repository_tabs::select(const std::size_t index)
    {
        if (index >= tabs_.size() || index == selected_)
            return false;
        selected_ = index;
        return true;
    }

    bool repository_tabs::close(const std::size_t index)
    {
        if (index >= tabs_.size())
            return false;

        if (tabs_.size() == 1)
        {
            // 시작 탭 하나뿐이면 닫아도 같은 모양이라 바꿀 것이 없다.
            if (tabs_.front().kind == repository_tab_kind::start)
                return false;
            tabs_.front() = make_start_tab();
            selected_ = 0;
            return true;
        }

        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
        if (index < selected_)
            --selected_;
        else if (index == selected_)
            // 오른쪽 이웃이 지운 자리로 당겨 와 있다. 마지막 탭이었다면 왼쪽 이웃이 남는다.
            selected_ = std::min(index, tabs_.size() - 1);
        return true;
    }

    bool repository_tabs::reorder(const std::size_t from, const std::size_t to)
    {
        if (from >= tabs_.size() || to >= tabs_.size() || from == to)
            return false;

        // 선택은 인덱스가 아니라 탭을 따라가야 하므로 id로 기억한다.
        const repository_tab_id selected_id { tabs_[selected_].id };
        repository_tab moved { std::move(tabs_[from]) };
        tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
        tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        selected_ = index_of(selected_id).value_or(0);
        return true;
    }

    void repository_tabs::restore(const std::vector<std::u8string>& paths, const std::u8string_view active_path)
    {
        tabs_.clear();
        selected_ = 0;

        std::unordered_set<std::u8string> seen {};
        for (const std::u8string& path : paths)
        {
            if (path.empty() || seen.insert(repository_path_key(path)).second == false)
                continue;
            tabs_.push_back(make_repository_tab(path));
        }

        if (tabs_.empty())
        {
            tabs_.push_back(make_start_tab());
            return;
        }

        if (active_path.empty() == false)
            selected_ = find_repository(active_path).value_or(0);
    }

    std::vector<std::u8string> repository_tabs::repository_paths() const
    {
        std::vector<std::u8string> paths {};
        for (const repository_tab& tab : tabs_)
            if (tab.kind == repository_tab_kind::repository)
                paths.push_back(tab.path);
        return paths;
    }

    std::u8string repository_tabs::active_repository_path() const
    {
        return selected().kind == repository_tab_kind::repository ? selected().path : std::u8string {};
    }

    void capture_open_tabs(const repository_tabs& tabs, app_settings& settings)
    {
        settings.open_tabs = tabs.repository_paths();
        settings.active_tab = tabs.active_repository_path();
    }

    repository_tabs restore_tabs(const app_settings& settings)
    {
        repository_tabs tabs {};
        tabs.restore(settings.open_tabs, settings.active_tab);
        return tabs;
    }
} // namespace cyella
