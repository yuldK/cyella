#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cyella {
    // submodule 하나의 상태다. 사이드바 SUBMODULES 구역의 점·문구와 메뉴 조건이 이 값을 읽는다.
    enum class submodule_state
    {
        // 체크아웃되어 있고 상위 저장소가 기록한 커밋과 같다. 내용도 깨끗하다.
        initialized,
        // 아직 `submodule update --init`을 하지 않았다. 작업 폴더가 비어 있다.
        uninitialized,
        // 기록된 커밋과 같은 곳에 있지만 submodule 안에 커밋하지 않은 변경이 있거나,
        // 기록보다 앞선 커밋을 체크아웃했다(상위 저장소에서 포인터를 커밋하면 된다).
        modified,
        // 체크아웃한 커밋이 기록과 다르고 기록한 커밋의 후손도 아니다. 상위 저장소가 가리키는 커밋을
        // 아직 받지 못했거나 뒤처져 있다. `update`로 기록한 커밋에 맞춘다.
        out_of_date,
        // 병합 충돌 중이다.
        conflict,
    };

    struct submodule_info
    {
        // 최상위 저장소 루트 기준 경로다(`/` 구분). 중첩 submodule은 부모 경로가 앞에 붙는다.
        std::string path {};
        // `.gitmodules`의 구역 이름이다. 보통 경로와 같다. 중첩 submodule은 비어 있다.
        std::string name {};
        // `.gitmodules`의 url이다. 읽지 못했으면 비어 있다.
        std::string url {};
        // 상위 저장소의 index가 기록한 커밋이다. 모르면(중첩) 비어 있다.
        std::string recorded_sha {};
        // submodule에서 체크아웃된 커밋이다. 초기화하지 않았으면 비어 있다.
        std::string checked_out_sha {};
        // `git submodule status`의 괄호 안 설명이다(`heads/main`, `v1.0-3-gabcdef`). 없으면 비어 있다.
        std::string description {};
        submodule_state state { submodule_state::initialized };
        // 중첩 단계다. 최상위 submodule은 0이다.
        std::uint32_t depth { 0 };
    };

    [[nodiscard]] std::string_view submodule_state_name(submodule_state state) noexcept;
} // namespace cyella
