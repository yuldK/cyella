#pragma once

#include "domain/hosting_models.h"
#include "presentation/dialog_state.h"
#include "presentation/form_builders.h"
#include "presentation/form_view.h"

#include <string>

namespace cyella {
    // PR·MR 만들기 대화 상자다. `form_builders`와 같은 규칙의 순수 함수들이다.
    //
    // `dialog_state`를 이렇게 쓴다: `title`=대화 상자 제목, `subject`=원본(현재) 브랜치, `lines`=안내 글, `choices[0]`=먼저 push할 원격,
    // `flag_a`=초안으로 만들기, `flag_b`=먼저 push하기, `flag_c`=push가 필요하다(원격에 없거나 앞서 있다).
    // 칸: 0 = 대상 브랜치, 1 = 제목, 2 = 본문.

    [[nodiscard]] bool create_pull_request_ready(const dialog_state& dialog, const form_inputs& inputs, std::u8string* reason = nullptr);
    [[nodiscard]] form_view build_create_pull_request_form(const dialog_state& dialog, const form_build_context& context);
    // 칸의 글에서 API로 보낼 값이다. ready가 아니면 쓰지 않는다.
    [[nodiscard]] create_pull_request_params make_create_pull_request_params(const dialog_state& dialog, const form_inputs& inputs);
} // namespace cyella
