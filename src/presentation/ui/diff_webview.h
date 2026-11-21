#pragma once

#include "presentation/ui/ui_parts.h"
#include "presentation/view_snapshot.h"

#include "luil/app/webview.h"
#include "luil/ui/ui_element.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
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
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_diff_area(const repository_view& repository, float width, feedback_sources& sources);

    // diff 자리의 본문이 웹뷰 자리표인가다 (diff가 열려 있고, 카드로 보이는 특수 diff가 아니고, 웹뷰를 쓸 수 있다).
    // 웹뷰 자리 안에서는 주 tree의 그림이 보이지 않으므로, 그 위에 떠야 하는 것(토스트)은 이 값을 보고 popup 표면으로 간다.
    [[nodiscard]] bool diff_webview_stands(const repository_view& repository) noexcept;

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
    // 표면에 표시할 수 있는지도 알려 받는다 (`report_presentation`, `diff_webview_health::presentable`).
    [[nodiscard]] luil::ui_webview make_diff_webview(const diff_webview_options& options);

    // 페이지가 보낸 문서가 우리가 연 뷰어인가. 웹뷰는 간 적이 있는 어느 문서든 메시지를 보낼 수 있으므로(luil 헤더의 경고)
    // 앱이 보낸 쪽을 가린다. 대소문자와 `\`·`/` 차이는 무시한다.
    [[nodiscard]] bool is_viewer_document(std::u8string_view document_url, std::u8string_view viewer_url) noexcept;

    // 웹뷰 사건 하나가 diff 보기에 시키는 일이다.
    enum class diff_webview_action
    {
        // diff 보기와 상관없는 사건이다.
        none,
        // 페이지가 다시 읽힌다. 새 `ready`가 올 때까지 뷰어에 보내지 않는다.
        page_reloading,
        // 페이지가 보낸 메시지다. 보낸 문서가 뷰어인지는 받는 쪽이 가린다.
        web_message,
        // 웹뷰를 더는 쓸 수 없다. 이번 실행 동안 텍스트 대체 보기로 물러서고 웹뷰를 frame에서 뺀다.
        fall_back,
        // 웹뷰가 앉은 표면이 이번 실행에서 처음으로 합성 제시를 못 하게 되었다. 사용자에게 한 번 알린다.
        //  - 웹뷰는 frame에 그대로 싣는다. 본문을 텍스트로 바꾸는 것은 `presentable()`이 하고, 다시 가능해지면 뷰어로 돌아간다.
        //  - 두 번째부터의 오감은 `none`이다 (상태만 바뀐다).
        presentation_lost,
    };

    // diff 웹뷰가 아직 쓸 만한가를 luil의 웹뷰 사건으로 판정한다. 시작할 때의 Runtime 확인이 보지 못하는 실패(오래된 Runtime,
    // 환경·컨트롤러 생성 실패, 브라우저 프로세스 종료)가 여기서 걸린다.
    //  - **한 번 물러서면 되돌아가지 않는다.** luil은 목록에서 빠졌다가 다시 실린 id를 다시 세우는데, 실패한 채 다시 실으면
    //    만들고 실패하기를 되풀이하고 화면은 placeholder와 텍스트 사이를 오간다. 물러선 뒤 늦게 도착한 사건도 모두 `none`이다.
    //  - 렌더러가 죽으면 luil이 주 문서를 한 번 다시 읽고, 10초 안에 또 죽으면 다시 읽지 않아 오류 페이지가 남는다. 그 갈래는
    //    `error` 글로만 구분되는데 글은 약속이 아니므로, 앱이 같은 규칙을 더 넓은 창으로 따로 세어 두 번째 죽음에서 물러선다.
    //  - **표시할 수 없음은 실패가 아니다.** 환경·컨트롤러는 정상인데 앉은 표면이 합성 제시를 못 하면(CPU·레이어드 표시, 실행 중
    //    GPU에서 CPU로 물러난 표면) luil이 `presentation_changed`(거짓)를 알리고 웹뷰를 숨긴다. 그동안 본문은 텍스트이고 웹뷰는
    //    frame에 남는다 — 빼면 luil이 다시 가능해진 것을 알릴 수 없고 페이지 상태도 날아간다. 참이 오면 뷰어로 돌아간다.
    //    첫 값이 오기 전에는 표시할 수 있다고 본다. luil은 컨트롤러가 준비되는 순간 표면의 실제 값을 처음 알리므로, GPU 표면에서는
    //    첫 값부터 참이라 정상 PC의 시작이 텍스트로 깜빡이지 않는다.
    class diff_webview_health
    {
    public:
        // 렌더러가 이 간격 안에 다시 죽으면 물러선다. luil의 10초보다 넓게 잡아, 사건이 logic thread에 늦게 닿아도 luil이
        // 다시 읽지 않은 경우를 놓치지 않는다 (넓어서 잃는 것은 다시 읽힌 웹뷰 대신 텍스트를 보이는 것뿐이다).
        static constexpr std::chrono::seconds render_failure_window { 30 };

        [[nodiscard]] diff_webview_action on_event(const luil::webview_event& event, std::chrono::steady_clock::time_point now);

        [[nodiscard]] bool failed() const noexcept
        {
            return failed_;
        }

        // 물러선 까닭이다 (사건의 갈래·HRESULT·luil의 설명). 물러서지 않았으면 비어 있다.
        [[nodiscard]] const std::u8string& reason() const noexcept
        {
            return reason_;
        }

        // 웹뷰가 앉은 표면에 지금 표시할 수 있는가. 마지막으로 받은 `presentation_changed`의 값이고, 받기 전에는 참이다.
        //  - 실패(`failed`)와 따로 본다. 본문이 뷰어인 것은 둘 다 맞을 때다.
        [[nodiscard]] bool presentable() const noexcept
        {
            return presentable_;
        }

    private:
        diff_webview_action fall_back(const luil::webview_event& event);
        diff_webview_action presentation_changed(bool available);

        bool failed_ { false };
        bool presentable_ { true };
        // 표시할 수 없게 된 것을 이번 실행에서 이미 알렸다.
        bool presentation_lost_reported_ { false };
        std::u8string reason_ {};
        std::optional<std::chrono::steady_clock::time_point> last_render_failure_ {};
    };
} // namespace cyella::ui
