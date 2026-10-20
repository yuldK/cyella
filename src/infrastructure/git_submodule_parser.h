#pragma once

#include "domain/file_change.h"
#include "domain/submodule.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // `git submodule status --recursive`의 줄 하나다.
    struct submodule_status_entry
    {
        // 첫 글자다. ` ` 초기화됨·일치, `-` 초기화 안 됨, `+` 체크아웃한 커밋이 기록과 다르다, `U` 병합 충돌.
        char prefix { ' ' };
        // `-`면 기록된 커밋이고 그 밖에는 체크아웃된 커밋이다. `U`는 영(0)으로만 채워진 값이다.
        std::string sha {};
        std::string path {};
        // 끝의 `(heads/main)`이다. 초기화하지 않았으면 없다.
        std::string description {};
    };

    // `.gitmodules`의 구역 하나다 (`config -f .gitmodules -z --get-regexp '^submodule[.]'`).
    struct gitmodules_entry
    {
        std::string name {};
        std::string path {};
        std::string url {};
    };

    // `ls-files --stage -z`가 준 gitlink(모드 160000) 하나다. 상위 저장소 index가 기록한 커밋이다.
    struct gitlink_entry
    {
        std::string path {};
        std::string sha {};
        // 병합 중이면 1·2·3, 아니면 0이다.
        int stage { 0 };
    };

    [[nodiscard]] std::vector<submodule_status_entry> parse_git_submodule_status(std::string_view bytes);
    [[nodiscard]] std::vector<gitmodules_entry> parse_gitmodules_config(std::string_view bytes);
    // 모드가 160000인 항목만 돌려준다.
    [[nodiscard]] std::vector<gitlink_entry> parse_git_gitlinks(std::string_view bytes);

    // status 줄과 .gitmodules, index 기록, 작업 트리 변경을 합쳐 사이드바가 쓰는 목록을 만든다.
    //
    // - `-`는 uninitialized, `U`는 conflict다.
    // - ` `는 initialized이고, 상위 저장소 status가 내용 변경(`M`)이나 추적 안 된 파일(`U`)을 알리면 modified다.
    // - `+`는 체크아웃한 커밋이 기록과 다르다. 기록한 커밋이 체크아웃한 커밋의 조상이면(submodule에서 앞서 나갔다)
    //   modified, 그 밖에는(뒤처졌거나 갈라졌거나 기록한 커밋이 없다) out_of_date다. 조상 여부는 `descends`가
    //   답하며 모르면(값이 없으면) out_of_date다.
    //
    // 최상위 submodule의 url·name은 .gitmodules에서, 기록된 커밋은 `gitlinks`에서 온다. 중첩 submodule은 둘 다 없다.
    struct submodule_sources
    {
        std::vector<submodule_status_entry> status {};
        std::vector<gitmodules_entry> modules {};
        std::vector<gitlink_entry> gitlinks {};
        // 상위 저장소 status의 submodule 항목이다.
        std::vector<file_change> changes {};
    };

    // descends(info): 기록된 커밋(info.recorded_sha)이 체크아웃한 커밋(info.checked_out_sha)의 조상인가.
    [[nodiscard]] std::vector<submodule_info> assemble_submodules(const submodule_sources& sources, const std::function<std::optional<bool>(const submodule_info&)>& descends);
} // namespace cyella
