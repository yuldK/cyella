#include "domain/credential_prompt.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

using namespace cyella;

// 아래 문구는 OpenSSH 10.2(시스템과 Git for Windows 두 빌드)와 git 2.52가 askpass에 넘긴 실제 값이다.
// 끝의 공백까지 원문 그대로다.

TEST_CASE("classifier reads an ssh key passphrase prompt", "[credential]")
{
    const credential_prompt prompt { classify_credential_prompt(u8"Enter passphrase for key 'C:/Users/x/.ssh/id_ed25519': ") };

    CHECK(prompt.kind == credential_prompt_kind::ssh_key_passphrase);
    CHECK(prompt.key_path == u8"C:/Users/x/.ssh/id_ed25519");
    CHECK(credential_prompt_expects_secret(prompt.kind));
    CHECK(prompt.raw_text == u8"Enter passphrase for key 'C:/Users/x/.ssh/id_ed25519': ");
}

TEST_CASE("classifier keeps spaces and non ascii characters in the key path", "[credential]")
{
    const credential_prompt prompt { classify_credential_prompt(u8"Enter passphrase for key 'C:/Users/홍 길동/.ssh/id rsa': ") };

    CHECK(prompt.kind == credential_prompt_kind::ssh_key_passphrase);
    CHECK(prompt.key_path == u8"C:/Users/홍 길동/.ssh/id rsa");
}

TEST_CASE("classifier reads the ssh-keygen and ssh-add passphrase shapes", "[credential]")
{
    const credential_prompt keygen_unlock { classify_credential_prompt(u8"Enter passphrase for \"g\": ") };
    CHECK(keygen_unlock.kind == credential_prompt_kind::ssh_key_passphrase);
    CHECK(keygen_unlock.key_path == u8"g");

    const credential_prompt agent_add { classify_credential_prompt(u8"Enter passphrase for /home/x/.ssh/id_rsa (will confirm each use): ") };
    CHECK(agent_add.kind == credential_prompt_kind::ssh_key_passphrase);
    CHECK(agent_add.key_path == u8"/home/x/.ssh/id_rsa");

    const credential_prompt no_file { classify_credential_prompt(u8"Enter passphrase: ") };
    CHECK(no_file.kind == credential_prompt_kind::ssh_key_passphrase);
    CHECK(no_file.key_path.empty());
}

TEST_CASE("classifier separates new key passphrase prompts from the unlock prompt", "[credential]")
{
    const credential_prompt with_file { classify_credential_prompt(u8"Enter passphrase for \"g\" (empty for no passphrase): ") };
    CHECK(with_file.kind == credential_prompt_kind::ssh_new_key_passphrase);
    CHECK(with_file.key_path == u8"g");

    const credential_prompt windows_build { classify_credential_prompt(u8"Enter passphrase (empty for no passphrase): ") };
    CHECK(windows_build.kind == credential_prompt_kind::ssh_new_key_passphrase);
    CHECK(windows_build.key_path.empty());

    const credential_prompt confirm { classify_credential_prompt(u8"Enter same passphrase again: ") };
    CHECK(confirm.kind == credential_prompt_kind::ssh_new_key_passphrase_confirm);
    CHECK(credential_prompt_expects_secret(confirm.kind));
}

TEST_CASE("classifier reads the system OpenSSH host key prompt", "[credential]")
{
    const std::u8string_view text = u8"The authenticity of host 'github.com (20.200.245.247)' can't be established.\n"
                                    u8"ED25519 key fingerprint is SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU.\n"
                                    u8"This key is not known by any other names.\n"
                                    u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ";

    const credential_prompt prompt { classify_credential_prompt(text) };

    CHECK(prompt.kind == credential_prompt_kind::ssh_host_key_confirm);
    CHECK(prompt.host == u8"github.com");
    CHECK(prompt.address == u8"20.200.245.247");
    CHECK(prompt.key_type == u8"ED25519");
    CHECK(prompt.fingerprint == u8"SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU");
    CHECK(credential_prompt_expects_yes_no(prompt.kind));
    CHECK_FALSE(credential_prompt_expects_secret(prompt.kind));
}

