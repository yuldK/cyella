#include "presentation/diff_special.h"

#include "domain/lfs.h"

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
                return u8"없음";
            return to_u8(format_byte_size(pointer->size)) + u8" · oid " + short_text(pointer->oid, 10);
        }

        std::u8string local_text(const std::optional<lfs_pointer>& pointer, const bool local)
        {
            if (pointer.has_value() == false)
                return u8"해당 없음";
            return local ? u8"로컬에 있음" : u8"로컬에 없음";
        }

        diff_special_view build_gitlink_card(const diff_file& file)
        {
            subproject_side old_side {};
            subproject_side new_side {};
            read_subproject_sides(file, old_side, new_side);

            diff_special_view card {};
            card.kind = diff_special_kind::submodule_pointer;
            card.title = u8"서브모듈 포인터";
            card.rows.push_back({ u8"서브모듈", to_u8(file.display_path()) });
            card.rows.push_back({ u8"이전 커밋", old_side.present ? short_text(old_side.sha, 12) : u8"없음 (새 서브모듈)" });
            std::u8string current { new_side.present ? short_text(new_side.sha, 12) : u8"없음 (서브모듈 제거)" };
            if (new_side.dirty)
                current += u8" · 서브모듈 안에 커밋하지 않은 변경이 있습니다";
            card.rows.push_back({ u8"새 커밋", std::move(current) });
            card.note = u8"서브모듈은 파일이 아니라 커밋 하나를 가리킵니다. 텍스트 diff 대신 가리키는 커밋만 보입니다.";
            if (file.status != 'D')
                card.open_path = file.display_path();
            return card;
        }

        diff_special_view build_lfs_card(const diff_file& file)
        {
            const lfs_diff_info& info { *file.lfs };
            diff_special_view card {};
            card.kind = diff_special_kind::lfs_pointer;
            card.title = u8"LFS 포인터";
            card.rows.push_back({ u8"파일", to_u8(file.display_path()) });
            card.rows.push_back({ u8"이전", pointer_text(info.old_pointer) });
            card.rows.push_back({ u8"현재", pointer_text(info.new_pointer) });
            card.rows.push_back({ u8"이전 내용", local_text(info.old_pointer, info.old_local) });
            card.rows.push_back({ u8"현재 내용", local_text(info.new_pointer, info.new_local) });
            card.note = u8"저장소에는 큰 파일 대신 oid와 크기만 든 포인터가 있어 텍스트 diff로 보이지 않습니다.";
            if (info.all_local() == false)
            {
                card.note += u8" 실제 내용을 받으면 이미지·바이너리 비교가 보입니다.";
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
