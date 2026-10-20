#pragma once

#include "domain/file_change.h"
#include "domain/repository_summary.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // `git remote -v`의 출력을 원격 목록으로 해석한다. 줄 모양은 `<name>\t<url> (fetch|push)`다.
    // 첫 등장 순서를 지키고 이름이 같은 두 줄을 한 항목으로 합친다.
    [[nodiscard]] std::vector<remote_info> parse_git_remotes(std::string_view bytes);

    struct overwritten_files
    {
        std::vector<std::string> files {};
        // `untracked working tree files would be overwritten` 목록이 있었다.
        bool untracked { false };
    };

    // 체크아웃·병합이 "would be overwritten"으로 멈출 때 stderr의 파일 목록을 읽는다. git은
    // 목록 머리 줄 다음에 탭으로 들여 쓴 경로를 한 줄씩 낸다. 머리 줄은 영어 문구로 찾으므로
    // `LC_ALL=C`로 실행해야 한다. 목록이 없으면 빈 값이다.
    [[nodiscard]] overwritten_files parse_overwritten_files(std::string_view standard_error);

    // `check-attr -z diff -- <path>`의 출력에서 diff 드라이버 이름을 뽑는다. 출력은 `<path>\0diff\0<value>\0`이고 값이
    // `unspecified`·`set`·`unset`이면 드라이버를 지정한 것이 아니라 값이 없다. 모양이 맞지 않아도 값이 없다.
    [[nodiscard]] std::optional<std::string> parse_git_diff_driver(std::string_view bytes);

    // `config --get`의 출력에서 값 한 줄을 뽑는다. 끝의 줄바꿈을 떼고, 비어 있으면 값이 없다.
    [[nodiscard]] std::optional<std::string> parse_git_config_value(std::string_view bytes);

    // `--name-status -z` 출력을 변경 목록으로 해석한다. 일반 항목은 `<S>\0<path>\0`, 이름
    // 변경·복사는 `R<점수>\0<old>\0<new>\0`다. 모양이 맞지 않는 항목은 건너뛴다.
    [[nodiscard]] std::vector<file_change> parse_git_name_status(std::string_view bytes);

    // 진행 중 작업의 단계를 git 디렉터리 안의 파일로 읽는다. `read`는 git 디렉터리 기준 상대
    // 경로(`rebase-merge/msgnum`)를 받아 내용을 돌려주고, 없으면 값이 없다. 파일 시스템 접근을
    // 호출자에게 맡겨 이 함수는 순수하게 둔다.
    //
    // - rebase(merge 방식): `rebase-merge/{msgnum,end,onto,head-name,message}`
    // - rebase(apply 방식): `rebase-apply/{next,last,onto,head-name}`, 제목은 `rebase-apply/msg`
    // - merge·cherry-pick·revert: 제목은 `MERGE_MSG` 첫 줄
    [[nodiscard]] in_progress_operation read_in_progress_operation(repository_operation_state kind, const std::function<std::optional<std::string>(std::string_view relative_path)>& read);
} // namespace cyella
