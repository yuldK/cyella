#pragma once

#include "application/git_operation.h"
#include "domain/diff_file.h"
#include "domain/file_blame.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// diff 뷰어(assets/diff-viewer)와 C++이 주고받는 메시지의 정의, 직렬화와 해석이다. 순수 함수만 있다.
//
// 전송은 luil 웹뷰의 JSON 메시지 하나다. C++ → JS는 frame마다 **마지막 메시지만** 적용되므로 diff
// 하나는 메시지 하나로 보내고, 화면 상태(layout·theme)는 내용 메시지가 함께 싣는다. 모든 C++ → JS
// 메시지는 단조 증가하는 `seq`를 가진다. JS → C++ 동작은 자기가 보고 있는 diff를 실어 온 메시지의
// `seq`를 되돌려 보내고, 그 diff가 이미 바뀌었으면 거절한다.
namespace cyella {
    // 두 쪽이 같은 규칙으로 읽는 프로토콜 판이다. `ready`가 JS의 판을 알린다.
    inline constexpr int diff_view_protocol_version { 1 };

    // 웹뷰가 C++로 보내는 메시지의 상한이다. 가장 큰 것은 `copy`의 텍스트이고, 나머지(줄 범위 포함)는
    // 수 KiB다. luil은 페이지가 보내는 메시지에만 상한(`webview_policy::maximum_message_bytes`,
    // 기본 256 KiB)을 걸고 앱이 페이지로 보내는 메시지에는 걸지 않는다.
    inline constexpr std::size_t diff_view_maximum_copy_bytes { 4u * 1024u * 1024u };
    // copy 텍스트를 JSON으로 감싼 크기까지 담는 정책 값이다. 8 MiB면 JSON 이스케이프로 두 배가 되어도
    // 4 MiB 텍스트가 들어간다.
    inline constexpr std::size_t diff_view_maximum_message_bytes { 8u * 1024u * 1024u };
    // 줄 범위 목록의 상한이다. 선택은 연속 범위로 보내므로 수천 개면 사람이 만든 선택이 아니다.
    inline constexpr std::size_t diff_view_maximum_ranges { 4096 };
    // 한 번에 보내는 diff의 줄 수 상한 기본값이다. 넘으면 앞부분만 보내고 "전체 보기"를 둔다.
    inline constexpr std::size_t diff_view_default_line_limit { 100000 };

    enum class diff_layout
    {
        // 한 열로 옛·새 줄을 위아래로 쌓는다. JSON 이름은 `inline`이다.
        inline_view,
        split,
    };

    enum class image_compare_mode
    {
        side_by_side,
        onion_skin,
        swipe,
    };

    enum class diff_theme_mode
    {
        light,
        dark,
    };

    // 웹뷰가 지금 보이고 있는(또는 곧 보일) 내용의 종류다. 머리 막대가 어떤 단추를 보일지 정하는 데 쓴다.
    enum class diff_content
    {
        empty,
        // 이미지·바이너리의 두 쪽을 꺼내는 중이다.
        loading,
        text,
        image,
        binary,
        // 텍스트 diff로 보일 수 없는 변경(submodule 포인터, 실제 내용이 로컬에 없는 LFS 포인터)이다. 웹뷰는 비우고 luil 카드가 그린다.
        special,
        // 파일의 blame이다. diff가 아니라서 보기 전환(inline·split)과 줄 단위 동작이 없다.
        blame,
    };

    // 웹뷰가 쓰는 색이다. tokens는 CSS 변수 이름(`--window-background`)과 값(`#1e1e1e`)이며 비워 두면 뷰어에
    // 내장된 기본 색을 쓴다. luil 팔레트를 그대로 넘기고 싶을 때 채운다.
    struct diff_theme
    {
        diff_theme_mode mode { diff_theme_mode::dark };
        std::map<std::string, std::string> tokens {};

        [[nodiscard]] bool operator==(const diff_theme&) const noexcept = default;
    };

    // ------------------------------------------------------------------ C++ → JS

    // 지금 카탈로그의 뷰어 문구와 언어 이름(`ko`·`en`)이다.
    [[nodiscard]] std::map<std::string, std::string> diff_viewer_strings();
    [[nodiscard]] std::string diff_viewer_language();

