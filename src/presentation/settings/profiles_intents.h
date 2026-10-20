#pragma once

#include "domain/git_identity.h"

#include <cstddef>
#include <string>

namespace cyella {
    // 저장소별 사용자 프로필의 화면 메시지다.

    // ---- worker가 돌려주는 결과 (driver가 relay에서 받아 controller에 넘긴다)

    // 저장소의 작성자 신원을 읽었다. `ok`가 거짓이면 git을 실행하지 못했거나 오류로 끝난 것이고, 신원이 비어 있는 것은 실패가 아니다.
    struct identity_loaded_intent
    {
        std::string repository_id {};
        git_identity identity {};
        bool ok { false };
        std::u8string error {};
    };

    // `git config --local`로 프로필을 쓴 결과다.
    struct identity_applied_intent
    {
        std::string repository_id {};
        bool ok { false };
        std::u8string message {};
    };

    // ---- 프로필을 선택된 저장소에 적용한다 (설정 화면의 단추, 팔레트, 상태 줄, 제안 배너가 같은 메시지를 낸다)
    struct apply_profile_intent
    {
        std::u8string name {};
    };

    // ---- 환경 설정의 "사용자 프로필" 쪽
    struct add_profile_intent
    {};

    struct edit_profile_intent
    {
        std::size_t index { 0 };
    };

    struct remove_profile_intent
    {
        std::size_t index { 0 };
    };

    struct save_profile_intent
    {};

    struct cancel_profile_edit_intent
    {};

    // 규칙을 더하려고 고른 프로필이다.
    struct choose_rule_profile_intent
    {
        std::u8string name {};
    };

    struct add_profile_rule_intent
    {};

    struct remove_profile_rule_intent
    {
        std::size_t index { 0 };
    };
} // namespace cyella
