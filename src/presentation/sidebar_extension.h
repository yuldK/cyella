#pragma once

#include "application/operation_events.h"
#include "presentation/build_view_snapshot.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace cyella {
    // 사이드바의 SUBMODULES·WORKTREES·LFS 구역이다. `build_sidebar_rows`가 LOCAL~STASHES 뒤에 이어 붙인다.
    // snapshot만 읽는 순수 함수라 UI 없이 test한다.
    //
    // 행 키: `sm:<경로>`, `wt:<폴더>`, `lfs:status`, `lfs:f:<경로>`. LFS 구역은 저장소가 LFS를 쓸 때(`snapshot.lfs.in_use`)만 선다.
    void append_extension_sidebar_rows(sidebar_rows& result, const repository_snapshot& snapshot, std::u8string_view filter, std::uint8_t collapsed_sections);

    // LFS 파일 행은 이만큼만 보이고 나머지는 "외 N개"다.
    inline constexpr std::size_t lfs_sidebar_file_limit { 200 };

    // 사이드바 꼬리 글과 메뉴가 쓰는 문구다.
    [[nodiscard]] std::u8string submodule_state_text(submodule_state state);
    // 폴더 경로의 마지막 구성 요소다 (`C:/work/wt1` → `wt1`).
    [[nodiscard]] std::u8string path_leaf(std::string_view path);
} // namespace cyella