    // 내용 메시지가 공통으로 싣는 화면 상태다.
    struct diff_view_state
    {
        diff_layout layout { diff_layout::inline_view };
        image_compare_mode image_mode { image_compare_mode::side_by_side };
        diff_theme theme {};
        // 뷰어 안의 머리 줄(경로·통계·보기 전환)을 보인다. luil이 머리를 그리면 끈다.
        bool toolbar { true };
        // 뷰어가 그리는 문구(`diff_viewer.*` 키 → 틀)와 문서 언어다 (ADR-008). 카탈로그는 시작할 때 정해지고 바뀌지 않으므로
        // 상태를 만들 때 한 번 읽는다. frame마다 마지막 메시지만 적용되므로 내용 메시지마다 함께 싣는다.
        std::map<std::string, std::string> strings { diff_viewer_strings() };
        std::string language { diff_viewer_language() };
    };

    struct show_diff_message
    {
        std::uint64_t seq { 0 };
        diff_view view { diff_view::unstaged };
        diff_view_state state {};
        bool can_stage { false };
        bool can_unstage { false };
        bool can_discard { false };
        const diff_file* file { nullptr };
        // 0이 아니면 이 줄 수까지만 보낸다. hunk 머리와 No newline 표지도 한 줄로 센다.
        std::size_t line_limit { 0 };
    };

    struct show_image_message
    {
        std::uint64_t seq { 0 };
        diff_view_state state {};
        std::string path {};
        char status { 'M' };
        // `file:///` 주소다. 그 쪽 파일이 없으면(추가·삭제) 비어 있다.
        std::string left_url {};
        std::string right_url {};
        std::optional<std::uint64_t> left_size {};
        std::optional<std::uint64_t> right_size {};
        bool can_open_external { true };
    };

    struct show_binary_message
    {
        std::uint64_t seq { 0 };
        diff_view_state state {};
        std::string path {};
        char status { 'M' };
        std::optional<std::uint64_t> old_size {};
        std::optional<std::uint64_t> new_size {};
        bool can_open_external { true };
    };

    // 파일의 blame이다. 줄마다 그 줄을 마지막으로 바꾼 커밋(`origins`의 위치)을 싣는다. 뷰어는 같은 커밋이 이어지는 묶음의
    // 첫 줄에만 커밋·작성자·날짜를 적는다.
    struct show_blame_message
    {
        std::uint64_t seq { 0 };
        diff_view_state state {};
        std::string path {};
        // 비면 작업 트리, 아니면 blame한 리비전이다.
        std::string revision {};
        const std::vector<blame_origin>* origins { nullptr };
        const std::vector<blame_line>* lines { nullptr };
        // 0이 아니면 이 줄 수까지만 보낸다.
        std::size_t line_limit { 0 };
        // 비어 있지 않으면 뷰어가 그 커밋의 줄을 고른 채로 열고 그 첫 줄로 흘린다 (파일 이력에서 고른 커밋).
        std::string focus {};
    };

    [[nodiscard]] std::string serialize_show_diff(const show_diff_message& message);
    [[nodiscard]] std::string serialize_show_blame(const show_blame_message& message);
    [[nodiscard]] std::string serialize_show_image(const show_image_message& message);
    [[nodiscard]] std::string serialize_show_binary(const show_binary_message& message);
    [[nodiscard]] std::string serialize_clear(std::uint64_t seq, const diff_view_state& state);
    // 작은 `set_*` 메시지는 지금 보일 내용 메시지의 seq(`content_seq`)를 함께 싣는다. frame이 건너뛰어 앞의 내용 메시지가
    // 페이지에 닿지 않았으면 페이지는 자기 내용 seq와 이 값이 달라진 것을 보고 `ready`를 다시 보내 전체 상태를 받는다.
    [[nodiscard]] std::string serialize_set_layout(std::uint64_t seq, std::uint64_t content_seq, diff_layout layout);
    [[nodiscard]] std::string serialize_set_theme(std::uint64_t seq, std::uint64_t content_seq, const diff_theme& theme);
    [[nodiscard]] std::string serialize_set_image_mode(std::uint64_t seq, std::uint64_t content_seq, image_compare_mode mode);

    // diff의 줄 수다(hunk 머리 제외, No newline 표지 포함). 줄 수 상한과 같은 단위다.
    [[nodiscard]] std::size_t count_diff_lines(const diff_file& file) noexcept;

    // 바이트가 올바른 UTF-8인가. 과잉 인코딩, 대리 쌍(U+D800~DFFF), U+10FFFF 초과는 올바르지 않다.
    [[nodiscard]] bool is_valid_utf8(std::string_view bytes) noexcept;

