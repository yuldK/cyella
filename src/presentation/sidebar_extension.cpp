#include "presentation/sidebar_extension.h"

#include "presentation/start_page_view.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string number(const std::uint64_t value)
        {
            return to_u8(std::to_string(value));
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
                tooltip += u8"\n기록된 커밋 " + short_sha(info.recorded_sha);
            if (info.checked_out_sha.empty() == false)
                tooltip += u8"\n체크아웃한 커밋 " + short_sha(info.checked_out_sha);
            return tooltip;
        }

        std::u8string worktree_tooltip(const worktree_info& info)
        {
            std::u8string tooltip { to_u8(info.path) };
            if (info.branch.empty() == false)
                tooltip += u8"\n브랜치 " + to_u8(info.branch);
            else if (info.detached)
                tooltip += u8"\ndetached HEAD " + short_sha(info.head_sha);
            if (info.locked)
                tooltip += u8"\n잠김" + (info.lock_reason.empty() ? std::u8string {} : u8": " + to_u8(info.lock_reason));
            if (info.prunable)
                tooltip += u8"\n폴더가 사라져 정리(prune)할 수 있습니다";
            return tooltip;
        }
    } // namespace

    std::u8string submodule_state_text(const submodule_state state)
    {
        switch (state)
        {
        case submodule_state::initialized:
            return u8"초기화됨";
        case submodule_state::uninitialized:
            return u8"미초기화";
        case submodule_state::modified:
            return u8"수정됨";
        case submodule_state::out_of_date:
            return u8"구버전";
        case submodule_state::conflict:
            return u8"충돌";
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
                    join(badge, u8"주");
                if (info.is_current)
                    join(badge, u8"현재");
                if (info.locked)
                    join(badge, u8"잠김");
                if (info.prunable)
                    join(badge, u8"정리 가능");
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
                    status.detail = missing == 0 ? u8"모두 받음" : number(missing) + u8"개 포인터";
                    status.badge = snapshot.lfs.installed ? std::u8string {} : u8"설정 필요";
                    status.tone = badge_tone::warning;
                    status.tooltip = status.label + u8" · " + (snapshot.lfs.installed ? u8"이 저장소에 설정됨" : u8"이 저장소에 `git lfs install`이 없어 pull이 설정부터 합니다");
                }
                else
                {
                    status.label = u8"git-lfs를 찾을 수 없음";
                    status.badge = u8"없음";
                    status.tone = badge_tone::danger;
                    status.tooltip = u8"이 저장소는 LFS를 쓰지만 git-lfs를 실행할 수 없습니다. 설치한 뒤 다시 읽으세요.";
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
                row.badge = file.checked_out ? u8"받음" : u8"포인터";
                row.tone = file.checked_out ? badge_tone::accent : badge_tone::warning;
                row.tooltip = row.label + u8" · " + row.detail
                    + (file.checked_out       ? u8" · 실제 내용이 작업 트리에 있습니다"
                            : file.downloaded ? u8" · 객체는 받았고 작업 트리는 포인터입니다"
                                              : u8" · 포인터만 있습니다. LFS pull로 받으세요");
                result.rows.push_back(std::move(row));
            }
            if (count > listed && collapsed(sidebar_section::lfs) == false)
            {
                sidebar_row_view rest {};
                rest.key = u8"lfs:more";
                rest.section = sidebar_section::lfs;
                rest.item = sidebar_item_kind::lfs;
                rest.label = u8"외 " + number(count - listed) + u8"개";
                rest.dimmed = true;
                result.rows.push_back(std::move(rest));
            }
            set_count(sidebar_section::lfs, header_row, count);
        }
    }
} // namespace cyella
