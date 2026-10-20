#pragma once

#include "infrastructure/git_command_builder.h"

#include <string_view>

namespace cyella {
    // `lfs version`. git-lfs가 없으면 `'lfs' is not a git command`로 종료 코드 1이다.
    [[nodiscard]] git_arguments make_git_lfs_version_args();

    // `lfs ls-files --json`. 크기와 내려받음 여부까지 준다.
    [[nodiscard]] git_arguments make_git_lfs_ls_files_json_args();
    // `lfs ls-files --long`. json을 모르는 오래된 git-lfs의 대체다.
    [[nodiscard]] git_arguments make_git_lfs_ls_files_long_args();

    // `lfs install --local`. 이 저장소에만 `filter.lfs.*`와 hook을 설정한다. 전역 설정은 건드리지 않는다.
    [[nodiscard]] git_arguments make_git_lfs_install_local_args();

    // `config --get filter.lfs.smudge`. 설정이 없으면 종료 코드 1이다.
    [[nodiscard]] git_arguments make_git_lfs_filter_probe_args();

    // `lfs pull`. 객체를 받고 작업 트리의 포인터를 실제 내용으로 바꾼다.
    [[nodiscard]] git_arguments make_git_lfs_pull_args();

    // `lfs fetch [--all]`. 객체만 받는다. all이면 모든 ref의 객체다.
    [[nodiscard]] git_arguments make_git_lfs_fetch_args(bool all);

    // `lfs track -- <pattern>`. `.gitattributes`에 `<pattern> filter=lfs diff=lfs merge=lfs -text`를 더한다.
    [[nodiscard]] git_arguments make_git_lfs_track_args(std::string_view pattern);

    // `check-attr -z --stdin filter`. 경로를 NUL로 구분해 stdin으로 넘기면 `경로\0filter\0값\0`을 돌려준다.
    [[nodiscard]] git_arguments make_git_check_attr_filter_args();
} // namespace cyella
