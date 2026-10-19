#pragma once

#include "presentation/ui/ui_parts.h"
#include "presentation/view_snapshot.h"

#include "luil/app/webview.h"
#include "luil/ui/ui_element.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace cyella::ui {
    // 창에 웹뷰는 하나다. 탭이나 파일을 바꿔도 같은 id를 쓰므로 luil이 웹뷰를 다시 만들지 않고 `show_diff`만 새로 보낸다.
    inline constexpr std::u8string_view diff_webview_id { u8"diff" };

    // 웹뷰가 앱에 알리는 일(항해·페이지가 보낸 메시지·실패)을 logic thread로 실어 나르는 앱 메시지다.
    struct diff_webview_event_intent
    {
        luil::webview_event event {};
    };

    // diff 자리 전체다. 머리 막대(돌아가기·제목·전환 단추)는 luil이 그리고, 본문은 웹뷰 자리표다. 웹뷰를 쓸 수 없으면
    // 본문이 텍스트 hunk 목록(`diff_row_element`)이다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_diff_area(const view_snapshot& view, const repository_view& repository, feedback_sources& sources);

    struct diff_webview_options
    {
        // 뷰어 페이지의 `file://` 주소다.
        std::u8string navigate_url {};
        std::u8string user_data_folder {};
        // `diff_view_controller`가 마지막으로 만든 메시지와 번호다. 번호가 바뀐 frame에만 페이지로 간다.
        std::uint64_t post_revision { 0 };
        std::u8string post_message {};
    };

    // frame에 실을 웹뷰다. 정책은 이렇다: `file`만 열고, 페이지가 보내는 메시지는 8 MiB(복사 텍스트)까지 받고,
    // **개발자 도구는 끈다.** 항해는 한 번(`navigate_revision` 1)만 하므로 frame마다 같은 값을 실어도 다시 읽지 않는다.
    [[nodiscard]] luil::ui_webview make_diff_webview(const diff_webview_options& options);

    // 페이지가 보낸 문서가 우리가 연 뷰어인가. 웹뷰는 간 적이 있는 어느 문서든 메시지를 보낼 수 있으므로(luil 헤더의 경고)
    // 앱이 보낸 쪽을 가린다. 대소문자와 `\`·`/` 차이는 무시한다.
    [[nodiscard]] bool is_viewer_document(std::u8string_view document_url, std::u8string_view viewer_url) noexcept;
} // namespace cyella::ui
