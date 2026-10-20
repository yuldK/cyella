#pragma once

#include "application/file_probe.h"
#include "application/process_cancellation.h"
#include "application/process_runner.h"
#include "domain/gpg_key.h"
#include "domain/signing_config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 환경 설정 "서명" 화면이 쓰는 값을 읽고 쓴다. 프로세스를 띄우므로 worker에서 부른다. 어떤 실패도 값으로 돌려주고 예외를 던지지 않는다.
// 저장소 operation이 아닌 이유: 저장소가 없어도(시작 탭) 전역 설정을 볼 수 있어야 하고, 설정 파일 몇 줄을 읽고 쓰는 일이라 작업 로그에 남길 일이
// 아니다. SSH 화면의 `collect_ssh_info`와 같은 길이다.
namespace cyella {
    struct signing_info_sources
    {
        process_runner* runner { nullptr };
        std::u8string git_executable {};
        // 지역 설정을 읽고 쓸 저장소 폴더다. 비면 저장소 없이 전역 설정만 다룬다.
        std::u8string repository_path {};
        // 저장소가 없을 때 git을 돌릴 폴더다 (절대 경로). 어느 폴더든 전역 설정은 같다.
        std::u8string fallback_directory {};
        const file_probe* probe { nullptr };
        process_cancellation_token cancellation {};
        // 키 만료 판정의 기준 시각(유닉스 초)이다. 0이면 지금이다.
        std::int64_t now { 0 };
        // 거짓이면 gpg 키 목록을 읽지 않는다 (`gpg_pending`을 올려 둔다). gpg는 agent를 띄우느라 수 초 걸릴 수 있어 설정을 먼저 보이고
        // `read_signing_gpg_keys`로 따로 채운다.
        bool read_gpg_keys { true };
    };

    struct signing_info_snapshot
    {
        // 저장소 폴더를 받아 읽었다 (지역 설정을 바꿀 수 있다).
        bool repository { false };
        // git config를 읽었다. 읽지 못했으면 `config`는 비어 있고 화면이 이유를 알린다.
        bool config_read { false };
        signing_config_layers config {};
        // Git for Windows의 `usr\bin\gpg.exe`다. git은 `gpg.program`이 없으면 `gpg`를 PATH에서 찾는데, git이 자기 `usr\bin`을 PATH에 더하므로
        // 이 파일이 보통 쓰인다. 못 찾으면 비어 있다.
        std::u8string default_gpg {};
        // 키 목록을 읽는 데 쓴 실행 파일이다 (`gpg.program`이 절대 경로면 그것, 아니면 `default_gpg`).
        std::u8string gpg_executable {};
        bool gpg_keys_read { false };
        // 키 목록을 아직 읽는 중이다 (`read_gpg_keys`가 거짓이었다).
        bool gpg_pending { false };
        std::vector<gpg_secret_key> gpg_keys {};
        // 키 목록을 읽지 못한 이유다 (한국어 한 줄). 읽었으면 비어 있다.
        std::u8string gpg_problem {};
        // `gpg.ssh.allowedSignersFile`을 `~`까지 펼친 경로와 그 파일이 있는지다. 설정이 없으면 경로가 비어 있다.
        std::u8string allowed_signers_path {};
        bool allowed_signers_exists { false };

        [[nodiscard]] bool operator==(const signing_info_snapshot&) const noexcept = default;
    };

    // Git for Windows의 gpg를 찾는다: git 실행 파일(`<루트>\cmd\git.exe`, `<루트>\mingw64\bin\git.exe`)의 위쪽 `usr\bin\gpg.exe`. 없으면 비어 있다.
    [[nodiscard]] std::u8string locate_default_gpg(std::u8string_view git_executable, const file_probe& probe);

    // 설정(실효·지역·전역)과 gpg 비밀 키 목록, allowed signers 파일을 읽는다.
    [[nodiscard]] signing_info_snapshot collect_signing_info(const signing_info_sources& sources) noexcept;

    // gpg 비밀 키 목록을 읽어 `info`를 채운다 (`info.config`를 읽은 뒤에 부른다). `collect_signing_info`가 `read_gpg_keys`일 때 한다.
    void read_signing_gpg_keys(const signing_info_sources& sources, signing_info_snapshot& info) noexcept;

    // 설정 하나를 바꾼다. 값이 없으면 지운다.
    struct signing_change
    {
        signing_key_id key { signing_key_id::commit_gpgsign };
        std::optional<std::string> value {};
    };

    struct signing_write_result
    {
        bool ok { false };
        // 실패했을 때의 이유다 (한국어 한 줄, 끝에 git의 원문 첫 줄).
        std::u8string error {};
    };

    // `git config --local|--global`로 변경을 차례로 적용한다. 전역은 사용자의 실제 설정을 바꾸므로 호출자(화면)가 확인을 받은 뒤에만 부른다.
    // 지역은 `repository_path`가 있어야 한다. 하나라도 실패하면 거기서 멈추고 그 이유를 돌려준다 (앞서 적용한 것은 그대로다).
    [[nodiscard]] signing_write_result write_signing_config(const signing_info_sources& sources, bool global, const std::vector<signing_change>& changes) noexcept;
} // namespace cyella
