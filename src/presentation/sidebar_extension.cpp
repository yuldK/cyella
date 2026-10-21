#include "presentation/sidebar_extension.h"

#include "domain/text_catalog.h"
#include "presentation/start_page_view.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string short_sha(const std::string_view sha)
        {
            return to_u8(sha.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        badge_tone submodule_tone(const submodule_state state) noexcept
        {
            switch (state)
            {
            case submodule_state::initialized:
                return badge_tone::none;
            case submodule_state::uninitialized:
            case submodule_state::modified:
            case submodule_state::out_of_date:
                return badge_tone::warning;
            case submodule_state::conflict:
                return badge_tone::danger;
            }
            return badge_tone::none;
        }

        // 구분자(` · `)로 이어 붙인다.
        void join(std::u8string& text, const std::u8string_view part)
        {
            if (text.empty() == false)
                text += u8" · ";
            text += part;
        }

        std::u8string submodule_tooltip(const submodule_info& info)
        {
            std::u8string tooltip { to_u8(info.path) + u8" · " + submodule_state_text(info.state) };
            if (info.url.empty() == false)
                tooltip += u8"\n" + to_u8(info.url);
            if (info.recorded_sha.empty() == false)
                tooltip += u8"\n" + tr(u8"sidebar.submodule.recorded_commit", { { u8"sha", short_sha(info.recorded_sha) } });
            if (info.checked_out_sha.empty() == false)
                tooltip += u8"\n" + tr(u8"sidebar.submodule.checked_out_commit", { { u8"sha", short_sha(info.checked_out_sha) } });
            return tooltip;
        }

        std::u8string worktree_tooltip(const worktree_info& info)
        {
            std::u8string tooltip { to_u8(info.path) };
            if (info.branch.empty() == false)
                tooltip += u8"\n" + tr(u8"sidebar.worktree.branch", { { u8"branch", info.branch } });
            else if (info.detached)
                tooltip += u8"\ndetached HEAD " + short_sha(info.head_sha);
            if (info.locked)
                tooltip += u8"\n" + (info.lock_reason.empty() ? tr(u8"sidebar.worktree.locked") : tr(u8"sidebar.worktree.locked_reason", { { u8"reason", info.lock_reason } }));
            if (info.prunable)
                tooltip += u8"\n" + tr(u8"sidebar.worktree.prunable_tooltip");
            return tooltip;
        }
    } // namespace

    std::u8string submodule_state_text(const submodule_state state)
    {
        switch (state)
        {
        case submodule_state::initialized:
            return tr(u8"sidebar.submodule.state_initialized");
        case submodule_state::uninitialized:
            return tr(u8"sidebar.submodule.state_uninitialized");
        case submodule_state::modified:
            return tr(u8"sidebar.submodule.state_modified");
        case submodule_state::out_of_date:
            return tr(u8"sidebar.submodule.state_out_of_date");
        case submodule_state::conflict:
            return tr(u8"sidebar.submodule.state_conflict");
        }
        return {};
    }

    std::u8string path_leaf(const std::string_view path)
    {
        std::string_view trimmed { path };
        while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\'))
            trimmed.remove_suffix(1);
        const std::size_t separator { trimmed.find_last_of("/\\") };
        return to_u8(separator == std::string_view::npos ? trimmed : trimmed.substr(separator + 1));
    }

    void append_extension_sidebar_rows(sidebar_rows& result, const repository_snapshot& snapshot, const std::u8string_view filter, const std::uint8_t collapsed_sections)
    {
        const auto collapsed = [collapsed_sections](const sidebar_section section) { return (collapsed_sections & (1u << static_cast<unsigned>(section))) != 0; };
        const auto matches = [filter](const std::string_view name) { return matches_search_text(to_u8(name), filter); };
        const auto add_header = [&result, &collapsed](const sidebar_section section, std::u8string label) {
            result.rows.push_back({ .key = u8"h:" + label, .header = true, .section = section, .label = std::move(label), .collapsed = collapsed(section) });
            return result.rows.size() - 1;
        };
        const auto set_count = [&result](const sidebar_section section, const std::size_t header_row, const std::uint32_t count) {
            result.counts[static_cast<std::size_t>(section)] = count;
            result.rows[header_row].count = count;
        };

        // SUBMODULES
        {
            const std::size_t header_row { add_header(sidebar_section::submodules, u8"SUBMODULES") };
            std::uint32_t count { 0 };
            for (const submodule_info& info : snapshot.submodules)
            {
                if (matches(info.path) == false)
                    continue;
                ++count;
                if (collapsed(sidebar_section::submodules))
                    continue;
                sidebar_row_view row {};
                row.key = u8"sm:" + to_u8(info.path);
                row.section = sidebar_section::submodules;
                row.item = sidebar_item_kind::submodule;
                row.path = info.path;
                row.label = to_u8(info.path);
                row.submodule = info.state;
                row.depth = static_cast<int>(info.depth);
                // 체크아웃한 커밋이 없으면(초기화 전) 기록된 커밋을 보인다.
                row.detail = short_sha(info.checked_out_sha.empty() ? info.recorded_sha : info.checked_out_sha);
                if (info.state != submodule_state::initialized)
                    row.badge = submodule_state_text(info.state);
                row.tone = submodule_tone(info.state);
                row.dimmed = info.state == submodule_state::uninitialized;
                row.target_sha = info.checked_out_sha.empty() ? info.recorded_sha : info.checked_out_sha;
                row.tooltip = submodule_tooltip(info);
                result.rows.push_back(std::move(row));
            }
            set_count(sidebar_section::submodules, header_row, count);
        }

        // WORKTREES
        {
            const std::size_t header_row { add_header(sidebar_section::worktrees, u8"WORKTREES") };
            std::uint32_t count { 0 };
            for (const worktree_info& info : snapshot.worktrees)
            {
                if (matches(info.path) == false && matches(info.branch) == false)
                    continue;
                ++count;
                if (collapsed(sidebar_section::worktrees))
                    continue;
                sidebar_row_view row {};
                row.key = u8"wt:" + to_u8(info.path);
                row.section = sidebar_section::worktrees;
                row.item = sidebar_item_kind::worktree;
                row.path = info.path;
                row.label = path_leaf(info.path);
                row.current = info.is_current;
                row.worktree_main = info.is_main;
                row.worktree_current = info.is_current;
                row.worktree_locked = info.locked;
                row.worktree_prunable = info.prunable;
                if (info.bare)
                    row.detail = u8"bare";
                else if (info.branch.empty() == false)
                    row.detail = to_u8(info.branch);
                else
                    row.detail = u8"detached " + short_sha(info.head_sha);
                std::u8string badge {};
                if (info.is_main)
                    join(badge, tr(u8"sidebar.worktree.main"));
                if (info.is_current)
                    join(badge, tr(u8"sidebar.worktree.current"));
                if (info.locked)
                    join(badge, tr(u8"sidebar.worktree.locked"));
                if (info.prunable)
                    join(badge, tr(u8"sidebar.worktree.prunable"));
                row.badge = std::move(badge);
                row.tone = info.prunable ? badge_tone::danger : info.locked ? badge_tone::warning : badge_tone::accent;
                row.dimmed = info.prunable;
                row.target_sha = info.head_sha;
                row.tooltip = worktree_tooltip(info);
                result.rows.push_back(std::move(row));
            }
            set_count(sidebar_section::worktrees, header_row, count);
        }

        // LFS: 저장소가 LFS를 쓸 때만 선다. 머리 메뉴에 pull·fetch·추적 패턴 추가가 있다.
        if (snapshot.lfs.in_use)
        {
            const std::size_t header_row { add_header(sidebar_section::lfs, u8"GIT LFS") };
            std::uint32_t count { 0 };
            if (collapsed(sidebar_section::lfs) == false)
            {
                sidebar_row_view status {};
                status.key = u8"lfs:status";
                status.section = sidebar_section::lfs;
                status.item = sidebar_item_kind::lfs;
                if (snapshot.lfs.available)
                {
                    status.label = to_u8(snapshot.lfs.version);
                    const std::size_t missing { snapshot.lfs.missing_count() };
                    status.detail = missing == 0 ? tr(u8"sidebar.lfs.all_downloaded") : tr(u8"sidebar.lfs.pointer_count", { { u8"count", missing } });
                    status.badge = snapshot.lfs.installed ? std::u8string {} : tr(u8"sidebar.lfs.setup_needed");
                    status.tone = badge_tone::warning;
                    status.tooltip = status.label + u8" · " + (snapshot.lfs.installed ? tr(u8"sidebar.lfs.installed_tooltip") : tr(u8"sidebar.lfs.not_installed_tooltip"));
                }
                else
                {
                    status.label = tr(u8"sidebar.lfs.not_found");
                    status.badge = tr(u8"sidebar.lfs.missing");
                    status.tone = badge_tone::danger;
                    status.tooltip = tr(u8"sidebar.lfs.unavailable_tooltip");
                }
                result.rows.push_back(std::move(status));
            }
            std::size_t listed { 0 };
            for (const lfs_file& file : snapshot.lfs.files)
            {
                if (matches(file.path) == false)
                    continue;
                ++count;
                if (collapsed(sidebar_section::lfs) || listed >= lfs_sidebar_file_limit)
                    continue;
                ++listed;
                sidebar_row_view row {};
                row.key = u8"lfs:f:" + to_u8(file.path);
                row.section = sidebar_section::lfs;
                row.item = sidebar_item_kind::lfs;
                row.path = file.path;
                row.label = to_u8(file.path);
                row.detail = to_u8(format_byte_size(file.size));
                row.badge = file.checked_out ? tr(u8"sidebar.lfs.file_downloaded") : tr(u8"sidebar.lfs.file_pointer");
                row.tone = file.checked_out ? badge_tone::accent : badge_tone::warning;
                row.tooltip = row.label + u8" · " + row.detail
                    + (file.checked_out       ? tr(u8"sidebar.lfs.file_checked_out_suffix")
                            : file.downloaded ? tr(u8"sidebar.lfs.file_object_only_suffix")
                                              : tr(u8"sidebar.lfs.file_pointer_only_suffix"));
                result.rows.push_back(std::move(row));
            }
            if (count > listed && collapsed(sidebar_section::lfs) == false)
            {
                sidebar_row_view rest {};
                rest.key = u8"lfs:more";
                rest.section = sidebar_section::lfs;
                rest.item = sidebar_item_kind::lfs;
                rest.label = tr(u8"sidebar.lfs.more_files", { { u8"count", count - listed } });
                rest.dimmed = true;
                result.rows.push_back(std::move(rest));
            }
            set_count(sidebar_section::lfs, header_row, count);
        }
    }
} // namespace cyella
