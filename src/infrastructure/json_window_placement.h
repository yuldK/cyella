#pragma once

#include "application/settings_file_system.h"
#include "application/window_placement.h"

#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // 창 배치 파일은 설정 파일과 같은 폴더의 `window-placement.json`이다. 설정 파일과 따로 두는 이유는 창을 옮길 때마다
    // 쓰는 값이라 환경 설정의 저장·복구 규칙(깨진 설정 백업, 편집 중인 draft)과 얽히지 않게 하려는 것이다.
    [[nodiscard]] std::u8string window_placement_path(std::u8string_view settings_path);

    // 읽을 수 없거나 값이 int 범위를 넘거나 크기가 양수가 아니면 값이 없다.
    [[nodiscard]] std::optional<saved_window_placement> parse_window_placement_json(std::u8string_view bytes);
    [[nodiscard]] std::u8string window_placement_json(const saved_window_placement& placement);

    // 설정 경로가 비었거나 파일이 없거나 깨졌으면 값이 없다 (처음 크기로 연다).
    [[nodiscard]] std::optional<saved_window_placement> load_window_placement(settings_file_system& files, std::u8string_view settings_path);
    [[nodiscard]] bool save_window_placement(settings_file_system& files, std::u8string_view settings_path, const saved_window_placement& placement);
} // namespace cyella
