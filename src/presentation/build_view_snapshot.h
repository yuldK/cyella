#pragma once

#include "application/repository_session.h"
#include "application/repository_tabs.h"
#include "domain/app_settings.h"
#include "domain/operation_status.h"
#include "presentation/graph_index.h"
#include "presentation/start_page_view.h"
#include "presentation/ui_state.h"
#include "presentation/view_snapshot.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // snapshot을 짓는 데 필요한 것 전부다. 값을 고치지 않고 읽기만 한다.
    struct view_inputs
    {
        // 창 크기는 물리 픽셀이다.
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
        const repository_tabs* tabs { nullptr };
        const app_settings* settings { nullptr };
        const ui_state* ui { nullptr };
        // 저장소 탭 id → workspace의 저장소 id다.
        const std::map<repository_tab_id, std::string>* tab_repositories { nullptr };
        std::function<const repository_session*(const std::string& repository_id)> find_session {};
        // workspace의 operation 전부(낸 순서)다.
        std::vector<const operation_status*> operations {};
        repository_exists_function exists {};
        // git을 쓸 수 없으면 그 안내다.
        std::u8string git_message {};
        std::u8string git_version {};
        std::chrono::system_clock::time_point now {};
        // 그래프 모델의 cache다. 비어 있으면 매번 짓는다 (test).
        std::function<std::shared_ptr<const std::vector<luil::virtual_list_item>>(const repository_session& session)> graph_items {};
        // 읽어 둔 이력의 부모 관계 cache다. 비어 있으면 매번 짓는다 (test).
        std::function<std::shared_ptr<const graph_index>(const repository_session& session)> history_index {};
    };

    [[nodiscard]] view_snapshot build_view_snapshot(const view_inputs& inputs);

    // ---------------------------------------------------------------- 조각 (test가 직접 부른다)

    // 탭 표시다. `operations`는 그 저장소의 operation만이다.
    [[nodiscard]] tab_marker compute_tab_marker(std::span<const operation_status* const> operations, std::uint32_t changed_file_count, bool awaiting_input) noexcept;

    // 사이드바 행이다. 거르기는 이름의 부분 문자열이고, 접힌 구역은 머리 줄만 남는다.
    struct sidebar_rows
    {
        std::vector<sidebar_row_view> rows {};
        // 거른 뒤의 구역별 항목 수다.
        std::array<std::uint32_t, sidebar_section_count> counts {};
    };
    [[nodiscard]] sidebar_rows build_sidebar_rows(const repository_snapshot& snapshot, std::u8string_view filter, std::uint8_t collapsed_sections);

    // 그래프 목록 모델이다. 키는 `graph_item_key`, 높이는 기본 행 높이다.
    [[nodiscard]] std::vector<luil::virtual_list_item> build_graph_items(const std::vector<commit_info>& commits);

    // "방금", "N분 전", "N시간 전", "N일 전", "N개월 전", "N년 전"이다.
    [[nodiscard]] std::u8string relative_time_text(std::int64_t now_seconds, std::int64_t then_seconds);

    // 상태 줄·로그가 쓰는 작업 이름이다 ("Fetch", "커밋").
    [[nodiscard]] std::u8string operation_display_name(operation_kind kind);

    // 파일 경로를 이름과 폴더로 가른다 (`src/a/b.cpp` → `b.cpp`, `/src/a`).
    [[nodiscard]] file_row_view make_file_row(const file_change& change, std::u8string key_prefix);
} // namespace cyella
