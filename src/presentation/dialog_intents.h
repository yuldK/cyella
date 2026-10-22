#pragma once

#include "application/git_operation.h"
#include "domain/operation_status.h"
#include "presentation/menu_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cyella {
    // 대화 상자·메뉴·끌어 놓기·충돌 해결이 내는 화면 메시지다. `ui_intents.h`의 짝이고, controller가 처리한다.

    // ------------------------------------------------------------------ 메뉴

    // 우클릭이다. 좌표는 창 좌표의 물리 픽셀이다 (`ui_action_context`가 준 값).
    struct open_context_menu_intent
    {
        menu_target target {};
        float x { 0.0f };
        float y { 0.0f };
    };

    // 메뉴 키(Shift+F10, 메뉴 키)다. 고른 사이드바 항목이나 커밋의 메뉴를 연다.
    struct open_menu_for_selection_intent
    {};

    struct close_menu_intent
    {};

    struct open_ref_list_intent
    {
        std::string sha {};
        float x { 0.0f };
        float y { 0.0f };
    };

    struct close_ref_list_intent
    {};

    struct scroll_ref_list_intent
    {
        float value { 0.0f };
        bool absolute { false };
    };

    struct menu_select_intent
    {
        std::u8string key {};
    };

    // ------------------------------------------------------------------ 끌어 놓기

    // 브랜치를 다른 브랜치나 커밋에 놓았다. 좌표는 메뉴를 놓을 자리(물리 픽셀)다.
    struct drop_intent
    {
        drop_party source {};
        drop_party destination {};
        float x { 0.0f };
        float y { 0.0f };
    };

    // 커밋 행을 두 번 눌렀다. 그 커밋의 브랜치로 체크아웃하고, 없으면 detached 체크아웃을 묻는다.
    struct activate_commit_intent
    {
        std::string sha {};
    };

    // ------------------------------------------------------------------ 모달 입력 상자

    // 선택지(라디오)를 골랐다. `index`는 `form_view::choices`의 자리다.
    struct form_choose_intent
    {
        std::size_t index { 0 };
    };

    // 체크 칸을 뒤집었다. 인증 상자의 0번은 "입력 보이기"다.
    struct form_check_intent
    {
        std::size_t index { 0 };
    };

    // 단추를 눌렀다. `key`는 `form_button::key`다. Esc와 닫기 단추는 `form_view::dismiss_key`를 보낸다.
    struct form_button_intent
    {
        std::u8string key {};
    };

    // 칸에서 Enter를 눌렀다. 기본 단추가 켜져 있으면 그것을 누른 것과 같다.
    struct form_enter_intent
    {};

    struct form_scroll_intent
    {
        float value { 0.0f };
        bool absolute { false };
    };

    // ------------------------------------------------------------------ 열기

    // 버리기 같은 확인이 필요한 동작을 확인 상자로 연다. hunk 버리기도 이 길을 쓴다.
    struct open_confirm_intent
    {
        std::u8string title {};
        std::vector<std::u8string> lines {};
        std::u8string confirm_label {};
        bool danger { false };
        std::vector<git_operation> operations {};
    };

    struct discard_files_intent
    {
        std::vector<std::string> paths {};
        // 추적하지 않는 파일이다. 확인 상자가 "휴지통으로 보냅니다"를 말하고 `trash_files_request`를 낸다.
        bool untracked { false };
    };

    // 툴바와 메뉴가 여는 대화 상자다.
    struct open_branch_dialog_intent
    {
        // 비우면 HEAD다.
        std::string at {};
    };

    // ------------------------------------------------------------------ 알림·충돌

    // 알림의 조치 단추를 눌렀다. `id`는 실패 알림의 operation이고(안내는 0), `notification`은 누른 알림이다.
    // `notification`이 0이면 키(와 operation)가 맞는 가장 최근 알림이다.
    struct notification_action_intent
    {
        std::u8string key {};
        operation_id id { 0 };
        std::uint64_t notification { 0 };
    };

    enum class conflict_action
    {
        continue_sequence,
        skip_sequence,
        abort_sequence,
    };

    struct conflict_action_intent
    {
        conflict_action action { conflict_action::continue_sequence };
    };
} // namespace cyella
