#pragma once

#include "domain/text_catalog.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 커밋 메시지 제안에 쓰는 외부 명령이다 (ADR-009). 사용자가 설치해 로그인해 둔 AI CLI를 띄우고, 프롬프트를 표준 입력으로 넘겨
    // 표준 출력을 메시지로 받는다. cyella는 그 명령의 로그인·키를 읽지도 저장하지도 않는다.
    struct commit_suggestion_settings
    {
        // 켜면 커밋 상자에 제안 단추가 생긴다. 단추를 누르면 staged 변경 내용이 이 명령으로 넘어가므로 기본은 꺼짐이다.
        bool enabled { false };
        // 실행 파일이다. PATH의 이름이거나 절대 경로다. 찾는 일은 실행하는 쪽의 몫이다.
        std::u8string executable {};
        // 공백으로 나누는 인자다 (`split_commit_suggestion_arguments`). 프롬프트는 인자가 아니라 표준 입력으로 간다.
        std::u8string arguments {};
        // 기본 지시문 뒤에 덧붙이는 한 줄이다 ("제목은 한국어로" 같은 것).
        std::u8string instruction {};

        [[nodiscard]] bool operator==(const commit_suggestion_settings&) const noexcept = default;
    };

    // 설정 화면의 "채우기" 단추다. 저장되는 값이 아니라 실행 파일·인자 칸을 채우는 본보기다.
    struct commit_suggestion_preset
    {
        std::u8string_view id {};
        std::u8string_view name {};
        std::u8string_view executable {};
        std::u8string_view arguments {};
    };

    [[nodiscard]] std::span<const commit_suggestion_preset> commit_suggestion_presets() noexcept;

    // 한 파일의 패치와 전체 패치가 프롬프트에 실리는 상한(바이트)이다. 큰 파일 하나가 다른 파일의 자리를 다 쓰지 않게 둘로 나눈다.
    inline constexpr std::size_t commit_suggestion_file_patch_limit { std::size_t { 16 } * 1024 };
    inline constexpr std::size_t commit_suggestion_patch_limit { std::size_t { 96 } * 1024 };
    // 프롬프트의 파일 목록에 싣는 파일 수의 상한이다. 넘는 파일은 "외 N개"로 줄인다. 수천 개를 한꺼번에 옮긴 커밋이 프롬프트를 목록으로 채우지 않게 한다.
    inline constexpr std::size_t commit_suggestion_file_list_limit { 200 };
    // 패치에서 빼 달라고 git에 넘기는 경로 수의 상한이다. 경로마다 인자 하나라 너무 많으면 명령줄 길이 제한에 걸린다. 넘으면 패치를 아예
    // 싣지 않는다 — 다 빼지 못한 채로 내용을 보내느니 파일 목록만 보낸다.
    inline constexpr std::size_t commit_suggestion_excluded_path_limit { 200 };
    // 말투와 접두어 관례를 보이려고 싣는 최근 커밋 제목의 수다.
    inline constexpr std::uint32_t commit_suggestion_recent_subject_count { 10 };
    // 추가 지시의 상한(바이트)이다. 설정 파일을 손으로 고쳐 넣은 긴 글이 프롬프트를 덮지 않게 한다.
    inline constexpr std::size_t maximum_commit_suggestion_instruction_bytes { 500 };

    // 인자 칸을 argv로 나눈다. 외부 diff 도구의 템플릿과 같은 규칙이다 (`expand_external_diff_arguments`): 공백으로 나누고,
    // 큰따옴표 안의 공백은 나누지 않으며 따옴표는 벗긴다. `""`는 빈 인자 하나로 남는다. 닫히지 않은 따옴표는 값이 없다.
    [[nodiscard]] std::optional<std::vector<std::u8string>> split_commit_suggestion_arguments(std::u8string_view arguments);

    // staged 파일 하나다 (`diff --numstat`에서 온다).
    struct commit_suggestion_file
    {
        std::u8string path {};
        // 이름 변경·복사일 때의 원래 경로다.
        std::u8string original_path {};
        std::optional<std::uint32_t> lines_added {};
        std::optional<std::uint32_t> lines_removed {};
        bool is_binary { false };
    };

    // 내용을 프롬프트에 싣지 않을 경로인가. lock 파일은 길기만 하고 뜻이 없고, `.env`·개인 키처럼 비밀로 보이는 파일은 내용이 밖으로
    // 나가면 안 된다. 이런 파일은 이름과 줄 수만 싣는다. 이름으로 보는 최선 노력이다.
    [[nodiscard]] bool commit_suggestion_omits_content(std::u8string_view path) noexcept;

    struct commit_suggestion_patch
    {
        std::u8string text {};
        // 상한 때문에 잘렸거나 빠진 파일이 있다.
        bool shortened { false };
    };

    // 패치를 `diff --git` 경계에서 파일별로 나눠 상한에 맞춘다. 한 파일이 `file_limit`를 넘으면 줄 경계에서 자르고, 전체가
    // `total_limit`를 넘으면 뒤의 파일을 뺀다. 자른 자리에는 표시 줄을 남긴다.
    [[nodiscard]] commit_suggestion_patch shorten_commit_suggestion_patch(
        std::u8string_view patch, std::size_t file_limit = commit_suggestion_file_patch_limit, std::size_t total_limit = commit_suggestion_patch_limit);

    struct commit_suggestion_prompt_inputs
    {
        std::u8string_view instruction {};
        // 최근 제목이 없을 때 쓸 언어다 (화면 언어).
        ui_language language { ui_language::korean };
        std::span<const std::u8string> recent_subjects {};
        std::span<const commit_suggestion_file> files {};
        commit_suggestion_patch patch {};
    };

    // 표준 입력으로 넘길 프롬프트다. 지시문은 영어로 고정한다 (모델에게 하는 말이라 화면 문구가 아니다).
    [[nodiscard]] std::u8string build_commit_suggestion_prompt(const commit_suggestion_prompt_inputs& inputs);

    struct suggested_commit_message
    {
        std::u8string subject {};
        std::u8string body {};

        [[nodiscard]] bool operator==(const suggested_commit_message&) const noexcept = default;
    };

    // 명령의 표준 출력을 제목과 본문으로 나눈다. 바깥 코드 펜스와 앞뒤 공백을 걷고, 첫 줄을 제목으로, 그 뒤(빈 줄은 건너뛴다)를
    // 본문으로 삼는다. 제목은 한 줄이고 제어 문자는 지운다 — 이 값은 입력 필터를 거치지 않고 입력란에 들어간다.
    // 쓸 글이 없으면 제목이 빈 값이다.
    [[nodiscard]] suggested_commit_message parse_suggested_commit_message(std::u8string_view output);
} // namespace cyella
