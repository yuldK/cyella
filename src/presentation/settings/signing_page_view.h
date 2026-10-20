#pragma once

#include "application/file_probe.h"
#include "domain/signing_config.h"
#include "infrastructure/ssh_info.h"
#include "presentation/settings/signing_page_state.h"
#include "presentation/ui_state.h"

#include "luil/ui/text_input_state.h"

#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 환경 설정 "서명" 쪽의 snapshot이다. `build_signing_page_view`가 상태와 읽어 온 값에서 짓는 순수 함수의 결과이고,
    // `ui::build_signing_page`가 이 값만 읽어 luil tree를 짓는다.

    // 설정 하나의 현재 값이다. 적용되는 값(실효)과 이 저장소·전역에서 각각 정한 값을 나란히 보인다.
    struct signing_setting_row_view
    {
        signing_key_id key { signing_key_id::commit_gpgsign };
        std::u8string name {};
        // 적용되는 값이다 ("켜짐", "ssh", 경로). 설정이 없으면 git의 기본 동작이다 ("꺼짐 (기본)").
        std::u8string effective {};
        // 적용되는 값이 어디서 왔는가 ("이 저장소", "전역", "설정 없음").
        std::u8string origin {};
        std::u8string local {};
        std::u8string global {};
    };

    // 서명 키 후보 한 줄이다. ssh는 `~/.ssh`의 공개 키, openpgp는 gpg 비밀 키다.
    struct signing_key_row_view
    {
        std::u8string label {};
        std::u8string detail {};
        // `user.signingkey`에 쓸 값이다 (ssh는 공개 키 경로, openpgp는 지문).
        std::u8string value {};
        // 고른 범위의 `user.signingkey`와 같다.
        bool selected { false };
        // 만료·폐기되었거나 서명할 수 없는 키는 고르지 못하게 한다.
        bool usable { true };
    };

    // 프로그램 경로 입력 칸 하나다.
    struct signing_text_view
    {
        luil::text_input_view input {};
        // 고른 범위에 저장된 값이다. 비어 있으면 이 범위에서는 정하지 않았다.
        std::u8string stored {};
        // 입력을 지금 적용할 수 있는가 (저장된 값과 다르고 문제가 없다).
        bool can_apply { false };
        // 이 범위의 설정을 지울 수 있는가 (저장된 값이 있다).
        bool can_clear { false };
        // 적용을 막는 문제 또는 알려 둘 것이다.
        std::u8string error {};
        std::u8string warning {};
    };

    struct signing_page_view
    {
        bool loaded { false };
        bool loading { false };
        bool writing { false };
        // 이 저장소 범위를 쓸 수 있다 (저장소 탭이 열려 있다).
        bool repository { false };
        std::u8string repository_path {};
        // 바꿀 범위다.
        bool global_scope { false };

        std::vector<signing_setting_row_view> rows {};

        // 고른 범위에서 정한 값이다. "enabled"·"disabled"·"unset" / "openpgp"·"ssh"·"unset".
        std::u8string commit_choice {};
        std::u8string tag_choice {};
        std::u8string format_choice {};

        // 키 고르기다. 목록의 종류는 고른 범위의 결과 형식을 따른다.
        signing_format key_format { signing_format::openpgp };
        std::vector<signing_key_row_view> keys {};
        // 목록이 비었거나 읽지 못한 이유다.
        std::u8string keys_message {};
        // 고른 범위의 `user.signingkey`다 (후보에 없는 값이어도 보인다).
        std::u8string current_key {};

        signing_text_view gpg_program {};
        signing_text_view ssh_program {};
        signing_text_view allowed_signers {};
        // Git for Windows의 gpg 경로다. 못 찾았으면 비어 있다.
        std::u8string default_gpg {};
        // 지금 쓰이는 gpg와 상태다 ("키 2개를 읽었습니다", 읽지 못한 이유).
        std::u8string gpg_status {};
        bool gpg_ok { false };
        // allowed signers 파일 상태다 ("파일이 있습니다", "파일이 없습니다").
        std::u8string allowed_signers_status {};
        bool allowed_signers_ok { false };

        // 전역 설정을 바꾸는 요청이 확인을 기다린다.
        bool confirming { false };
        std::u8string confirm_text {};

        std::u8string message {};
        bool message_error { false };
    };

    struct signing_page_view_inputs
    {
        const signing_page_state* state { nullptr };
        // `~/.ssh`의 키 목록이다. 아직 읽지 않았으면 null이다.
        const ssh_info_snapshot* ssh { nullptr };
        const file_probe* probe { nullptr };
        std::u8string repository_path {};
        std::optional<luil::text_composition_event> composition {};
    };

    [[nodiscard]] signing_page_view build_signing_page_view(const signing_page_view_inputs& inputs);

    // ---- 보조 함수 (test가 직접 부른다)

    // 설정 값을 화면 글로 바꾼다. 불리언은 "켜짐"·"꺼짐", 나머지는 값 그대로, 없으면 빈 글이다.
    [[nodiscard]] std::u8string describe_signing_value(signing_key_id key, const signing_entry& entry);
    // 적용되는 값이다. 설정이 없으면 git의 기본 동작을 괄호로 붙인다 ("꺼짐 (기본)").
    [[nodiscard]] std::u8string describe_effective_signing_value(signing_key_id key, const signing_entry& entry);
    // 프로그램 경로·allowed signers 입력의 검사 결과다. 비어 있으면(이 범위의 설정을 지움) 문제가 없다.
    [[nodiscard]] std::u8string check_signing_path(std::u8string_view text, bool allow_home_prefix);
} // namespace cyella
