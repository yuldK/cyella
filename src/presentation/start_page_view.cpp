#include "presentation/start_page_view.h"

#include "domain/repository_path.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace cyella {
    namespace {
        constexpr char8_t ascii_lower(const char8_t value) noexcept
        {
            if (value >= u8'A' && value <= u8'Z')
                return static_cast<char8_t>(value - u8'A' + u8'a');
            return value;
        }

        constexpr bool is_search_space(const char8_t value) noexcept
        {
            return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n';
        }

        std::u8string normalized_search(std::u8string_view search)
        {
            while (search.empty() == false && is_search_space(search.front()))
                search.remove_prefix(1);
            while (search.empty() == false && is_search_space(search.back()))
                search.remove_suffix(1);

            std::u8string result {};
            result.reserve(search.size());
            for (const char8_t value : search)
                result.push_back(value == u8'/' ? u8'\\' : ascii_lower(value));
            return result;
        }

        bool contains_folded(const std::u8string_view text, const std::u8string& folded_search)
        {
            if (folded_search.empty())
                return true;
            if (text.size() < folded_search.size())
                return false;

            const std::size_t last_start { text.size() - folded_search.size() };
            for (std::size_t start = 0; start <= last_start; ++start)
            {
                std::size_t offset { 0 };
                while (offset < folded_search.size() && ascii_lower(text[start + offset]) == folded_search[offset])
                    ++offset;
                if (offset == folded_search.size())
                    return true;
            }
            return false;
        }
    } // namespace

    bool matches_search_text(const std::u8string_view text, const std::u8string_view search)
    {
        return contains_folded(text, normalized_search(search));
    }

    start_page_view build_start_page_view(const app_settings& settings, const repository_exists_function& exists, const std::u8string_view search)
    {
        start_page_view view {};
        view.total_count = settings.recent_repositories.size();
        const std::u8string folded_search { normalized_search(search) };

        std::vector<const recent_repository*> pinned {};
        std::vector<const recent_repository*> recent {};
        for (const recent_repository& repository : settings.recent_repositories)
            (repository.pinned ? pinned : recent).push_back(&repository);

        // 설정 목록은 보통 이미 이 순서지만 손으로 고친 파일을 믿지 않는다. stable이라 시각이
        // 같으면 목록의 순서가 남는다.
        std::stable_sort(recent.begin(), recent.end(), [](const recent_repository* left, const recent_repository* right) { return left->opened_at > right->opened_at; });

        const auto append = [&](const std::vector<const recent_repository*>& group) {
            for (const recent_repository* repository : group)
            {
                const std::u8string display_name { repository->display_name.empty() ? repository_display_name(repository->path) : repository->display_name };
                // 이름과 경로 어느 쪽이든 맞으면 남긴다.
                if (contains_folded(display_name, folded_search) == false && contains_folded(normalize_repository_path(repository->path), folded_search) == false)
                    continue;

                start_page_item item {};
                item.display_name = display_name;
                item.parent_folder = repository_parent_folder(repository->path);
                item.path = repository->path;
                item.opened_at = repository->opened_at;
                item.pinned = repository->pinned;
                item.exists = exists ? exists(repository->path) : true;
                view.items.push_back(std::move(item));
            }
        };
        append(pinned);
        append(recent);
        return view;
    }
} // namespace cyella
