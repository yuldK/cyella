#pragma once

#include "domain/app_settings.h"
#include "infrastructure/ssh_info.h"
#include "presentation/settings/settings_state.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace cyella {
    // 환경 설정 화면이 내는 메시지다. 저장하는 값(테마·git 경로·도구 표)은 `app_settings_service`를 거치고, 나머지는
    // `settings_state`만 바꾼다.

    struct open_settings_intent
    {
        settings_page page { settings_page::general };
    };

    struct close_settings_intent
    {};

    struct select_settings_page_intent
    {
        settings_page page { settings_page::general };
    };

    struct settings_scroll_intent
    {
        float value { 0.0f };
        // 참이면 `value`가 자리, 아니면 변화량이다 (논리 픽셀).
        bool absolute { false };
    };

    // ------------------------------------------------------------------ 일반

    struct set_theme_intent
    {
        theme_preference theme { theme_preference::system };
    };

    struct apply_font_settings_intent
    {};

    struct select_font_intent
    {
        bool code { false };
        std::u8string family {};
    };

    struct open_font_list_intent
    {
        font_list_state list {};
    };

    struct close_font_list_intent
    {};

    struct font_list_scroll_intent
    {
        float value { 0.0f };
        bool absolute { false };
    };

    // 커밋 작성자 사진을 Gravatar에서 받을지다. 바로 저장한다.
    struct set_gravatar_intent
    {
        bool on { false };
    };

    // 입력한 git 경로를 설정에 저장한다. 비어 있으면 자동 탐색으로 돌아간다.
    struct apply_git_path_intent
    {};

    // 입력을 비우고 자동 탐색으로 돌아간다.
    struct reset_git_path_intent
    {};

    // ------------------------------------------------------------------ diff 보기

    // diff를 처음 열 때의 보기다. 고르면 열려 있는 diff도 바로 그 보기로 바뀐다.
    struct set_diff_layout_default_intent
    {
        diff_layout_preference layout { diff_layout_preference::inline_view };
    };

    // hunk 둘레의 문맥 줄 수다. 다음에 여는 diff부터 `git diff -U<n>`에 쓴다.
    struct set_diff_context_lines_intent
    {
        std::uint32_t lines { default_diff_context_lines };
    };

    // ------------------------------------------------------------------ diff 도구

    struct add_tool_intent
    {
        bool file_app { false };
    };

    struct edit_tool_intent
    {
        std::size_t index { 0 };
        bool file_app { false };
    };

    struct remove_tool_intent
    {
        std::size_t index { 0 };
        bool file_app { false };
    };

    struct save_tool_intent
    {};

    struct cancel_tool_edit_intent
    {};

    // 인자 템플릿 끝에 `{left}` 같은 토큰을 붙인다.
    struct insert_tool_token_intent
    {
        std::u8string token {};
    };

    // ------------------------------------------------------------------ 파일 고르기 (UI thread 대화 상자의 결과)

    enum class file_purpose
    {
        git_executable,
        tool_executable,
    };

    // 취소했으면 오지 않는다.
    struct settings_file_chosen_intent
    {
        file_purpose purpose { file_purpose::git_executable };
        std::u8string path {};
    };

    // ------------------------------------------------------------------ SSH

    struct refresh_ssh_intent
    {};

    // worker가 읽은 agent·키 목록이다 (driver가 relay에서 받아 controller에 넘긴다).
    struct ssh_info_loaded_intent
    {
        ssh_info_snapshot info {};
    };

    struct create_ssh_key_intent
    {};

    struct copy_public_key_intent
    {
        std::u8string name {};
    };

    struct test_ssh_connection_intent
    {};

    // ------------------------------------------------------------------ 호스팅

    // 입력한 호스트·API 주소를 호스팅 표에 더한다.
    struct add_hosting_host_intent
    {};

    // 호스팅 표에서 호스트 한 줄을 지운다 (내장 호스트는 지울 수 없다).
    struct remove_hosting_host_intent
    {
        std::string host {};
    };

    // 새로 더할 호스트의 서비스 종류를 고른다.
    struct set_new_hosting_provider_intent
    {
        hosting_provider provider { hosting_provider::github };
    };
} // namespace cyella
