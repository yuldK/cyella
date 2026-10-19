#pragma once

#include "domain/diff_file.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // hunk나 일부 줄에 어떤 작업을 할지 가른다. 작업이 정하는 것은 patch를 어느 쪽에 어떤 방향으로
    // 적용하느냐다.
    enum class patch_direction
    {
        // unstaged diff의 변경을 index에 올린다. `git apply --cached`.
        stage,
        // staged diff의 변경을 index에서 내린다. `git apply --cached -R`.
        unstage,
        // unstaged diff의 변경을 작업 트리에서 되돌린다. `git apply -R`.
        discard,
    };

    // 작업에 맞는 `git apply` 옵션이다. apply_patch_request의 cached·reverse가 이 값을 그대로 쓴다.
    struct patch_apply_mode
    {
        bool cached { true };
        bool reverse { false };
    };

    [[nodiscard]] patch_apply_mode patch_apply_mode_for(patch_direction direction) noexcept;

    // diff 한 파일의 hunk 하나에서 고른 줄만 담은 patch를 만든다. 돌려주는 값은 `git apply`의
    // stdin에 그대로 넘기는 텍스트이고, `patch_apply_mode_for(direction)`의 옵션과 짝이다.
    //
    // selected_lines는 `diff_hunk::lines`의 위치다. add·del 줄만 고를 수 있고, 범위를 벗어나거나
    // context·no_newline 줄을 가리키면 값이 없다. 중복은 하나로 본다.
    //
    // 알고리즘은 `git add -p`의 편집과 같다. patch의 적용 대상(stage는 옛쪽, unstage·discard는 새쪽)에서
    // 보면 고르지 않은 변경은 "일어나지 않은 일"이어야 한다.
    //  - stage: 고르지 않은 add는 버리고, 고르지 않은 del은 context로 바꾼다.
    //  - unstage·discard: `-R`이 부호를 뒤집으므로 반대다. 고르지 않은 add는 context로 바꾸고, 고르지
    //    않은 del은 버린다.
    //
    // 반환하는 patch는 hunk 하나만 담고, 머리말은 새 파일·삭제 파일 같은 경계를 다룬다. 줄 수는 다시
    // 세어 쓰므로 `--recount` 없이도 맞는다. 다음 경우는 값이 없다.
    //  - 바이너리 파일이거나 hunk 번호가 범위 밖이다.
    //  - 고른 줄이 없다(적용해도 바뀌는 것이 없다).
    //
    // `\ No newline at end of file`을 가진 줄은 짝이 묶인다. 옛쪽 마지막 줄이 줄바꿈 없이 끝나는
    // del과 그 뒤의 add는 따로 고르면 줄바꿈 없는 줄 뒤에 다른 줄이 오는 patch가 되므로, 하나라도
    // 고르면 모두 고른 것으로 본다.
    [[nodiscard]] std::optional<std::string> build_line_patch(const diff_file& file, std::size_t hunk_index, const std::vector<std::size_t>& selected_lines, patch_direction direction);

    // hunk 전체(add·del 줄 모두)를 고른 것과 같다.
    [[nodiscard]] std::optional<std::string> build_hunk_patch(const diff_file& file, std::size_t hunk_index, patch_direction direction);

    // hunk의 add·del 줄 위치를 모두 돌려준다. UI가 hunk 전체를 줄 선택으로 바꿀 때 쓴다.
    [[nodiscard]] std::vector<std::size_t> changed_line_indices(const diff_hunk& hunk);
} // namespace cyella
