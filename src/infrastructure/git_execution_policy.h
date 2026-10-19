#pragma once

#include "application/process_request.h"
#include "infrastructure/askpass_environment.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 명령 부류별 제한이다. 실제 값은 한곳에 모아 두어 설정으로 바꾸기 쉽게 한다.
    enum class git_command_class
    {
        // `--version` 확인이다. 응답 없는 실행 파일이 시작을 막지 않게 짧게 둔다.
        tool_probe,
        // index·ref를 바꾸지 않는 로컬 조회다. status·log·diff·show.
        local_query,
        // 로컬 저장소를 바꾸는 작업이다. checkout·commit·merge·rebase·stash.
        local_write,
        // fetch·pull·push·clone처럼 네트워크를 쓰는 작업이다. 느린 회선과 인증 대화 상자를
        // 기다리는 시간을 가늠해 자르지 않는다. 종료는 사용자의 취소가 맡는다.
        network,
    };

    struct git_command_limits
    {
        // 값이 없으면 무제한이다. 취소만으로 종료를 제어한다.
        std::optional<std::chrono::milliseconds> timeout {};
        std::size_t maximum_captured_bytes_per_stream {};
    };

    [[nodiscard]] git_command_limits git_limits_for(git_command_class command_class) noexcept;

    // 요청마다 달라지는 값이다. 기본값은 가장 흔한 줄 단위 비대화형 실행이다.
    struct git_request_options
    {
        // 인증 도우미 실행 파일의 절대 경로다. 비어 있으면 askpass 변수를 환경에서 지운다.
        // 도우미가 없는 상태에서 인증이 필요하면 `GIT_TERMINAL_PROMPT=0` 때문에 즉시 실패한다.
        std::u8string askpass_helper {};
        // 도우미가 앱에 접속하는 값이다. `askpass_helper`가 있을 때만 `CYELLA_ASKPASS_*`로 전달한다.
        askpass_session askpass {};
        // 자식의 표준 입력이다 (`commit -F -`, `apply --cached`, `cat-file --batch`).
        std::string standard_input {};
        // 기본값을 넘길 수 있는 줄(rename 항목을 담은 status 등)만 올린다.
        std::size_t maximum_record_bytes { default_process_record_byte_limit };
        // true면 stdout을 가공 없이 조각으로 받는다 (`log -z`, `diff --binary`, `cat-file`).
        // stderr는 진행 표시를 줄 단위로 받아야 하므로 항상 `lines`다.
        bool raw_standard_output { false };
        // 부류별 기본 캡처 상한을 바꾼다. 해석기가 조각을 바로 소비해 버퍼에 쌓지 않는
        // 스트리밍 요청은 `unlimited_process_capture_bytes`를 넘긴다.
        std::optional<std::size_t> maximum_captured_bytes_per_stream {};
    };

    // git을 비대화형으로 실행하는 환경 override다.
    //
    // - `GIT_TERMINAL_PROMPT=0`: 터미널 프롬프트를 막는다.
    // - 인증은 askpass 도우미로만 받는다. 도우미가 있으면 `GIT_ASKPASS`·`SSH_ASKPASS`와
    //   `SSH_ASKPASS_REQUIRE=force`를 설정하고, 없으면 사용자 환경에서 온 값을 지운다.
    // - `GIT_SSH_COMMAND`와 `BatchMode`는 건드리지 않는다. 사용자의 `core.sshCommand`와
    //   ssh 설정이 터미널에서와 똑같이 적용되어야 한다.
    // - `GCM_INTERACTIVE`도 건드리지 않는다. Git Credential Manager가 자기 UI를 띄울 수 있다.
    // - `GIT_OPTIONAL_LOCKS` 환경 변수는 건드리지 않는다. 읽기 조회(`local_query`)는 대신
    //   `--no-optional-locks` 인자를 받는다(`make_git_process_request`). 읽기는 쓰기와 다른
    //   lane에서 병렬로 돌므로, status가 index 갱신을 위해 `index.lock`을 잡으면 같은 저장소의
    //   쓰기가 잠금 오류로 실패한다.
    // - `LC_ALL=C`: 오류 분류기와 `%(upstream:track)`(`[ahead 1, behind 2]`)가 영어 문구를
    //   기대한다. 경로는 `core.quotepath=false`로 UTF-8 원문이 나오므로 로캘과 무관하다.
    // - `GIT_EDITOR=true`: `rebase --continue`·`merge`·`revert`가 편집기를 띄워 worker를
    //   붙잡지 않게 한다. 메시지는 git의 기본값이나 stdin(`-F -`)으로 정한다.
    [[nodiscard]] std::vector<process_environment_override> git_environment_overrides(std::u8string_view askpass_helper = {});

    // 모든 git 명령 앞에 붙는 공통 인자다. `-c gc.auto=0`은 명령이 background 유지보수
    // 프로세스를 띄워 출력 pipe를 물고 늘어지는 것을 막는다.
    [[nodiscard]] std::vector<std::u8string> git_common_arguments();

    // 공통 정책을 적용한 요청을 만든다. `arguments`는 공통 인자 뒤에 붙는다. 부류가
    // `local_query`면 공통 인자 끝에 `--no-optional-locks`를 더한다. git의 배경 읽기
    // 방식이며 status가 index를 다시 쓰지 않아 `index.lock`을 잡지 않는다.
    [[nodiscard]] process_request make_git_process_request(
        std::u8string_view executable, std::u8string_view working_directory, std::vector<std::u8string> arguments, git_command_class command_class, const git_request_options& options = {});
} // namespace cyella
