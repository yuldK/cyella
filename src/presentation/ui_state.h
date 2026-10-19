#pragma once

#include "application/git_operation.h"
#include "application/repository_tabs.h"
#include "domain/operation_status.h"
#include "presentation/shell_layout.h"

#include "luil/text/text_edit.h"
#include "luil/ui/ui_interaction.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace cyella {
    // 화면만 아는 상태(선택·스크롤·판 폭·접힘·입력 초안)다. driver가 소유하고 logic thread에서만 바꾼다.
    // git 상태의 정본은 workspace·세션이고, 탭 목록의 정본은 `repository_tabs`다. 여기 있는 것은
    // 껐다 켜면 잃어도 되는 것들이다.

    // 텍스트 입력 칸의 대상이다. luil `text_input_target`에 값 그대로 실린다 (0은 luil의 none이라 쓰지 않는다).
    enum class text_target : std::uint32_t
    {
        none = 0,
        start_search,
        sidebar_filter,
        commit_subject,
        commit_body,
        clone_url,
        clone_path,
    };

    // 사이드바 구역이다. 순서가 곧 화면 순서다.
    enum class sidebar_section : std::uint8_t
    {
        local,
        remote,
        tags,
        stashes,
    };

    inline constexpr std::size_t sidebar_section_count { 4 };

    // 상세 패널이 보이는 것이다.
    enum class detail_mode
    {
        // 고른 것이 없다.
        none,
        commit,
        // WIP 행을 골랐다. 작업 영역(unstaged·staged·커밋 상자)이다.
        working,
    };

    struct tab_ui_state
    {
        // 고른 커밋의 SHA다. WIP 행이면 `graph_wip_sha`다.
        std::string selected_sha {};
        // 사이드바에서 고른 항목의 키다.
        std::u8string selected_sidebar_key {};
        float graph_scroll { 0.0f };
        float sidebar_scroll { 0.0f };
        float detail_scroll { 0.0f };
        float diff_scroll { 0.0f };
        // 접힌 구역이다 (`sidebar_section` 비트).
        std::uint8_t collapsed_sections { 0 };
        luil::text::text_edit_state sidebar_filter {};
        luil::text::text_edit_state commit_subject {};
        luil::text::text_edit_state commit_body {};
        bool amend { false };
        // 상세 패널에서 고른 파일의 diff 요청이다. 있으면 그래프 자리를 diff가 덮는다.
        std::optional<diff_file_request> diff {};
        // 낸 커밋 operation이다. 성공하면 커밋 상자를 비운다.
        operation_id pending_commit { 0 };
        // 배너에서 닫은 실패·입력 필요 operation이다. 그보다 새 것만 배너로 보인다.
        operation_id dismissed_banner { 0 };
        bool banner_expanded { false };

        [[nodiscard]] bool section_collapsed(const sidebar_section section) const noexcept
        {
            return (collapsed_sections & (1u << static_cast<unsigned>(section))) != 0;
        }
    };

    // Clone 대화 상자다. 열려 있는 동안만 값이 있다.
    struct clone_dialog_state
    {
        luil::text::text_edit_state url {};
        luil::text::text_edit_state path {};
        // 낸 clone operation이다. 0이면 아직 입력 중이다.
        operation_id operation { 0 };
        std::u8string error {};
    };

    // 화면에 띄운 안내 한 줄이다 (아직 없는 대화 상자의 자리).
    struct notice_state
    {
        std::u8string text {};
        bool warning { false };
    };

    struct ui_state
    {
        std::map<repository_tab_id, tab_ui_state> tabs {};
        float sidebar_width { default_sidebar_width };
        float detail_width { default_detail_width };
        bool operation_log_open { false };
        float log_scroll { 0.0f };
        float tab_scroll { 0.0f };
        luil::text::text_edit_state start_search {};
        float start_scroll { 0.0f };
        // medium·narrow에서 상세 서랍이 열려 있다.
        bool drawer_open { false };
        // narrow의 아이콘 막대에서 연 구역이다. 떠 있는 판으로 그 구역만 보인다.
        std::optional<sidebar_section> rail_section {};
        std::optional<clone_dialog_state> clone {};
        std::optional<notice_state> notice {};
        // 시작 페이지 배너에서 닫은 경로 검사·init 실패다.
        operation_id dismissed_start_banner { 0 };
        // IME 조합 중인 칸의 표시 글이다. 한 번에 한 칸만 조합한다.
        std::optional<luil::text_composition_event> composition {};
    };

    [[nodiscard]] inline luil::text_input_target to_luil_target(const text_target target) noexcept
    {
        return static_cast<luil::text_input_target>(static_cast<std::uint32_t>(target));
    }

    [[nodiscard]] inline text_target from_luil_target(const luil::text_input_target target) noexcept
    {
        const auto value { static_cast<std::uint32_t>(target) };
        if (value > static_cast<std::uint32_t>(text_target::clone_path))
            return text_target::none;
        return static_cast<text_target>(value);
    }
} // namespace cyella
