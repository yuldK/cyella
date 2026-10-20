#pragma once

#include "infrastructure/git_command_builder.h"

#include <span>
#include <string>
#include <string_view>

namespace cyella {
    // 이 파일의 함수는 명령줄만 만들고 프로세스를 실행하지 않는다(`git_command_builder.h`와 같다).

    // `submodule status --recursive`.
    [[nodiscard]] git_arguments make_git_submodule_status_args();

    // `config -f .gitmodules -z --get-regexp ^submodule[.]`. 파일이 없거나 항목이 없으면 종료 코드 1이다.
    [[nodiscard]] git_arguments make_git_gitmodules_config_args();

    // `--literal-pathspecs ls-files --stage -z -- <paths>`. 경로가 비어 있으면 거절한다(전체 목록은 너무 크다).
    [[nodiscard]] git_arguments make_git_gitlinks_args(std::span<const std::string> paths);

    struct git_submodule_update_options
    {
        // 아직 초기화하지 않은 submodule도 등록하고 받는다(`--init`).
        bool init { true };
        bool recursive { true };
        // 비우면 모든 submodule이다.
        std::vector<std::string> paths {};
    };

    // `submodule update [--init] [--recursive] --progress [-- <paths>]`. 기록된 커밋으로 체크아웃한다.
    [[nodiscard]] git_arguments make_git_submodule_update_args(const git_submodule_update_options& options);

    // `submodule sync [--recursive] [-- <paths>]`. `.gitmodules`의 url을 각 submodule의 설정에 옮긴다.
    [[nodiscard]] git_arguments make_git_submodule_sync_args(bool recursive, std::span<const std::string> paths);
} // namespace cyella
