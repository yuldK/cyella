#pragma once

#include "domain/commit_signature.h"
#include "domain/gpg_key.h"
#include "domain/signing_config.h"

#include <string_view>
#include <vector>

// 서명 관련 git·gpg 출력 해석기다. 모두 프로세스를 실행하지 않는 순수 함수다.
namespace cyella {
    // `log --no-walk -z --format=%H%x1f%G?%x1f%GS%x1f%GK`의 출력이다. 커밋마다 NUL로 끝난다. 모양이 틀린 항목은 건너뛴다.
    [[nodiscard]] std::vector<commit_signature> parse_git_signature_records(std::string_view bytes);

    // `config -z --show-scope --get-regexp <서명 키들>`의 출력이다. 항목은 `scope\0key\nvalue\0`이고 `=` 없는 불리언은 `scope\0key\0`다.
    // 실효 값은 마지막에 나온 항목(git이 읽는 순서가 우선순위 순이다)이고, 지역·전역은 그 scope의 마지막 항목이다.
    // 서명과 관계없는 키는 무시한다.
    [[nodiscard]] signing_config_layers parse_git_signing_config(std::string_view bytes);

    // `gpg --list-secret-keys --with-colons`의 출력이다. 주 키(`sec`)마다 지문(`fpr`)과 사용자 ID(`uid`)를 모은다.
    // 하위 키(`ssb`)는 서명 능력만 주 키에 보탠다.
    [[nodiscard]] std::vector<gpg_secret_key> parse_gpg_secret_keys(std::string_view colon_listing, std::int64_t now);
} // namespace cyella