    // 올바르지 않은 바이트를 U+FFFD로 바꾼 UTF-8이다. 경로 같은 화면 표시용 값에만 쓴다. 줄 내용은
    // 바이트를 보존해야 하므로 base64로 보낸다.
    [[nodiscard]] std::string to_valid_utf8(std::string_view bytes);

    [[nodiscard]] std::string base64_encode(std::string_view bytes);

    // 절대 경로(백슬래시·한글·공백 포함)를 `file:///C:/dir/%ED%95%9C.png`로 바꾼다. 경로가 비어 있으면
    // 빈 문자열이다. 구분자 `/`와 드라이브 `:`, 영숫자와 `-_.~`만 그대로 둔다.
    [[nodiscard]] std::string make_file_url(std::u8string_view absolute_path);

    // ------------------------------------------------------------------ JS → C++

    enum class diff_hunk_command
    {
        stage,
        unstage,
        discard,
    };

    // 줄 선택 한 구간이다. `diff_hunk::lines`의 위치이며 양 끝을 포함한다. 구간 안의 context 줄은 선택되지
    // 않는다(add·del 줄만 고른 것으로 본다).
    struct diff_line_range
    {
        std::size_t first { 0 };
        std::size_t last { 0 };

        [[nodiscard]] bool operator==(const diff_line_range&) const noexcept = default;
    };

    struct ready_action
    {
        int protocol { 0 };
    };

    struct hunk_action
    {
        diff_hunk_command command { diff_hunk_command::stage };
        std::uint64_t seq { 0 };
        std::size_t hunk { 0 };
    };

    struct lines_action
    {
        diff_hunk_command command { diff_hunk_command::stage };
        std::uint64_t seq { 0 };
        std::size_t hunk { 0 };
        std::vector<diff_line_range> ranges {};
    };

    struct open_external_action
    {
        std::uint64_t seq { 0 };
    };

    struct copy_action
    {
        std::uint64_t seq { 0 };
        std::string text {};
    };

    // 줄 수 상한으로 잘린 diff의 전체를 청한다.
    struct show_full_action
    {
        std::uint64_t seq { 0 };
    };

    // 뷰어 안에서 사용자가 보기를 바꿨다. diff 내용과 무관하므로 seq가 없다.
    struct layout_changed_action
    {
        diff_layout layout { diff_layout::inline_view };
    };

    struct image_mode_changed_action
    {
        image_compare_mode mode { image_compare_mode::side_by_side };
    };

    // blame의 줄을 남긴 커밋을 그래프에서 연다. `origin`은 보고 있는 blame의 `origins` 위치다.
    struct blame_open_commit_action
    {
        std::uint64_t seq { 0 };
        std::size_t origin { 0 };
    };

    // 그 커밋 직전 판으로 blame을 다시 연다 (`blame_origin::previous_sha`·`previous_path`).
    struct blame_previous_action
    {
        std::uint64_t seq { 0 };
        std::size_t origin { 0 };
    };

    using diff_view_action = std::variant<ready_action, hunk_action, lines_action, open_external_action, copy_action, show_full_action, layout_changed_action, image_mode_changed_action,
        blame_open_commit_action, blame_previous_action>;

    struct diff_action_parse_result
    {
        std::optional<diff_view_action> action {};
        // action이 없을 때의 까닭이다. 로그용이다.
        std::string error {};
    };

    // 웹뷰가 보낸 JSON을 해석한다. 객체가 아니거나, 모르는 type이거나, 필드가 모자라거나 남거나, 형이 다르거나,
    // 범위가 맞지 않으면 거절한다. diff 내용과의 대조(seq, hunk 번호가 실제 범위인가)는 컨트롤러가 한다.
    [[nodiscard]] diff_action_parse_result parse_diff_view_action(std::string_view json);

    // 구간 목록을 hunk 안의 add·del 줄 위치로 펼친다. 정렬되고 중복이 없다. hunk 밖을 가리키는 구간이 있으면
    // 값이 없다.
    [[nodiscard]] std::optional<std::vector<std::size_t>> expand_line_ranges(const diff_hunk& hunk, const std::vector<diff_line_range>& ranges);

    [[nodiscard]] std::string_view diff_layout_name(diff_layout layout) noexcept;
    [[nodiscard]] std::string_view image_compare_mode_name(image_compare_mode mode) noexcept;
    [[nodiscard]] std::string_view diff_view_name(diff_view view) noexcept;
} // namespace cyella
