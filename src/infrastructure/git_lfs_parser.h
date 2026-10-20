#pragma once

#include "domain/lfs.h"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // `git lfs version`의 첫 토큰이다(`git-lfs/3.7.1`). 알아볼 수 없으면 값이 없다.
    [[nodiscard]] std::optional<std::string> parse_git_lfs_version(std::string_view bytes);

    // `git lfs ls-files --json`의 출력이다. JSON이 아니거나 `files`가 없으면 값이 없다.
    // `checkout`은 작업 트리에 실제 내용이 있다는 뜻이고 `downloaded`는 로컬 객체 저장소에 있다는 뜻이다.
    [[nodiscard]] std::optional<std::vector<lfs_file>> parse_git_lfs_files_json(std::string_view bytes);

    // `git lfs ls-files --long`의 줄이다(`<64자리 oid> <*|-> <경로>`). `*`는 실제 내용이 작업 트리에 있고 `-`는 포인터다.
    // 크기와 객체 저장소 여부는 알 수 없으므로 json을 모르는 오래된 git-lfs에서만 쓰는 대체 해석이다.
    [[nodiscard]] std::vector<lfs_file> parse_git_lfs_files_long(std::string_view bytes);

    // `.gitattributes` 한 파일의 내용에 `filter=lfs` 규칙이 있는가. 주석(`#`)은 건너뛴다.
    [[nodiscard]] bool gitattributes_uses_lfs(std::string_view text);

    // `git check-attr -z --stdin filter`의 출력(`경로\0filter\0값\0` 반복)에서 값이 `lfs`인 경로다.
    [[nodiscard]] std::set<std::string> parse_git_check_attr_lfs_paths(std::string_view bytes);

    // `lfs/objects/ab/cd/<oid>`의 상대 경로다. oid가 64자리 16진수가 아니면 빈 문자열이다.
    [[nodiscard]] std::string lfs_object_relative_path(std::string_view oid);
} // namespace cyella