TEST_CASE("classifier reads the Git for Windows host key prompt with a colon", "[credential]")
{
    const std::u8string_view text = u8"The authenticity of host 'github.com (20.200.245.247)' can't be established.\n"
                                    u8"ED25519 key fingerprint is: SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU\n"
                                    u8"This key is not known by any other names.\n"
                                    u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ";

    const credential_prompt prompt { classify_credential_prompt(text) };

    CHECK(prompt.kind == credential_prompt_kind::ssh_host_key_confirm);
    CHECK(prompt.fingerprint == u8"SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU");
    CHECK(prompt.host == u8"github.com");
}

TEST_CASE("classifier tolerates crlf line endings and a bare host key question", "[credential]")
{
    const std::u8string_view windows_text = u8"The authenticity of host 'example.org' can't be established.\r\n"
                                            u8"RSA key fingerprint is SHA256:abc.\r\n"
                                            u8"Are you sure you want to continue connecting (yes/no)? ";
    const credential_prompt crlf { classify_credential_prompt(windows_text) };
    CHECK(crlf.kind == credential_prompt_kind::ssh_host_key_confirm);
    CHECK(crlf.host == u8"example.org");
    CHECK(crlf.address.empty());
    CHECK(crlf.key_type == u8"RSA");
    CHECK(crlf.fingerprint == u8"SHA256:abc");

    const credential_prompt bare { classify_credential_prompt(u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ") };
    CHECK(bare.kind == credential_prompt_kind::ssh_host_key_confirm);
    CHECK(bare.host.empty());
}

TEST_CASE("classifier reads an ssh password prompt", "[credential]")
{
    const credential_prompt prompt { classify_credential_prompt(u8"git@github.com's password: ") };

    CHECK(prompt.kind == credential_prompt_kind::ssh_password);
    CHECK(prompt.user_name == u8"git");
    CHECK(prompt.host == u8"github.com");
    CHECK(credential_prompt_expects_secret(prompt.kind));
}

TEST_CASE("classifier reads the https username prompt from git", "[credential]")
{
    const credential_prompt prompt { classify_credential_prompt(u8"Username for 'https://example.invalid': ") };

    CHECK(prompt.kind == credential_prompt_kind::https_username);
    CHECK(prompt.url == u8"https://example.invalid");
    CHECK(prompt.host == u8"example.invalid");
    CHECK(prompt.user_name.empty());
    CHECK_FALSE(credential_prompt_expects_secret(prompt.kind));
}

TEST_CASE("classifier reads the https password prompt from git", "[credential]")
{
    const credential_prompt prompt { classify_credential_prompt(u8"Password for 'https://bob@example.invalid': ") };

    CHECK(prompt.kind == credential_prompt_kind::https_password);
    CHECK(prompt.url == u8"https://bob@example.invalid");
    CHECK(prompt.user_name == u8"bob");
    CHECK(prompt.host == u8"example.invalid");
    CHECK(credential_prompt_expects_secret(prompt.kind));

    const credential_prompt with_port { classify_credential_prompt(u8"Password for 'https://alice@git.example.com:8443/team/repo.git': ") };
    CHECK(with_port.user_name == u8"alice");
    CHECK(with_port.host == u8"git.example.com:8443");
}

TEST_CASE("classifier falls back to unknown for unrecognized text", "[credential]")
{
    const std::vector<std::u8string_view> samples { u8"", u8"Enter your OATH response: ", u8"사용자 이름: ", u8"Enter passphrase for key 'unterminated: ", u8"Password for 'https://x@y" };
    for (const std::u8string_view text : samples)
    {
        const credential_prompt prompt { classify_credential_prompt(text) };
        CHECK(prompt.kind == credential_prompt_kind::unknown);
        CHECK(prompt.raw_text == text);
        CHECK_FALSE(credential_prompt_expects_secret(prompt.kind));
        CHECK_FALSE(credential_prompt_expects_yes_no(prompt.kind));
    }
}
