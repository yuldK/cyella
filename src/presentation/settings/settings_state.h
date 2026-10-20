#pragma once

#include "application/git_operation.h"
#include "domain/hosting_remote.h"
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
        // GitHub·GitLab 호스트 표와 로그인 상태다.
        hosting,
        // 새 쪽은 끝에 덧붙인다.
        profiles,
        shortcuts,
        // 커밋 서명 설정이다. 상태는 `signing_page_controller`가 따로 소유한다.
        signing,
    };

    // 단축키 하나를 고치는 중이다. 입력 칸의 글은 ` / `로 이은 정규 표기(`Ctrl+Shift+P / Ctrl+P`)다. 실제 키를 누르면 그 키로 바뀌고,
    // 손으로 쳐도 된다 (보조 기술은 칸에 글을 넣는다).
    struct shortcut_edit_state
    {
        std::string command {};
        luil::text::text_edit_state text {};
    };

    // 사용자 프로필 한 줄을 고치는 중이다.
    struct profile_edit_state
    {
        // 표에서 고치는 줄이다. 값이 없으면 새로 더하는 줄이다.
        std::optional<std::size_t> index {};
        luil::text::text_edit_state name {};
        luil::text::text_edit_state user_name {};
        luil::text::text_edit_state user_email {};
        luil::text::text_edit_state signing_key {};
    };

    // 프로필 적용 규칙을 더하는 입력이다 (폴더 접두와 고른 프로필).
    struct profile_rule_draft
    {
        luil::text::text_edit_state prefix {};
        std::u8string profile {};
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

        // 호스팅: 표에 더할 호스트와 API 주소의 입력 초안, 고른 서비스 종류다.
        luil::text::text_edit_state hosting_host {};
        luil::text::text_edit_state hosting_api_base {};
        hosting_provider hosting_new_provider { hosting_provider::github };
        // 마지막으로 더하려다 막힌 이유다.
        std::u8string hosting_error {};
        // 단축키 쪽에서 고치는 중인 명령과, 사용자 프로필 쪽의 고치는 줄·규칙 입력이다.
        std::optional<shortcut_edit_state> shortcut_edit {};
        std::optional<profile_edit_state> profile_edit {};
        profile_rule_draft rule_draft {};

        // 화면 아래의 한 줄 안내다 (공개 키를 복사했다 등).
        std::u8string message {};
    };
} // namespace cyella
