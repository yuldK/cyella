#pragma once

#include <string>
#include <string_view>

namespace cyella {
    // askpass 프로그램이 받는 `argv[1]`의 종류다. git과 OpenSSH가 물어 보는 문구는 로캘에
    // 따라 번역될 수 있어, 분류는 영어 원문에 대한 최선 노력이다. 알아보지 못하면 `unknown`이며
    // 그때는 원문을 그대로 보여 주고 자유 텍스트를 받는다.
    enum class credential_prompt_kind
    {
        unknown,
        // 기존 키의 passphrase다 (`Enter passphrase for key '<경로>':`).
        ssh_key_passphrase,
        // `ssh-keygen`이 새 키에 거는 passphrase다. 빈 답이 "passphrase 없음"이라는 유효한 답이다.
        ssh_new_key_passphrase,
        // `ssh-keygen`이 같은 passphrase를 한 번 더 묻는 확인 문구다.
        ssh_new_key_passphrase_confirm,
        // 처음 보는 호스트 키를 믿을지 묻는다. 답은 문자 그대로 `yes` 또는 `no`다.
        ssh_host_key_confirm,
        // `git@host's password:`다.
        ssh_password,
        https_username,
        https_password,
    };

    struct credential_prompt
    {
        credential_prompt_kind kind { credential_prompt_kind::unknown };
        // 도우미가 받은 문구 그대로다. 대화 상자의 대체 표시와 `unknown`의 본문이 된다.
        std::u8string raw_text {};
        // 키 passphrase 부류의 키 파일 경로다. 문구에 파일 이름이 없으면 비어 있다.
        std::u8string key_path {};
        // https 부류에서 문구에 적힌 URL이다 (사용자 정보를 포함할 수 있다).
        std::u8string url {};
        // https 비밀번호의 URL 사용자 정보, 또는 ssh 비밀번호의 계정이다.
        std::u8string user_name {};
        // 접속 대상 호스트다. 호스트 키 확인에서는 `github.com (140.82.112.3)` 중 앞부분이다.
        std::u8string host {};
        // 호스트 키 확인에서 괄호 안에 적힌 IP 주소다.
        std::u8string address {};
        // 호스트 키 확인의 키 종류다 (`ED25519`).
        std::u8string key_type {};
        // 호스트 키 확인의 지문이다 (`SHA256:...`).
        std::u8string fingerprint {};
    };

    // 문구를 분류한다. 끝의 공백과 줄 바꿈은 무시하며 예외를 던지지 않는다.
    [[nodiscard]] credential_prompt classify_credential_prompt(std::u8string_view text);

    // 비밀(passphrase, 비밀번호)을 받는 부류인지다. 대화 상자는 입력을 가리고, 답은 기록하지 않는다.
    [[nodiscard]] bool credential_prompt_expects_secret(credential_prompt_kind kind) noexcept;

    // 답이 문자 그대로 `yes` 또는 `no`여야 하는 부류인지다.
    [[nodiscard]] bool credential_prompt_expects_yes_no(credential_prompt_kind kind) noexcept;

    [[nodiscard]] std::u8string_view credential_prompt_kind_name(credential_prompt_kind kind) noexcept;
} // namespace cyella
