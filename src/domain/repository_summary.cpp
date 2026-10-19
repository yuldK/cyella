#include "domain/repository_summary.h"

#include <algorithm>
#include <cstddef>

namespace cyella {
    namespace {
        // 경로 구분자를 슬래시와 역슬래시 모두 인정하고 마지막 구성 요소만 돌려준다.
        // 끝의 구분자(디렉터리 표지 `rebase-merge/`)는 무시한다.
        std::string_view last_component(std::string_view path) noexcept
        {
            while (path.empty() == false && (path.back() == '/' || path.back() == '\\'))
                path.remove_suffix(1);

            const std::size_t separator { path.find_last_of("/\\") };
            if (separator != std::string_view::npos)
                path.remove_prefix(separator + 1);
            return path;
        }
    } // namespace

    repository_operation_state detect_repository_operation_state(const std::span<const std::string> existing_marker_paths) noexcept
    {
        const auto exists = [existing_marker_paths](const std::string_view marker) noexcept {
            return std::any_of(existing_marker_paths.begin(), existing_marker_paths.end(), [marker](const std::string& path) noexcept { return last_component(path) == marker; });
        };

        if (exists("rebase-merge") || exists("rebase-apply"))
            return repository_operation_state::rebase;
        if (exists("CHERRY_PICK_HEAD"))
            return repository_operation_state::cherry_pick;
        if (exists("REVERT_HEAD"))
            return repository_operation_state::revert;
        if (exists("MERGE_HEAD"))
            return repository_operation_state::merge;
        return repository_operation_state::none;
    }

    std::string_view repository_operation_state_name(const repository_operation_state state) noexcept
    {
        switch (state)
        {
        case repository_operation_state::none:
            return "none";
        case repository_operation_state::rebase:
            return "rebase";
        case repository_operation_state::merge:
            return "merge";
        case repository_operation_state::cherry_pick:
            return "cherry_pick";
        case repository_operation_state::revert:
            return "revert";
        }
        return "none";
    }
} // namespace cyella
