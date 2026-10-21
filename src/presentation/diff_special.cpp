#include "presentation/diff_special.h"

#include "domain/lfs.h"
#include "domain/text_catalog.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace cyella {
    namespace {
        constexpr std::string_view subproject_prefix { "Subproject commit " };
        constexpr std::string_view dirty_suffix { "-dirty" };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string short_text(const std::string_view text, const std::size_t length)
        {
            return to_u8(text.substr(0, std::min(text.size(), length)));
        }

        struct subproject_side
        {
            std::string sha {};
            bool dirty { false };
            bool present { false };
        };

        // hunk의 `Subproject commit <sha>[-dirty]` 줄에서 옛 쪽(삭제·문맥)과 새 쪽(추가·문맥)을 읽는다.
        void read_subproject_sides(const diff_file& file, subproject_side& old_side, subproject_side& new_side)
        {
            for (const diff_hunk& hunk : file.hunks)
                for (const diff_line& line : hunk.lines)
                {
                    if (line.content.starts_with(subproject_prefix) == false)
                        continue;
                    std::string_view sha { std::string_view { line.content }.substr(subproject_prefix.size()) };
                    subproject_side side {};
                    side.present = true;
                    if (sha.ends_with(dirty_suffix))
                    {
                        side.dirty = true;
                        sha.remove_suffix(dirty_suffix.size());
                    }
                    side.sha = std::string { sha };
                    if (line.kind != diff_line_kind::add)
                        old_side = side;
                    if (line.kind != diff_line_kind::del)
                        new_side = side;
                }
        }

        std::u8string pointer_text(const std::optional<lfs_pointer>& pointer)
        {
            if (pointer.has_value() == false)
                return tr(u8"diff.special.lfs_pointer_none");
            return to_u8(format_byte_size(pointer->size)) + u8" · oid " + short_text(pointer->oid, 10);
        }

        std::u8string local_text(const std::optional<lfs_pointer>& pointer, const bool local)
        {
            if (pointer.has_value() == false)
                return tr(u8"diff.special.lfs_local_not_applicable");
            return local ? tr(u8"diff.special.lfs_local_present") : tr(u8"diff.special.lfs_local_missing");
        }

        diff_special_view build_gitlink_card(const diff_file& file)
        {
            subproject_side old_side {};
            subproject_side new_side {};
            read_subproject_sides(file, old_side, new_side);

            diff_special_view card {};
            card.kind = diff_special_kind::submodule_pointer;
            card.title = tr(u8"diff.special.submodule_title");
            card.rows.push_back({ tr(u8"diff.special.submodule_label"), to_u8(file.display_path()) });
            card.rows.push_back({ tr(u8"diff.special.old_commit_label"), old_side.present ? short_text(old_side.sha, 12) : tr(u8"diff.special.submodule_added") });
            std::u8string current { new_side.present ? short_text(new_side.sha, 12) : tr(u8"diff.special.submodule_removed") };
            if (new_side.dirty)
                current += u8" · " + tr(u8"diff.special.submodule_dirty");
            card.rows.push_back({ tr(u8"diff.special.new_commit_label"), std::move(current) });
            card.note = tr(u8"diff.special.submodule_note");
            if (file.status != 'D')
                card.open_path = file.display_path();
            return card;
        }

        diff_special_view build_lfs_card(const diff_file& file)
        {
            const lfs_diff_info& info { *file.lfs };
            diff_special_view card {};
            card.kind = diff_special_kind::lfs_pointer;
            card.title = tr(u8"diff.special.lfs_title");
            card.rows.push_back({ tr(u8"diff.special.file_label"), to_u8(file.display_path()) });
            card.rows.push_back({ tr(u8"diff.special.lfs_old_label"), pointer_text(info.old_pointer) });
            card.rows.push_back({ tr(u8"diff.special.lfs_new_label"), pointer_text(info.new_pointer) });
            card.rows.push_back({ tr(u8"diff.special.lfs_old_content_label"), local_text(info.old_pointer, info.old_local) });
            card.rows.push_back({ tr(u8"diff.special.lfs_new_content_label"), local_text(info.new_pointer, info.new_local) });
            card.note = tr(u8"diff.special.lfs_note");
            if (info.all_local() == false)
            {
                card.note = tr(u8"diff.special.lfs_note_pull");
                card.action_label = u8"LFS pull";
                card.action = lfs_pull_request {};
            }
            return card;
        }
    } // namespace

    std::optional<diff_special_view> build_diff_special(const diff_file& file)
    {
        if (file.is_gitlink())
            return build_gitlink_card(file);
        if (file.lfs.has_value() && file.lfs->all_local() == false)
            return build_lfs_card(file);
        return std::nullopt;
    }
} // namespace cyella
