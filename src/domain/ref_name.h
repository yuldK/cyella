#pragma once

#include <optional>
#include <string_view>

namespace cyella {
    // `git check-ref-format`의 핵심 규칙이다. 사용자가 입력한 이름이 git에 넘어가기 전에
    // 같은 규칙으로 걸러 오류 문구를 앱이 만든다. 모든 경우를 git과 일치시키려는 것이
    // 아니라, git이 거절하거나 옵션으로 해석할 이름을 앞서 막는 것이 목적이다. 마지막
    // 판단은 항상 git이 한다.
    enum class ref_name_problem
    {
        empty,
        leading_dash,
        leading_slash,
        trailing_slash,
        consecutive_slashes,
        component_starts_with_dot,
        component_ends_with_lock,
        trailing_dot,
        contains_double_dot,
        contains_at_brace,
        is_single_at,
        is_head,
        forbidden_character,
        control_character,
        backslash,
    };

    // 새로 만들 브랜치·태그 이름(`feature/x`처럼 `refs/heads/` 뒤 부분)을 검사한다.
    // 문제가 없으면 값이 없다.
    [[nodiscard]] std::optional<ref_name_problem> find_ref_name_problem(std::string_view name) noexcept;

    // 이미 있는 ref나 커밋을 가리키는 식(`HEAD~3`, `origin/main`, SHA)이 옵션으로
    // 해석되거나 명령줄을 깨뜨리지 않는지만 본다. 식의 문법 전체를 검사하지 않는다.
    [[nodiscard]] std::optional<ref_name_problem> find_revision_problem(std::string_view revision) noexcept;

    // 로그와 예외 메시지에 쓰는 영어 진단 문구다.
    [[nodiscard]] std::string_view ref_name_problem_text(ref_name_problem problem) noexcept;
} // namespace cyella
