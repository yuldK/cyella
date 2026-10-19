#pragma once

#include "application/git_operation.h"
#include "infrastructure/ssh_info.h"

#include "luil/text/text_edit.h"

#include <cstddef>
#include <optional>
#include <string>

namespace cyella {
    // 환경 설정 화면이 열려 있는 동안만 사는 화면 상태다 (`settings_controller`가 소유한다). 설정의 정본은
    // `app_settings_service`이고, 여기에는 저장 전의 입력 초안과 도구 실행 결과 같은 껐다 켜면 잃어도 되는 것만 둔다.

    enum class settings_page
    {
        general,
        diff_tools,
        ssh,
    };

    // 외부 diff 도구 한 줄을 고치는 중이다.
    struct tool_edit_state
    {
        // 표에서 고치는 줄이다. 값이 없으면 새로 더하는 줄이다.
        std::optional<std::size_t> index {};
        // 쉼표나 공백으로 가른 확장자 목록이다 (`.png, .psd`).
        luil::text::text_edit_state extensions {};
        luil::text::text_edit_state executable {};
        luil::text::text_edit_state arguments {};
    };

    struct settings_state
    {
        settings_page page { settings_page::general };
        float scroll { 0.0f };

        // 일반: git 실행 파일 경로의 입력 초안이다. 설정값으로 채워 열고, [적용]을 눌러야 저장한다.
        luil::text::text_edit_state git_path {};
        std::optional<tool_edit_state> tool_edit {};

        // SSH: 키 만들기와 연결 시험의 입력이다.
        luil::text::text_edit_state key_name {};
        // 이름 칸에 우리가 채워 둔 값이다. 사용자가 고치지 않은 동안만 목록이 바뀔 때 다시 고른다.
        std::u8string suggested_key_name {};
        luil::text::text_edit_state key_comment {};
        luil::text::text_edit_state test_host {};
        // 마지막으로 낸 작업이다. 상태와 결과는 workspace의 operation 표에서 읽는다.
        operation_id keygen_operation { 0 };
        std::u8string keygen_target {};
        operation_id test_operation { 0 };
        std::u8string test_target {};

        // `~/.ssh`와 agent 조회 결과다. 열거나 [새로고침]을 누를 때 worker가 읽어 온다.
        std::optional<ssh_info_snapshot> ssh {};
        bool ssh_loading { false };

        // 화면 아래의 한 줄 안내다 (공개 키를 복사했다 등).
        std::u8string message {};
    };
} // namespace cyella
