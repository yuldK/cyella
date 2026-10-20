#pragma once

#include "application/git_operation.h"
#include "application/process_cancellation.h"
#include "domain/diff_file.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// 이미지 비교와 외부 diff 도구는 파일의 "왼쪽"과 "오른쪽"을 실제 파일로 꺼내야 한다. 이 파일은 그 두 쪽이
// 어디에서 오는지를 정하는 순수 규칙과, 꺼내는 일을 맡는 계약(diff_git_gateway)이다.
namespace cyella {
    enum class diff_side_kind
    {
        // 이 쪽에는 파일이 없다(추가된 파일의 왼쪽, 삭제된 파일의 오른쪽).
        none,
        // 커밋의 blob이다(`git cat-file blob <revision>:<path>`).
        revision,
        // index의 blob이다(`git cat-file blob :<path>`).
        index,
        // 작업 트리의 파일이다.
        worktree,
    };

    struct diff_side_spec
    {
        diff_side_kind kind { diff_side_kind::none };
        // kind가 revision일 때의 리비전이다(`HEAD`, `<sha>`, `<sha>^`).
        std::string revision {};
        // 저장소 루트 기준 경로다.
        std::string path {};

        [[nodiscard]] bool operator==(const diff_side_spec&) const noexcept = default;
    };

    // 어떤 diff의 두 쪽을 가리키는 값이다. 컨트롤러가 diff를 받을 때 만든다.
    struct diff_side_plan
    {
        std::u8string repository_path {};
        diff_view view { diff_view::unstaged };
        // view가 commit일 때의 커밋이다.
        std::string sha {};
        // diff_file의 옛·새 경로다. 추가된 파일의 old_path, 삭제된 파일의 new_path는 비어 있다.
        std::string old_path {};
        std::string new_path {};
        char status { 'M' };
        // 추적하지 않는 파일의 diff다. 왼쪽이 없다.
        bool untracked { false };
        // Git LFS 포인터의 diff다. 커밋·index의 blob은 포인터 텍스트이므로 로컬 LFS 객체(`lfs/objects`)를 꺼낸다.
        bool lfs { false };

        [[nodiscard]] bool operator==(const diff_side_plan&) const noexcept = default;
        // 화면에 보일 경로다.
        [[nodiscard]] const std::string& display_path() const noexcept
        {
            return new_path.empty() ? old_path : new_path;
        }
    };

    struct diff_side_pair
    {
        diff_side_spec left {};
        diff_side_spec right {};

        [[nodiscard]] bool operator==(const diff_side_pair&) const noexcept = default;
    };

    // 보기별로 비교하는 두 쪽이다.
    //  - unstaged: index(왼쪽)와 작업 트리(오른쪽)
    //  - staged: HEAD(왼쪽)와 index(오른쪽)
    //  - commit: 첫 부모(`<sha>^`, 왼쪽)와 그 커밋(오른쪽)
    // 추가된 파일은 왼쪽이, 삭제된 파일은 오른쪽이 없다. 이름이 바뀌었으면 왼쪽은 옛 경로다.
    [[nodiscard]] diff_side_pair resolve_diff_sides(const diff_side_plan& plan);

    // 확장자로 판단하는 이미지다(png, jpg, jpeg, gif, webp, bmp, svg, ico). 비교는 ASCII 대소문자를 무시한다.
    [[nodiscard]] bool is_image_path(std::string_view path) noexcept;

    // 꺼낸 한 쪽이다.
    struct extracted_side
    {
        bool present { false };
        // write_files로 꺼냈으면 임시 파일의 절대 경로다.
        std::u8string file {};
        std::optional<std::uint64_t> size {};
    };

    struct extracted_sides
    {
        bool ok { false };
        // ok가 아닐 때의 까닭이다.
        std::string error {};
        extracted_side left {};
        extracted_side right {};
    };

    // diff 두 쪽을 git에서 꺼내는 일과 git 설정을 읽는 일의 계약이다. 프로세스를 띄우므로 logic thread에서
    // 부르지 않는다. 구현은 여러 스레드에서 불려도 안전해야 한다.
    class diff_git_gateway
    {
    public:
        diff_git_gateway() = default;
        diff_git_gateway(const diff_git_gateway&) = delete;
        diff_git_gateway(diff_git_gateway&&) = delete;
        diff_git_gateway& operator=(const diff_git_gateway&) = delete;
        diff_git_gateway& operator=(diff_git_gateway&&) = delete;
        virtual ~diff_git_gateway() = default;

        // 두 쪽의 크기를 구하고, write_files가 참이면 임시 폴더(작업마다 새로 만든다)에 파일로 꺼낸다. 임시
        // 파일은 원래 파일 이름을 유지하고 `left`·`right` 하위 폴더에 두어 도구가 확장자와 이름을 알아보게
        // 한다. 한 쪽이 없으면 present가 거짓이다.
        [[nodiscard]] virtual extracted_sides extract(const diff_side_plan& plan, bool write_files) = 0;

        // 취소할 수 있는 판이다. 취소되면 실행 중인 git을 끝내고 `ok`가 거짓인 결과를 돌려준다. 취소를 모르는
        // 구현은 그냥 `extract`를 부른다.
        [[nodiscard]] virtual extracted_sides extract_cancellable(const diff_side_plan& plan, bool write_files, const process_cancellation_token& cancellation)
        {
            static_cast<void>(cancellation);
            return extract(plan, write_files);
        }

        // `git config diff.tool`이다. 설정이 없으면 값이 없다.
        [[nodiscard]] virtual std::optional<std::string> configured_diff_tool(std::u8string_view repository_path) = 0;
    };
} // namespace cyella
