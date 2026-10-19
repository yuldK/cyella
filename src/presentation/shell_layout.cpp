#include "presentation/shell_layout.h"

#include <algorithm>

namespace cyella {
    layout_mode select_layout_mode(const float logical_width) noexcept
    {
        if (logical_width >= wide_layout_minimum_width)
            return layout_mode::wide;
        if (logical_width >= medium_layout_minimum_width)
            return layout_mode::medium;
        return layout_mode::narrow;
    }

    bool toolbar_shows_labels(const float logical_width) noexcept
    {
        return logical_width >= toolbar_label_minimum_width;
    }

    bool toolbar_shows_repository_selector(const float logical_width) noexcept
    {
        return logical_width >= repository_selector_minimum_width;
    }

    float clamp_sidebar_width(const float width) noexcept
    {
        return std::clamp(width, minimum_sidebar_width, maximum_sidebar_width);
    }

    float clamp_detail_width(const float width) noexcept
    {
        return std::clamp(width, minimum_detail_width, maximum_detail_width);
    }

    shell_rects compute_shell_layout(const shell_layout_input& input) noexcept
    {
        shell_rects rects {};
        const float width { std::max(input.width, 0.0f) };
        const float height { std::max(input.height, 0.0f) };
        float top { input.caption_height };

        rects.tab_row = { 0.0f, top, width, shell_tab_row_height };
        top += shell_tab_row_height;
        if (input.repository)
        {
            rects.toolbar = { 0.0f, top, width, shell_toolbar_height };
            top += shell_toolbar_height;
        }
        if (input.banner_height > 0.0f)
        {
            rects.banner = { 0.0f, top, width, input.banner_height };
            top += input.banner_height;
        }

        float bottom { std::max(height - shell_status_height, top) };
        rects.status = { 0.0f, bottom, width, height - bottom };
        if (input.log_open)
        {
            // 작업 로그가 본문을 다 덮지 않게 남은 높이의 절반까지만 쓴다.
            const float log_height { std::min(shell_log_height, std::max(bottom - top, 0.0f) / 2.0f) };
            bottom -= log_height;
            rects.log = { 0.0f, bottom, width, log_height };
        }
        rects.body = { 0.0f, top, width, std::max(bottom - top, 0.0f) };
        if (input.repository == false)
            return rects;

        const shell_rect& body { rects.body };
        if (input.mode == layout_mode::narrow)
        {
            const float rail { std::min(shell_rail_width, body.width) };
            rects.sidebar = { body.x, body.y, rail, body.height };
            rects.graph = { body.x + rail, body.y, std::max(body.width - rail, 0.0f), body.height };
        }
        else
        {
            float sidebar { clamp_sidebar_width(input.sidebar_width) };
            float detail { input.mode == layout_mode::wide ? clamp_detail_width(input.detail_width) : 0.0f };
            const float splitters { input.mode == layout_mode::wide ? shell_splitter_width * 2.0f : shell_splitter_width };
            // 그래프 최소 폭을 지키려고 상세를 먼저, 그다음 사이드바를 줄인다.
            float overflow { sidebar + detail + splitters + minimum_graph_width - body.width };
            if (overflow > 0.0f && detail > 0.0f)
            {
                const float give { std::min(overflow, detail - minimum_detail_width) };
                detail -= std::max(give, 0.0f);
                overflow -= std::max(give, 0.0f);
            }
            if (overflow > 0.0f)
                sidebar = std::max(sidebar - overflow, minimum_sidebar_width);

            float x { body.x };
            rects.sidebar = { x, body.y, sidebar, body.height };
            x += sidebar;
            rects.sidebar_splitter = { x, body.y, shell_splitter_width, body.height };
            x += shell_splitter_width;
            const float graph_width { std::max(body.x + body.width - x - (detail > 0.0f ? detail + shell_splitter_width : 0.0f), 0.0f) };
            rects.graph = { x, body.y, graph_width, body.height };
            x += graph_width;
            if (detail > 0.0f)
            {
                rects.detail_splitter = { x, body.y, shell_splitter_width, body.height };
                x += shell_splitter_width;
                rects.detail = { x, body.y, std::max(body.x + body.width - x, 0.0f), body.height };
            }
        }

        if (input.mode != layout_mode::wide && input.drawer_open)
        {
            // 서랍은 그래프 위에 겹친다. 왼쪽에 바깥을 눌러 닫을 자리를 조금 남긴다.
            const float drawer { std::min(clamp_detail_width(input.detail_width), std::max(body.width - 40.0f, 0.0f)) };
            rects.detail = { body.x + body.width - drawer, body.y, drawer, body.height };
            rects.detail_is_drawer = true;
        }
        return rects;
    }
} // namespace cyella
