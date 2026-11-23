#pragma once

#include "domain/file_blame.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyella {
    struct git_blame_result
    {
        // 처음 나온 순서다.
        std::vector<blame_origin> origins {};
        // 파일의 줄 순서다. 바이너리면 비어 있다.
        std::vector<blame_line> lines {};
        // 내용에 NUL이 있었다.
        bool binary { false };
        // 모양이 맞지 않아 버린 줄 수다. 0이 아니면 출력이 잘렸거나 형식이 어긋난 것이라 `lines`의 줄 번호를 믿을 수 없다.
        std::size_t malformed_line_count { 0 };
    };

    // `git blame --porcelain`의 출력을 조각조각 받아 읽는 스트리밍 해석기다. 큰 파일의 출력은 수십 MiB라서 다 모은 뒤 읽지 않는다.
    //
    // 출력 형식 (git 2.56에서 확인):
    //   - 줄마다 머리 `<sha> <원래 줄> <최종 줄> [<묶음의 줄 수>]`가 오고, 탭으로 시작하는 내용 줄이 뒤따른다. 줄 수는 묶음의
    //     첫 줄에만 있다.
    //   - 커밋 정보(`author`, `author-mail <..>`, `author-time`, `summary` 등)는 그 커밋이 처음 나온 묶음의 머리 뒤에만 온다.
    //   - `previous <sha> <path>`와 `filename <path>`는 커밋 정보와 함께 온다. 한 커밋이 여러 경로에 걸쳐 줄을 남겼으면 그 커밋의
    //     묶음마다 다시 온다. 그래서 줄의 주인은 (sha, 경로) 쌍으로 가른다. 경로는 git의 C 스타일 인용일 수 있어 풀어 읽는다.
    //   - 커밋하지 않은 줄의 sha는 0으로만 되어 있다. 문구(`Not Committed Yet`)는 보지 않는다.
    //   - sha는 40자(sha1) 또는 64자(sha256)다.
    //
    // 줄 나눔은 `\n`만 본다. 내용 줄은 맨 앞의 탭 하나와 끝의 `\n`만 떼고 byte 그대로 둔다 (`\r`, 올바르지 않은 UTF-8 포함).
    // git은 파일의 마지막 줄에 줄바꿈이 없어도 `\n`을 붙여 내므로 그 차이는 알 수 없다.
    //
    // feed()에는 임의 경계로 자른 조각을 넣어도 같은 결과가 나온다.
    class git_blame_parser
    {
    public:
        void feed(std::string_view chunk);

        // 입력이 끝났음을 알리고 결과를 가져간다. `\n`으로 끝나지 않은 꼬리와 내용 줄이 오지 않은 머리는 잘린 출력이라 깨진 줄로
        // 센다. 이후 같은 객체를 새 출력에 다시 쓸 수 있다.
        [[nodiscard]] git_blame_result finish();

    private:
        // 내용 줄을 기다리는 머리다. 머리 줄과 그 뒤의 `키 값` 줄들이 채운다.
        struct line_header
        {
            bool active { false };
            std::string sha {};
            std::uint32_t original_line { 0 };
            std::uint32_t final_line { 0 };
            // 줄 수가 붙은, 묶음의 첫 줄이다.
            bool starts_group { false };
            bool has_filename { false };
            // 이 머리 뒤에 온 커밋 정보다. `sha`·`uncommitted`는 주인을 만들 때 채운다.
            blame_origin details {};
        };

        // 커밋 하나가 가진 주인들이다. 대개 하나다.
        struct commit_origins
        {
            std::vector<std::uint32_t> indices {};
            // `filename` 없이 온 묶음이 쓰는, 이 커밋의 가장 최근 주인이다.
            std::uint32_t latest { 0 };
        };

        struct text_hash
        {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(const std::string_view text) const noexcept
            {
                return std::hash<std::string_view> {}(text);
            }
        };

        void process_line(std::string_view line);
        bool begin_header(std::string_view line);
        void read_detail(std::string_view line);
        void take_content(std::string_view content);
        [[nodiscard]] bool resolve_origin(std::uint32_t& origin);

        git_blame_result result_ {};
        // 아직 `\n`을 만나지 못한 꼬리와, 그중 이미 `\n`을 찾아본 앞부분의 길이다.
        std::string pending_ {};
        std::size_t scanned_ { 0 };
        line_header header_ {};
        std::unordered_map<std::string, commit_origins, text_hash, std::equal_to<>> commits_ {};
        // 지금 묶음의 커밋과 주인이다. 묶음의 둘째 줄부터는 머리에 `filename`이 없어 이 값을 쓴다.
        std::string group_sha_ {};
        std::uint32_t group_origin_ { 0 };
        bool group_valid_ { false };
    };

    // 전체 출력이 이미 메모리에 있을 때의 편의 함수다.
    [[nodiscard]] git_blame_result parse_git_blame(std::string_view bytes);
} // namespace cyella
