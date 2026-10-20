#pragma once

#include "infrastructure/signing_info.h"

#include "luil/text/text_edit.h"

#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 환경 설정 "서명" 쪽이 열려 있는 동안만 사는 화면 상태다 (`signing_page_controller`가 소유한다). 설정의 정본은 git이고, 여기에는
    // 읽어 온 값과 바꾸려는 범위, 입력 초안만 둔다.

    // 확인을 기다리는 쓰기다. 전역 설정은 사용자의 실제 git 설정이라 확인을 받은 뒤에만 쓴다.
    struct pending_signing_write
    {
        bool global { false };
        std::vector<signing_change> changes {};
        // 확인 문구에 보일 한 줄이다 ("commit.gpgsign을 켭니다").
        std::u8string summary {};
    };

    struct signing_page_state
    {
        // worker가 읽은 값이다. 열자마자 읽고 [새로고침]이나 쓰기 뒤에 다시 읽는다.
        std::optional<signing_info_snapshot> info {};
        bool loading { false };
        bool writing { false };
        // 바꿀 범위다. 거짓이면 이 저장소(`--local`), 참이면 전역(`--global`)이다. 저장소가 없으면 전역뿐이다.
        bool global_scope { false };
        // 사용자가 범위를 직접 골랐다. 고르기 전에는 저장소가 있으면 이 저장소다.
        bool scope_chosen { false };
        std::optional<pending_signing_write> pending {};
        // 프로그램 경로와 allowed signers 입력 초안이다. 읽어 온 값(고른 범위의 값)으로 채우고 [적용]을 눌러야 쓴다.
        luil::text::text_edit_state gpg_program {};
        luil::text::text_edit_state ssh_program {};
        luil::text::text_edit_state allowed_signers {};
        // 화면 아래의 한 줄 안내다.
        std::u8string message {};
        bool message_error { false };
    };
} // namespace cyella
