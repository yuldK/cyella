#pragma once

#include "domain/signing_config.h"
#include "infrastructure/git_command_builder.h"

#include <span>
#include <string>
#include <string_view>

// 커밋 서명의 명령줄 조립기다. 프로세스를 실행하지 않는다. 인자가 잘못되면 `std::invalid_argument`다.
namespace cyella {
    // `[-c gpg.ssh.allowedSignersFile=] log --no-walk=unsorted --stdin -z --format=%H%x1f%G?%x1f%GS%x1f%GK`. 커밋 SHA는 줄마다 하나씩
    // stdin으로 넘긴다 (`make_git_verify_signatures_input`). 명령줄 길이에 걸리지 않고 SHA가 옵션처럼 읽힐 수도 없다.
    //
    // `blank_allowed_signers`가 참이면 `gpg.ssh.allowedSignersFile`을 빈 값으로 덮는다. 이 설정이 없으면 git은 ssh 서명을
    // 검증하지 못하고 `N`(서명 없음)으로 보고해서 서명 없는 커밋과 구별할 수 없다. 빈 값이면 서명은 읽히고 서명자 목록에 없는
    // 키로서 `U`와 지문이 나온다.
    [[nodiscard]] git_arguments make_git_verify_signatures_args(bool blank_allowed_signers);

    // 위 명령의 stdin이다. SHA가 아닌 값이 있으면 `std::invalid_argument`다.
    [[nodiscard]] std::string make_git_verify_signatures_input(std::span<const std::string> shas);

    // `config -z --show-scope --get-regexp ^(commit\.gpgsign|tag\.gpgsign|gpg\.format|user\.signingkey|gpg\.program|gpg\.ssh\.program|gpg\.ssh\.allowedsignersfile)$`.
    // 설정이 하나도 없으면 종료 코드 1이다. 실효 값과 지역·전역을 한 번에 얻는다.
    [[nodiscard]] git_arguments make_git_signing_config_args();

    // `config --type=path --get gpg.ssh.allowedsignersfile`. `~/`를 펼친 절대 경로를 받는다. 설정이 없으면 종료 코드 1이다.
    [[nodiscard]] git_arguments make_git_allowed_signers_path_args();

    // `config --local|--global <key> <value>`. 서명 설정 일곱 가지 키만 받는다. 값에 NUL·줄바꿈이 있으면 거절한다.
    [[nodiscard]] git_arguments make_git_signing_config_set_args(signing_key_id key, std::string_view value, bool global);

    // `config --local|--global --unset <key>`. 설정이 없으면 종료 코드 5다.
    [[nodiscard]] git_arguments make_git_signing_config_unset_args(signing_key_id key, bool global);

    // gpg 실행 파일에 넘길 인자다: `--list-secret-keys --with-colons --batch --fixed-list-mode --with-fingerprint`.
    [[nodiscard]] git_arguments make_gpg_list_secret_keys_args();

    // 서명 설정을 읽는 git 명령 두 곳(refresh와 환경 설정)이 같은 정규식을 쓰게 하는 값이다.
    [[nodiscard]] std::string_view signing_config_regexp() noexcept;
} // namespace cyella
