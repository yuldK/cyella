#pragma once

#include "application/process_request.h"
#include "domain/ssh_connection.h"
#include "infrastructure/askpass_environment.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // ---- ssh-agent 서비스 ----

    enum class ssh_agent_service_status
    {
        running,
        stopped,
        // 시작 유형이 "사용 안 함"이라 시작할 수 없다. 서비스 시작 유형을 바꾸려면 관리자 권한이 필요하다.
        disabled,
        not_installed,
    };

    // 서비스 제어 관리자(SCM)에서 읽은 값이다. 판정 규칙을 OS 없이 시험하려고 분리했다.
    struct ssh_agent_service_snapshot
    {
        bool installed { false };
        bool running { false };
        bool start_disabled { false };
    };

    // 실행 중이면 시작 유형과 상관없이 `running`이다.
    [[nodiscard]] ssh_agent_service_status classify_ssh_agent_service(const ssh_agent_service_snapshot& snapshot) noexcept;
    [[nodiscard]] std::u8string_view ssh_agent_service_status_name(ssh_agent_service_status status) noexcept;

    // ---- ssh-add -l ----

    struct ssh_agent_identity
    {
        int bits {};
        std::u8string fingerprint {};
        std::u8string comment {};
        std::u8string key_type {};
    };

    enum class ssh_agent_list_state
    {
        identities,
        // 종료 코드 1: 에이전트는 있으나 등록된 키가 없다.
        no_identities,
        // 종료 코드 2: 에이전트에 연결할 수 없다.
        agent_unavailable,
        failed,
    };

    struct ssh_agent_list
    {
        ssh_agent_list_state state { ssh_agent_list_state::failed };
        std::vector<ssh_agent_identity> identities {};
    };

    // `ssh-add -l`의 종료 코드와 표준 출력으로 목록을 만든다. 줄은 `<bits> <지문> <주석> (<종류>)`다.
    [[nodiscard]] ssh_agent_list parse_ssh_add_list(int exit_code, std::u8string_view standard_output);
    [[nodiscard]] process_request make_ssh_add_list_request(std::u8string_view ssh_add_executable, std::u8string_view working_directory);

    // ---- ~/.ssh의 키 쌍 ----

    struct ssh_key_pair
    {
        // 확장자 없는 파일 이름이다 (`id_ed25519`).
        std::u8string name {};
        std::filesystem::path private_key_path {};
        std::filesystem::path public_key_path {};
        // 공개 키 파일의 첫 줄에서 읽은 값이다. 읽지 못하면 비어 있다.
        std::u8string key_type {};
        std::u8string comment {};
    };

    // `<이름>`과 `<이름>.pub`이 모두 일반 파일인 쌍을 이름순으로 돌려준다. 디렉터리가 없거나 읽을 수
    // 없으면 빈 목록이다. 개인 키 파일은 열지 않는다.
    [[nodiscard]] std::vector<ssh_key_pair> list_ssh_key_pairs(const std::filesystem::path& ssh_directory);

    // 공개 키 한 줄(`ssh-ed25519 AAAA... 주석`)에서 종류와 주석을 꺼낸다.
    [[nodiscard]] std::optional<ssh_key_pair> parse_public_key_line(std::u8string_view line);

    // ---- ssh-keygen ----

    enum class ssh_keygen_request_error
    {
        none,
        askpass_missing,
        invalid_key_path,
        invalid_comment,
        key_exists,
    };

    struct ssh_keygen_options
    {
        std::u8string ssh_keygen_executable {};
        // 만들 개인 키의 절대 경로다. `<경로>.pub`도 함께 생긴다.
        std::filesystem::path key_path {};
        std::u8string comment {};
        std::u8string askpass_helper {};
        askpass_session askpass {};
    };

    struct ssh_keygen_request_result
    {
        process_request request {};
        ssh_keygen_request_error error { ssh_keygen_request_error::none };
    };

    // `ssh-keygen -t ed25519 -C <주석> -f <경로>`를 만든다.
    //
    // passphrase는 명령줄 `-N`으로 주지 않는다. 명령줄은 작업 로그와 프로세스 목록에 남기 때문이다.
    // ssh-keygen 10.2는 `-N`이 없으면 `SSH_ASKPASS`와 `SSH_ASKPASS_REQUIRE=force`로 askpass를 부르므로
    // (`Enter passphrase (empty for no passphrase)`, `Enter same passphrase again`) 앱의 대화 상자가
    // 답한다. 이미 있는 파일이면 덮어쓸지 묻는 프롬프트가 askpass로 가지 않고 실패하므로, 파일이
    // 있으면 요청을 만들지 않는다. askpass 도우미가 없어도 만들지 않는다.
    [[nodiscard]] ssh_keygen_request_result make_ssh_keygen_request(const ssh_keygen_options& options);

    // 지문 확인용 `ssh-keygen -l -f <공개 키>`다.
    [[nodiscard]] process_request make_ssh_keygen_fingerprint_request(std::u8string_view ssh_keygen_executable, const std::filesystem::path& public_key_path);

    // ---- 연결 시험 (ssh -T) ----

    struct ssh_connection_test_options
    {
        std::u8string ssh_executable {};
        std::u8string host {};
        std::u8string user_name { u8"git" };
        // 0이면 ssh 기본값이다.
        std::uint16_t port {};
        std::u8string working_directory {};
        std::u8string askpass_helper {};
        askpass_session askpass {};
    };

    // `ssh -T -o StrictHostKeyChecking=ask git@<host>`다. 호스트나 계정에 공백, 따옴표, 앞의 `-`가
    // 있으면 옵션 주입이 될 수 있어 만들지 않는다.
    [[nodiscard]] std::optional<process_request> make_ssh_connection_test_request(const ssh_connection_test_options& options);

    // 종료 코드와 두 출력을 합친 텍스트로 시험 결과를 분류한다. 영어 문구에 대한 최선 노력이며
    // 모르면 `unknown`이다.
    [[nodiscard]] ssh_connection_classification classify_ssh_connection_test(int exit_code, std::u8string_view standard_output, std::u8string_view standard_error);

    // ---- "이번만 연결" ----

    // 호스트 키를 한 번만 믿고 연결하는 git 실행의 `GIT_SSH_COMMAND` 값이다.
    // `<ssh> -o UserKnownHostsFile=<임시 파일> -o StrictHostKeyChecking=accept-new`다.
    // 사용자가 지문을 이미 확인했으므로 임시 파일에는 확인 없이 기록해도 되고, 사용자의 known_hosts는
    // 바뀌지 않는다. 이 값은 사용자의 `core.sshCommand`를 그 한 번의 실행에서 덮어쓰므로 호출자는
    // 사용자가 쓰는 ssh 실행 파일을 넘겨야 한다. 값은 git이 sh 규칙으로 해석하므로 경로를 작은따옴표로
    // 감싼다. ssh 인자 목록은 `make_temporary_known_hosts_ssh_arguments`로 따로 얻는다.
    [[nodiscard]] std::u8string make_temporary_known_hosts_ssh_command(std::u8string_view ssh_executable, const std::filesystem::path& known_hosts_path);
    [[nodiscard]] std::vector<std::u8string> make_temporary_known_hosts_ssh_arguments(const std::filesystem::path& known_hosts_path);
} // namespace cyella
