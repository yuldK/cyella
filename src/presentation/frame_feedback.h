#pragma once

#include <cstddef>
#include <optional>

namespace cyella {
    // tree를 배치한 뒤에야 아는 값이다 (흘릴 수 있는 최대치, 그래프 창 높이). driver가 controller에
    // 되돌려 다음 스크롤 메시지를 다듬는 데 쓴다. luil 목록은 범위 밖 offset을 배치에서만 다듬으므로,
    // 앱이 값을 다듬지 않으면 넘친 휠 양이 쌓여 되돌아오는 데 그만큼 더 굴려야 한다.
    struct frame_feedback
    {
        std::optional<float> graph_maximum {};
        std::optional<float> sidebar_maximum {};
        std::optional<float> detail_maximum {};
        std::optional<float> commit_meta_maximum {};
        std::optional<float> working_maximum {};
        std::optional<float> diff_maximum {};
        std::optional<float> start_maximum {};
        std::optional<float> log_maximum {};
        std::optional<float> settings_maximum {};
        std::optional<float> form_maximum {};
        std::optional<float> palette_maximum {};
        // 커서를 드러내도록 배치한 팔레트 목록의 실제 위치다.
        std::optional<float> palette_scroll {};
        std::optional<float> tab_scroll {};
        // 그래프 창 높이다 (논리 픽셀). 끝에 가까워지면 다음 쪽을 읽는 판정이 쓴다.
        float graph_viewport { 0.0f };
    };
} // namespace cyella
