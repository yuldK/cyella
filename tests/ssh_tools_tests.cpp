#include "infrastructure/ssh_tools.h"
#include "platform/win32/win32_ssh_environment.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    const process_environment_override* find_override(const std::vector<process_environment_override>& overrides, const std::u8string_view name)
    {
        for (const process_environment_override& entry : overrides)
            if (entry.name == name)
                return &entry;
        return nullptr;
    }

    // 임시 디렉터리를 만들고 test가 끝나면 지운다.
    class scratch_directory
    {
    public:
        scratch_directory()
        {
            static int counter { 0 };
            path_ = std::filesystem::temp_directory_path() / ("cyella-ssh-tools-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(counter++));
            std::filesystem::create_directories(path_);
        }

        scratch_directory(const scratch_directory&) = delete;
        scratch_directory& operator=(const scratch_directory&) = delete;

        ~scratch_directory()
        {
            std::error_code error {};
            std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

        void write(const std::filesystem::path& relative, const std::string& text) const
        {
            std::ofstream stream { path_ / relative, std::ios::binary };
            stream << text;
        }

    private:
        std::filesystem::path path_ {};
    };

    askpass_session sample_session()
    {
        return askpass_session { u8"\\\\.\\pipe\\cyella-askpass-1-abc", u8"deadbeef", u8"op-1" };
    }
} // namespace

TEST_CASE("agent service status follows install and start type", "[ssh][agent]")
{
    CHECK(classify_ssh_agent_service({ false, false, false }) == ssh_agent_service_status::not_installed);
    CHECK(classify_ssh_agent_service({ true, true, false }) == ssh_agent_service_status::running);
    CHECK(classify_ssh_agent_service({ true, false, false }) == ssh_agent_service_status::stopped);
    CHECK(classify_ssh_agent_service({ true, false, true }) == ssh_agent_service_status::disabled);
    // 시작 유형이 사용 안 함이어도 이미 돌고 있으면 쓸 수 있다.
    CHECK(classify_ssh_agent_service({ true, true, true }) == ssh_agent_service_status::running);
}

TEST_CASE("the real service control manager answers with one of the four states", "[ssh][agent][integration]")
{
    const ssh_agent_service_status status { win32::query_ssh_agent_service_status() };

    CHECK((status == ssh_agent_service_status::running || status == ssh_agent_service_status::stopped || status == ssh_agent_service_status::disabled
        || status == ssh_agent_service_status::not_installed));
}

TEST_CASE("ssh-add -l output lists fingerprint comment and type", "[ssh][agent]")
{
    // OpenSSH 10.2 ssh-add -l의 실제 출력이다. 두 번째 줄은 주석에 공백과 괄호가 있다.
    const std::u8string_view output = u8"256 SHA256:96TYRhZmu7gGQlefRcVoklrPE6LhreQdhjEtkwQUFpI c@y (ED25519)\n"
                                      u8"2048 SHA256:yLjsN5tdDXvFgO60+CoMC0G7MOB72M1c9i443H9A3aA work laptop (main) (RSA)\n";
    const ssh_agent_list list { parse_ssh_add_list(0, output) };

    REQUIRE(list.state == ssh_agent_list_state::identities);
    REQUIRE(list.identities.size() == 2);
    CHECK(list.identities[0].bits == 256);
    CHECK(list.identities[0].fingerprint == u8"SHA256:96TYRhZmu7gGQlefRcVoklrPE6LhreQdhjEtkwQUFpI");
    CHECK(list.identities[0].comment == u8"c@y");
    CHECK(list.identities[0].key_type == u8"ED25519");
    CHECK(list.identities[1].bits == 2048);
    CHECK(list.identities[1].comment == u8"work laptop (main)");
    CHECK(list.identities[1].key_type == u8"RSA");
}

TEST_CASE("ssh-add -l handles crlf lines and md5 fingerprints", "[ssh][agent]")
{
    const ssh_agent_list list { parse_ssh_add_list(0, u8"256 MD5:88:c3:51:1d:c7:18:d9:b1:ca:b7:69:56:88:2c:8b:52 c@y (ED25519)\r\n") };

    REQUIRE(list.identities.size() == 1);
    CHECK(list.identities[0].fingerprint == u8"MD5:88:c3:51:1d:c7:18:d9:b1:ca:b7:69:56:88:2c:8b:52");
    CHECK(list.identities[0].comment == u8"c@y");
}

TEST_CASE("ssh-add -l exit codes map to agent states", "[ssh][agent]")
{
    // 실제 출력: 종료 코드 1은 "The agent has no identities.", 2는 에이전트 연결 실패다.
    const ssh_agent_list empty { parse_ssh_add_list(1, u8"The agent has no identities.\n") };
    CHECK(empty.state == ssh_agent_list_state::no_identities);
    CHECK(empty.identities.empty());

    CHECK(parse_ssh_add_list(2, u8"Could not open a connection to your authentication agent.\n").state == ssh_agent_list_state::agent_unavailable);
    CHECK(parse_ssh_add_list(2, u8"Error connecting to agent: No such file or directory\n").state == ssh_agent_list_state::agent_unavailable);
    CHECK(parse_ssh_add_list(255, u8"").state == ssh_agent_list_state::failed);
    // 성공인데 읽을 수 있는 줄이 없으면 실패로 본다.
    CHECK(parse_ssh_add_list(0, u8"garbage\n").state == ssh_agent_list_state::failed);
}

TEST_CASE("public key lines give type and comment", "[ssh][keys]")
{
    const std::optional<ssh_key_pair> pair { parse_public_key_line(u8"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIL3s my laptop key\r\n") };
    REQUIRE(pair.has_value());
    CHECK(pair->key_type == u8"ssh-ed25519");
    CHECK(pair->comment == u8"my laptop key");

    const std::optional<ssh_key_pair> bare { parse_public_key_line(u8"ssh-rsa AAAAB3NzaC1yc2E") };
    REQUIRE(bare.has_value());
    CHECK(bare->comment.empty());

    CHECK_FALSE(parse_public_key_line(u8"-----BEGIN OPENSSH PRIVATE KEY-----").has_value());
    CHECK_FALSE(parse_public_key_line(u8"").has_value());
}

TEST_CASE("key pairs need both files and are sorted by name", "[ssh][keys]")
{
    const scratch_directory directory {};
    directory.write("id_rsa", "private");
    directory.write("id_rsa.pub", "ssh-rsa AAAA user@host\n");
    directory.write("id_ed25519", "private");
    directory.write("id_ed25519.pub", "ssh-ed25519 AAAA mine\n");
    directory.write("lonely_key", "private without public");
    directory.write("orphan.pub", "ssh-rsa AAAA orphan\n");
    directory.write("known_hosts", "github.com ssh-ed25519 AAAA");
    directory.write("config", "Host *");

    const std::vector<ssh_key_pair> pairs { list_ssh_key_pairs(directory.path()) };

    REQUIRE(pairs.size() == 2);
    CHECK(pairs[0].name == u8"id_ed25519");
    CHECK(pairs[0].key_type == u8"ssh-ed25519");
    CHECK(pairs[0].comment == u8"mine");
    CHECK(pairs[0].private_key_path == directory.path() / "id_ed25519");
    CHECK(pairs[0].public_key_path == directory.path() / "id_ed25519.pub");
    CHECK(pairs[1].name == u8"id_rsa");
    CHECK(pairs[1].comment == u8"user@host");

    CHECK(list_ssh_key_pairs(directory.path() / "missing").empty());
}

TEST_CASE("keygen request routes the passphrase through askpass and never through argv", "[ssh][keygen]")
{
    const scratch_directory directory {};
    ssh_keygen_options options {};
    options.ssh_keygen_executable = u8"C:/Program Files/Git/usr/bin/ssh-keygen.exe";
    options.key_path = directory.path() / "id_ed25519";
    options.comment = u8"alice@laptop";
    options.askpass_helper = u8"C:/cyella/bin/cyella-askpass.exe";
    options.askpass = sample_session();

    const ssh_keygen_request_result result { make_ssh_keygen_request(options) };

    REQUIRE(result.error == ssh_keygen_request_error::none);
    const std::vector<std::u8string> expected { u8"-t", u8"ed25519", u8"-C", u8"alice@laptop", u8"-f", options.key_path.generic_u8string() };
    CHECK(result.request.arguments == expected);
    for (const std::u8string& argument : result.request.arguments)
        CHECK(argument != u8"-N");

    const process_environment_override* const askpass { find_override(result.request.environment_overrides, u8"SSH_ASKPASS") };
    REQUIRE(askpass != nullptr);
    CHECK(askpass->value == std::u8string { u8"C:/cyella/bin/cyella-askpass.exe" });
    const process_environment_override* const force { find_override(result.request.environment_overrides, u8"SSH_ASKPASS_REQUIRE") };
    REQUIRE(force != nullptr);
    CHECK(force->value == std::u8string { u8"force" });
    const process_environment_override* const pipe { find_override(result.request.environment_overrides, u8"CYELLA_ASKPASS_PIPE") };
    REQUIRE(pipe != nullptr);
    CHECK(pipe->value == std::u8string { u8"\\\\.\\pipe\\cyella-askpass-1-abc" });
    CHECK(validate_process_request(result.request).empty());
}

TEST_CASE("keygen request refuses unsafe or unusable targets", "[ssh][keygen]")
{
    const scratch_directory directory {};
    ssh_keygen_options options {};
    options.ssh_keygen_executable = u8"C:/Git/usr/bin/ssh-keygen.exe";
    options.key_path = directory.path() / "id_ed25519";
    options.askpass_helper = u8"C:/cyella/bin/cyella-askpass.exe";
    options.askpass = sample_session();

    // 이미 있는 파일은 ssh-keygen이 덮어쓰기를 askpass가 아닌 곳으로 묻는다.
    directory.write("id_ed25519", "existing");
    CHECK(make_ssh_keygen_request(options).error == ssh_keygen_request_error::key_exists);
    std::filesystem::remove(directory.path() / "id_ed25519");
    directory.write("id_ed25519.pub", "existing");
    CHECK(make_ssh_keygen_request(options).error == ssh_keygen_request_error::key_exists);
    std::filesystem::remove(directory.path() / "id_ed25519.pub");
    CHECK(make_ssh_keygen_request(options).error == ssh_keygen_request_error::none);

    ssh_keygen_options relative { options };
    relative.key_path = "id_ed25519";
    CHECK(make_ssh_keygen_request(relative).error == ssh_keygen_request_error::invalid_key_path);

    ssh_keygen_options dashed { options };
    dashed.key_path = directory.path() / "-oProxyCommand=evil";
    CHECK(make_ssh_keygen_request(dashed).error == ssh_keygen_request_error::invalid_key_path);

    ssh_keygen_options bad_comment { options };
    bad_comment.comment = u8"line\nbreak";
    CHECK(make_ssh_keygen_request(bad_comment).error == ssh_keygen_request_error::invalid_comment);

    ssh_keygen_options no_helper { options };
    no_helper.askpass_helper.clear();
    CHECK(make_ssh_keygen_request(no_helper).error == ssh_keygen_request_error::askpass_missing);
}

TEST_CASE("connection test request is ssh -T with host key confirmation", "[ssh][connection]")
{
    ssh_connection_test_options options {};
    options.ssh_executable = u8"C:/Git/usr/bin/ssh.exe";
    options.host = u8"github.com";
    options.working_directory = u8"C:/work";
    options.askpass_helper = u8"C:/cyella/bin/cyella-askpass.exe";
    options.askpass = sample_session();

    const std::optional<process_request> request { make_ssh_connection_test_request(options) };

    REQUIRE(request.has_value());
    const std::vector<std::u8string> expected { u8"-T", u8"-o", u8"StrictHostKeyChecking=ask", u8"git@github.com" };
    CHECK(request->arguments == expected);
    CHECK(validate_process_request(*request).empty());

    options.port = 2222;
    const std::optional<process_request> with_port { make_ssh_connection_test_request(options) };
    REQUIRE(with_port.has_value());
    const std::vector<std::u8string> expected_with_port { u8"-T", u8"-o", u8"StrictHostKeyChecking=ask", u8"-p", u8"2222", u8"git@github.com" };
    CHECK(with_port->arguments == expected_with_port);
}

TEST_CASE("connection test request rejects option injection", "[ssh][connection]")
{
    ssh_connection_test_options options {};
    options.ssh_executable = u8"C:/Git/usr/bin/ssh.exe";
    options.working_directory = u8"C:/work";
    options.askpass_helper = u8"C:/cyella/bin/cyella-askpass.exe";
    options.askpass = sample_session();

    for (const std::u8string_view host :
        { std::u8string_view { u8"-oProxyCommand=calc" }, std::u8string_view { u8"host name" }, std::u8string_view { u8"" }, std::u8string_view { u8"a;b" }, std::u8string_view { u8"host'quote" } })
    {
        options.host = host;
        CHECK_FALSE(make_ssh_connection_test_request(options).has_value());
    }
    options.host = u8"github.com";
    options.user_name = u8"-l root";
    CHECK_FALSE(make_ssh_connection_test_request(options).has_value());
    options.user_name = u8"git";
    options.askpass_helper.clear();
    CHECK_FALSE(make_ssh_connection_test_request(options).has_value());
}

TEST_CASE("connection test counts the github greeting with exit 1 as success", "[ssh][connection]")
{
    const ssh_connection_classification github { classify_ssh_connection_test(1, u8"", u8"Hi alice! You've successfully authenticated, but GitHub does not provide shell access.\n") };
    CHECK(github.result == ssh_connection_result::authenticated);
    CHECK(github.authenticated_user == u8"alice");

    const ssh_connection_classification gitlab { classify_ssh_connection_test(0, u8"Welcome to GitLab, @bob!\n", u8"") };
    CHECK(gitlab.result == ssh_connection_result::authenticated);
    CHECK(gitlab.authenticated_user == u8"bob");

    // 같은 종료 코드 1이라도 인사말이 없으면 성공이 아니다.
    CHECK(classify_ssh_connection_test(1, u8"", u8"something broke\n").result == ssh_connection_result::unknown);
}

TEST_CASE("connection test classifies the real failure texts", "[ssh][connection]")
{
    // 앞의 세 줄은 OpenSSH 10.2가 github.com과 존재하지 않는 호스트에 낸 실제 stderr다. 시간 초과는 OpenSSH의 문구다.
    CHECK(classify_ssh_connection_test(255, u8"", u8"git@github.com: Permission denied (publickey).\n").result == ssh_connection_result::permission_denied);
    CHECK(classify_ssh_connection_test(255, u8"", u8"Host key verification failed.\n").result == ssh_connection_result::host_key_rejected);
    CHECK(classify_ssh_connection_test(255, u8"", u8"ssh: Could not resolve hostname nonexistent.invalid: Name or service not known\n").result == ssh_connection_result::connection_failed);
    CHECK(classify_ssh_connection_test(255, u8"", u8"ssh: connect to host example.org port 22: Connection timed out\n").result == ssh_connection_result::connection_failed);

    // 호스트 키가 바뀐 경우는 askpass를 거치지 않고 ssh가 이 문구로 실패한다. 뒤의 "verification failed"보다 먼저 판정한다.
    const std::u8string_view changed = u8"@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"
                                       u8"@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\r\n"
                                       u8"@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"
                                       u8"IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\r\n"
                                       u8"The fingerprint for the ED25519 key sent by the remote host is\n"
                                       u8"SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU.\r\n"
                                       u8"Offending ED25519 key in kh2:1\r\n"
                                       u8"Host key for github.com has changed and you have requested strict checking.\r\n"
                                       u8"Host key verification failed.\r\n";
    CHECK(classify_ssh_connection_test(255, u8"", changed).result == ssh_connection_result::host_key_changed);
}

TEST_CASE("temporary known hosts options keep the user's file untouched", "[ssh][known-hosts]")
{
    const std::vector<std::u8string> arguments { make_temporary_known_hosts_ssh_arguments("C:\\Temp\\cyella kh") };
    const std::vector<std::u8string> expected { u8"-o", u8"UserKnownHostsFile=C:/Temp/cyella kh", u8"-o", u8"StrictHostKeyChecking=accept-new" };
    CHECK(arguments == expected);

    // git이 sh 규칙으로 읽으므로 공백이 있는 경로와 작은따옴표를 안전하게 감싼다.
    CHECK(make_temporary_known_hosts_ssh_command(u8"C:\\Program Files\\Git\\usr\\bin\\ssh.exe", "C:/Temp/cyella kh")
        == u8"'C:/Program Files/Git/usr/bin/ssh.exe' '-o' 'UserKnownHostsFile=C:/Temp/cyella kh' '-o' 'StrictHostKeyChecking=accept-new'");
    CHECK(make_temporary_known_hosts_ssh_command(u8"ssh", "C:/it's/kh").find(u8"'UserKnownHostsFile=C:/it'\\''s/kh'") != std::u8string::npos);
}
