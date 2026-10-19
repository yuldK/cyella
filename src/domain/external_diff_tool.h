#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 확장자별 외부 diff 도구 한 줄이다 (ADR-002). 확장자는 `.png`처럼 점으로 시작하는
    // 접미사이며 `.tar.gz` 같은 여러 점도 쓸 수 있다. 점이나 `*.`이 없어도 받아들인다.
    struct external_diff_tool
    {
        std::vector<std::u8string> extensions {};
        // 실행 파일이다. 절대 경로 확인은 실행하는 쪽의 몫이다. 설정 파일에는 사용자가 PATH의
        // 이름을 적을 수도 있고, 그 해석은 프로세스 실행 단계에서 일어나기 때문이다.
        std::u8string executable {};
        // `{left}` `{right}` `{base}` `{path}`를 쓸 수 있는 인자 템플릿이다.
        std::u8string arguments {};

        [[nodiscard]] bool operator==(const external_diff_tool&) const noexcept = default;
    };

    // 템플릿에 채울 값이다. 쓰지 않는 값(예: merge가 아닐 때의 base)은 비워 둔다.
    struct external_diff_values
    {
        std::u8string left {};
        std::u8string right {};
        std::u8string base {};
        std::u8string path {};
    };

    // 템플릿을 argv로 풀어 낸다. 셸을 거치지 않는다.
    //
    // - 먼저 템플릿을 공백으로 나누고(큰따옴표 안의 공백은 나누지 않고 따옴표는 벗긴다),
    //   그 뒤에 각 인자 안에서 토큰을 값으로 바꾼다. 순서가 반대이면 공백이 든 경로가 여러
    //   인자로 쪼개진다. 바꾼 값은 다시 해석하지 않으므로 값 안의 따옴표·공백·`{left}`는
    //   그대로 한 인자의 일부다.
    // - 역슬래시는 특별하지 않다. Windows 경로를 그대로 쓰기 위해서다.
    // - 모르는 `{...}`는 글자 그대로 둔다.
    // - 템플릿에 내용이 있던 인자가 빈 값으로 바뀌면(`{base}`에 base가 없을 때) 그 인자를
    //   뺀다. `""`처럼 템플릿이 직접 쓴 빈 인자는 남긴다.
    // - 닫히지 않은 따옴표는 템플릿 오류이므로 값이 없다.
    [[nodiscard]] std::optional<std::vector<std::u8string>> expand_external_diff_arguments(std::u8string_view argument_template, const external_diff_values& values);

    // 설정에 적힌 확장자를 비교용 모양(`.png`, 소문자)으로 맞춘다. 비어 있거나 점뿐이면 빈 값이다.
    [[nodiscard]] std::u8string normalize_diff_extension(std::u8string_view extension);

    // 파일 경로에 맞는 도구를 고른다. 파일 이름이 확장자로 끝나는지 ASCII 대소문자를 무시하고
    // 본다. 여러 도구가 맞으면 가장 긴 확장자가 이긴다(`.tar.gz`가 `.gz`보다 구체적이다).
    // 길이가 같으면 목록에서 먼저 나온 도구다. 없으면 null이다.
    [[nodiscard]] const external_diff_tool* select_external_diff_tool(const std::vector<external_diff_tool>& tools, std::u8string_view path) noexcept;
} // namespace cyella
