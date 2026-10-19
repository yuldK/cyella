#pragma once

namespace cyella {
    // 창 폭에 따라 셸이 고르는 배치다.
    enum class layout_mode
    {
        // 1200px 이상. 사이드바·그래프·상세 패널 3단이다.
        wide,
        // 760~1199px. 상세 패널이 접히고 툴바의 "상세 패널" 단추가 오른쪽 서랍으로 연다.
        medium,
        // 760px 미만. 사이드바가 44px 아이콘 막대가 되고 그래프가 작성자·날짜 열을 감춘다.
        narrow,
    };

    // 셸의 고정 치수다 (논리 픽셀).
    inline constexpr float shell_tab_row_height { 32.0f };
    inline constexpr float shell_toolbar_height { 44.0f };
    inline constexpr float shell_status_height { 24.0f };
    inline constexpr float shell_banner_height { 32.0f };
    inline constexpr float shell_log_height { 200.0f };
    inline constexpr float shell_rail_width { 44.0f };
    inline constexpr float shell_splitter_width { 6.0f };
    inline constexpr float default_sidebar_width { 232.0f };
    inline constexpr float default_detail_width { 360.0f };
    inline constexpr float minimum_sidebar_width { 160.0f };
    inline constexpr float maximum_sidebar_width { 420.0f };
    inline constexpr float minimum_detail_width { 280.0f };
    inline constexpr float maximum_detail_width { 560.0f };
    // 그래프는 어느 폭에서도 남는다. 판을 넓혀도 이 폭은 지킨다.
    inline constexpr float minimum_graph_width { 240.0f };

    // 폭의 경계값이다 (논리 픽셀).
    inline constexpr float wide_layout_minimum_width { 1200.0f };
    inline constexpr float medium_layout_minimum_width { 760.0f };
    inline constexpr float toolbar_label_minimum_width { 960.0f };
    inline constexpr float repository_selector_minimum_width { 480.0f };

    [[nodiscard]] layout_mode select_layout_mode(float logical_width) noexcept;
    // 툴바 단추 아래의 글자를 보이는가. 960px 아래에서는 아이콘만 남긴다.
    [[nodiscard]] bool toolbar_shows_labels(float logical_width) noexcept;
    // 툴바의 저장소 선택 상자를 보이는가. 480px 아래에서 숨긴다.
    [[nodiscard]] bool toolbar_shows_repository_selector(float logical_width) noexcept;

    struct shell_rect
    {
        float x { 0.0f };
        float y { 0.0f };
        float width { 0.0f };
        float height { 0.0f };

        [[nodiscard]] bool operator==(const shell_rect&) const noexcept = default;
        [[nodiscard]] bool empty() const noexcept
        {
            return width <= 0.0f || height <= 0.0f;
        }
    };

    struct shell_layout_input
    {
        float width { 0.0f };
        float height { 0.0f };
        float caption_height { 0.0f };
        // 저장소 탭이 선택돼 툴바와 3단 본문이 서는가. 시작 탭이면 본문 하나다.
        bool repository { false };
        // 툴바 아래 배너(실패·입력 필요·안내)의 높이다. 0이면 배너가 없다.
        float banner_height { 0.0f };
        bool log_open { false };
        layout_mode mode { layout_mode::wide };
        float sidebar_width { default_sidebar_width };
        float detail_width { default_detail_width };
        // medium·narrow에서 상세 서랍이 열려 있다.
        bool drawer_open { false };
    };

    // 셸의 판 자리다 (논리 픽셀, 창 원점 기준). 없는 판은 빈 사각형이다.
    struct shell_rects
    {
        shell_rect tab_row {};
        shell_rect toolbar {};
        shell_rect banner {};
        // 사이드바·그래프·상세가 놓이는 본문 전체다. 시작 탭이면 시작 페이지가 여기에 선다.
        shell_rect body {};
        // wide·medium의 사이드바 또는 narrow의 아이콘 막대다.
        shell_rect sidebar {};
        shell_rect sidebar_splitter {};
        shell_rect graph {};
        shell_rect detail_splitter {};
        // wide에서는 3단의 오른쪽 판이고, 서랍이 열린 medium·narrow에서는 그래프 위에 겹치는 서랍이다.
        shell_rect detail {};
        bool detail_is_drawer { false };
        shell_rect log {};
        shell_rect status {};
    };

    // 판 자리를 계산하는 순수 함수다. 배치·test·스크롤 한계가 같은 식을 쓴다.
    [[nodiscard]] shell_rects compute_shell_layout(const shell_layout_input& input) noexcept;

    // 끌어서 바꾼 판 폭을 범위 안으로 다듬는다. 창이 좁으면 그래프 최소 폭을 먼저 지킨다.
    [[nodiscard]] float clamp_sidebar_width(float width) noexcept;
    [[nodiscard]] float clamp_detail_width(float width) noexcept;
} // namespace cyella
