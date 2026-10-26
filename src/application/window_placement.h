#pragma once

namespace cyella {
    // 저장했다가 다음 실행에 되돌리는 주 창의 배치다. luil이 알려 주는 일반 창 사각형 그대로다 (작업 영역 좌표, 물리 픽셀).
    // 화면 밖이거나 최소 크기보다 작은 값은 창을 만들 때 luil이 버린다 — 여기서 고치지 않는다.
    struct saved_window_placement
    {
        int x { 0 };
        int y { 0 };
        int width { 0 };
        int height { 0 };
        bool maximized { false };

        [[nodiscard]] bool valid() const noexcept
        {
            return width > 0 && height > 0;
        }

        [[nodiscard]] bool operator==(const saved_window_placement&) const noexcept = default;
    };
} // namespace cyella
