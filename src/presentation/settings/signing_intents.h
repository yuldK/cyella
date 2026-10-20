#pragma once

#include "domain/signing_config.h"
#include "infrastructure/signing_info.h"

#include <optional>
#include <string>

namespace cyella {
    // 환경 설정 "서명" 화면이 내는 메시지다. 커밋 상자의 서명 토글은 `presentation/signing_intents.h`다.

    // worker가 읽은 서명 설정(실효·지역·전역)과 gpg 키 목록이다 (driver가 relay에서 받아 controller에 넘긴다).
    struct signing_info_loaded_intent
    {
        signing_info_snapshot info {};
    };

    // worker가 설정을 쓰고 끝났다.
    struct signing_write_done_intent
    {
        bool ok { false };
        // 전역 설정을 바꾼 쓰기다 (열린 모든 저장소가 영향을 받는다).
        bool global { false };
        std::u8string error {};
    };

    struct refresh_signing_intent
    {};

    // 바꿀 범위를 고른다: 이 저장소(`git config --local`) 또는 전역(`--global`).
    struct select_signing_scope_intent
    {
        bool global { false };
    };

    enum class signing_toggle_choice
    {
        enabled,
        disabled,
        // 이 범위의 설정을 지운다 (아래 범위의 값이 다시 보인다).
        unset,
    };

    // `commit.gpgsign`·`tag.gpgsign`을 켜거나 끄거나 지운다.
    struct set_signing_toggle_intent
    {
        signing_key_id key { signing_key_id::commit_gpgsign };
        signing_toggle_choice choice { signing_toggle_choice::enabled };
    };

    // `gpg.format`. 값이 없으면 지운다 (git은 openpgp를 쓴다).
    struct set_signing_format_intent
    {
        std::optional<signing_format> format {};
    };

    // `user.signingkey`. ssh는 공개 키(.pub) 경로, openpgp는 지문이다. 비어 있으면 지운다.
    struct choose_signing_key_intent
    {
        std::u8string value {};
    };

    // `gpg.program`·`gpg.ssh.program`·`gpg.ssh.allowedSignersFile`의 입력 칸을 적용하거나(clear가 거짓) 이 범위의 설정을 지운다.
    struct apply_signing_text_intent
    {
        signing_key_id key { signing_key_id::gpg_program };
        bool clear { false };
    };

    // `gpg.program`을 Git for Windows의 gpg 경로로 정한다.
    struct use_default_gpg_intent
    {};

    // 전역 설정을 바꾸는 요청을 확인하거나 취소한다.
    struct confirm_global_signing_intent
    {};

    struct cancel_global_signing_intent
    {};
} // namespace cyella
