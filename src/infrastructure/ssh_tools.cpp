#include "infrastructure/ssh_tools.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::size_t keygen_capture_bytes { 1024u * 1024u };
        constexpr std::size_t public_key_line_limit { 8192u };

        std::string_view as_narrow(const std::u8string_view text) noexcept
        {
            return std::string_view { reinterpret_cast<const char*>(text.data()), text.size() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { reinterpret_cast<const char8_t*>(text.data()), text.size() };
        }

        bool is_space(const char value) noexcept
        {
            return value == ' ' || value == '\t' || value == '\r' || value == '\n';
        }

        std::string_view trim(std::string_view text) noexcept
        {
            while (text.empty() == false && is_space(text.back()))
                text.remove_suffix(1);
            while (text.empty() == false && is_space(text.front()))
                text.remove_prefix(1);
            return text;
        }

        std::string lower_ascii(const std::string_view text)
        {
            std::string lowered { text };
            std::ranges::transform(lowered, lowered.begin(), [](const char value) { return static_cast<char>(std::tolower(static_cast<unsigned char>(value))); });
            return lowered;
        }

        bool contains(const std::string_view text, const std::string_view needle) noexcept
        {
            return text.find(needle) != std::string_view::npos;
        }

        process_environment_override set_variable(const std::u8string_view name, const std::u8string_view value)
        {
            return { std::u8string { name }, std::u8string { value } };
        }

        // ssh와 ssh-keygen이 askpass를 쓰게 하는 환경이다. `SSH_ASKPASS_REQUIRE=force`는 터미널이나
        // DISPLAY가 없어도 도우미를 부르게 한다.
        std::vector<process_environment_override> ssh_askpass_overrides(const std::u8string_view helper, const askpass_session& session)
        {
            std::vector<process_environment_override> overrides {};
            overrides.push_back(set_variable(u8"SSH_ASKPASS", helper));
            overrides.push_back(set_variable(u8"SSH_ASKPASS_REQUIRE", u8"force"));
            for (process_environment_override& entry : askpass_session_overrides(true, session))
                overrides.push_back(std::move(entry));
            return overrides;
        }

        process_request make_ssh_family_request(const std::u8string_view executable, const std::u8string_view working_directory)
        {
            process_request request {};
            request.executable = executable;
            request.working_directory = working_directory;
            request.maximum_captured_bytes_per_stream = keygen_capture_bytes;
            request.text_encoding = process_text_encoding::active_code_page_fallback;
            return request;
        }

        bool safe_token(const std::u8string_view value, const std::string_view extra_characters)
        {
            if (value.empty() || value.front() == u8'-')
                return false;
            for (const char8_t character : value)
            {
                const char narrow { static_cast<char>(character) };
                if (std::isalnum(static_cast<unsigned char>(narrow)) == 0 && extra_characters.find(narrow) == std::string_view::npos)
                    return false;
            }
            return true;
        }

        bool has_control_character(const std::u8string_view value) noexcept
        {
            return std::ranges::any_of(value, [](const char8_t character) { return character < 0x20u || character == 0x7fu; });
        }

        std::u8string quote_for_shell(const std::u8string_view value)
        {
            std::u8string quoted { u8"'" };
            for (const char8_t character : value)
                if (character == u8'\'')
                    quoted += u8"'\\''";
                else
                    quoted.push_back(character);
            quoted.push_back(u8'\'');
            return quoted;
        }

        std::u8string forward_slashes(std::u8string value)
        {
            std::ranges::replace(value, u8'\\', u8'/');
            return value;
        }
    } // namespace

    // ---- ssh-agent 서비스 ----

    ssh_agent_service_status classify_ssh_agent_service(const ssh_agent_service_snapshot& snapshot) noexcept
    {
        if (snapshot.installed == false)
            return ssh_agent_service_status::not_installed;
        if (snapshot.running)
            return ssh_agent_service_status::running;
        return snapshot.start_disabled ? ssh_agent_service_status::disabled : ssh_agent_service_status::stopped;
    }

    std::u8string_view ssh_agent_service_status_name(const ssh_agent_service_status status) noexcept
    {
        switch (status)
        {
        case ssh_agent_service_status::running:
            return u8"running";
        case ssh_agent_service_status::stopped:
            return u8"stopped";
        case ssh_agent_service_status::disabled:
            return u8"disabled";
        case ssh_agent_service_status::not_installed:
            return u8"not_installed";
        }
        return u8"not_installed";
    }

    // ---- ssh-add -l ----

    namespace {
        std::optional<ssh_agent_identity> parse_identity_line(std::string_view line)
        {
            line = trim(line);
            std::size_t digits { 0 };
            while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])) != 0)
                ++digits;
            if (digits == 0 || digits > 6 || digits >= line.size() || line[digits] != ' ')
                return std::nullopt;

            ssh_agent_identity identity {};
            identity.bits = std::stoi(std::string { line.substr(0, digits) });
            line = trim(line.substr(digits));

            const std::size_t fingerprint_end { line.find(' ') };
            const std::string_view fingerprint { line.substr(0, fingerprint_end) };
            if (fingerprint.empty() || fingerprint.find(':') == std::string_view::npos)
                return std::nullopt;
            identity.fingerprint = to_u8(fingerprint);
            line = fingerprint_end == std::string_view::npos ? std::string_view {} : trim(line.substr(fingerprint_end));

            // 끝의 `(ED25519)`가 종류다. 주석에도 괄호가 올 수 있어 마지막 묶음만 본다.
            if (line.empty() == false && line.back() == ')')
            {
                const std::size_t open { line.rfind('(') };
                if (open != std::string_view::npos)
                {
                    identity.key_type = to_u8(line.substr(open + 1, line.size() - open - 2));
                    line = trim(line.substr(0, open));
                }
            }
            identity.comment = to_u8(line);
            return identity;
        }
    } // namespace

    ssh_agent_list parse_ssh_add_list(const int exit_code, const std::u8string_view standard_output)
    {
        ssh_agent_list list {};
        if (exit_code == 1)
        {
            list.state = ssh_agent_list_state::no_identities;
            return list;
        }
        if (exit_code == 2)
        {
            list.state = ssh_agent_list_state::agent_unavailable;
            return list;
        }
        if (exit_code != 0)
            return list;

        const std::string_view text { as_narrow(standard_output) };
        std::size_t begin { 0 };
        while (begin < text.size())
        {
            std::size_t end { text.find('\n', begin) };
            if (end == std::string_view::npos)
                end = text.size();
            if (std::optional<ssh_agent_identity> identity { parse_identity_line(text.substr(begin, end - begin)) })
                list.identities.push_back(std::move(*identity));
            begin = end + 1;
        }
        list.state = list.identities.empty() ? ssh_agent_list_state::failed : ssh_agent_list_state::identities;
        return list;
    }

    process_request make_ssh_add_list_request(const std::u8string_view ssh_add_executable, const std::u8string_view working_directory)
    {
        process_request request { make_ssh_family_request(ssh_add_executable, working_directory) };
        request.arguments = { u8"-l" };
        return request;
    }

    // ---- 키 쌍 ----

    std::optional<ssh_key_pair> parse_public_key_line(const std::u8string_view line)
    {
        const std::string_view text { trim(as_narrow(line)) };
        const std::size_t type_end { text.find(' ') };
        if (type_end == std::string_view::npos)
            return std::nullopt;

        const std::string_view type { text.substr(0, type_end) };
        if (type.starts_with("ssh-") == false && type.starts_with("ecdsa-") == false && type.starts_with("sk-") == false)
            return std::nullopt;

        // 종류 다음이 base64 본문이고 그 뒤가 주석이다.
        const std::string_view rest { trim(text.substr(type_end)) };
        const std::size_t body_end { rest.find(' ') };
        ssh_key_pair pair {};
        pair.key_type = to_u8(type);
        pair.comment = body_end == std::string_view::npos ? std::u8string {} : to_u8(trim(rest.substr(body_end)));
        return pair;
    }

    std::vector<ssh_key_pair> list_ssh_key_pairs(const std::filesystem::path& ssh_directory)
    {
        std::vector<ssh_key_pair> pairs {};
        std::error_code error {};
        std::filesystem::directory_iterator iterator { ssh_directory, error };
        if (error)
            return pairs;

        for (const std::filesystem::directory_iterator end {}; iterator != end; iterator.increment(error))
        {
            if (error)
                break;
            const std::filesystem::path& private_path { iterator->path() };
            std::error_code status_error {};
            if (iterator->is_regular_file(status_error) == false || status_error || private_path.extension() == ".pub")
                continue;

            std::filesystem::path public_path { private_path };
            public_path += ".pub";
            if (std::filesystem::is_regular_file(public_path, status_error) == false || status_error)
                continue;

            ssh_key_pair pair {};
            std::ifstream stream { public_path, std::ios::binary };
            std::string first_line(public_key_line_limit, '\0');
            stream.getline(first_line.data(), static_cast<std::streamsize>(first_line.size()));
            // gcount는 버린 줄 끝 문자까지 센다. 첫 NUL 앞까지만 쓴다.
            if (const std::size_t terminator { first_line.find('\0') }; terminator != std::string::npos)
                first_line.resize(terminator);
            if (std::optional<ssh_key_pair> parsed { parse_public_key_line(to_u8(first_line)) })
                pair = std::move(*parsed);
            pair.name = private_path.filename().u8string();
            pair.private_key_path = private_path;
            pair.public_key_path = public_path;
            pairs.push_back(std::move(pair));
        }
        std::ranges::sort(pairs, [](const ssh_key_pair& left, const ssh_key_pair& right) { return left.name < right.name; });
        return pairs;
    }

    // ---- ssh-keygen ----

    ssh_keygen_request_result make_ssh_keygen_request(const ssh_keygen_options& options)
    {
        ssh_keygen_request_result result {};
        if (options.askpass_helper.empty())
        {
            result.error = ssh_keygen_request_error::askpass_missing;
            return result;
        }
        if (options.key_path.empty() || options.key_path.is_absolute() == false || options.key_path.has_filename() == false || options.key_path.filename().u8string().starts_with(u8"-")
            || has_control_character(options.key_path.u8string()))
        {
            result.error = ssh_keygen_request_error::invalid_key_path;
            return result;
        }
        if (has_control_character(options.comment))
        {
            result.error = ssh_keygen_request_error::invalid_comment;
            return result;
        }

        std::filesystem::path public_path { options.key_path };
        public_path += ".pub";
        std::error_code private_error {};
        std::error_code public_error {};
        if (std::filesystem::exists(options.key_path, private_error) || std::filesystem::exists(public_path, public_error))
        {
            result.error = ssh_keygen_request_error::key_exists;
            return result;
        }

        result.request = make_ssh_family_request(options.ssh_keygen_executable, options.key_path.parent_path().generic_u8string());
        result.request.arguments = { u8"-t", u8"ed25519", u8"-C", options.comment, u8"-f", options.key_path.generic_u8string() };
        result.request.environment_overrides = ssh_askpass_overrides(options.askpass_helper, options.askpass);
        return result;
    }

    process_request make_ssh_keygen_fingerprint_request(const std::u8string_view ssh_keygen_executable, const std::filesystem::path& public_key_path)
    {
        process_request request { make_ssh_family_request(ssh_keygen_executable, public_key_path.parent_path().generic_u8string()) };
        request.arguments = { u8"-l", u8"-f", public_key_path.generic_u8string() };
        request.timeout = std::chrono::seconds { 30 };
        return request;
    }

    // ---- 연결 시험 ----

    std::optional<process_request> make_ssh_connection_test_request(const ssh_connection_test_options& options)
    {
        if (options.askpass_helper.empty() || safe_token(options.host, ".-_:[]") == false || safe_token(options.user_name, ".-_") == false)
            return std::nullopt;

        process_request request { make_ssh_family_request(options.ssh_executable, options.working_directory) };
        request.arguments = { u8"-T", u8"-o", u8"StrictHostKeyChecking=ask" };
        if (options.port != 0)
        {
            request.arguments.push_back(u8"-p");
            const std::string port { std::to_string(options.port) };
            request.arguments.push_back(to_u8(port));
        }
        request.arguments.push_back(options.user_name + u8"@" + options.host);
        request.environment_overrides = ssh_askpass_overrides(options.askpass_helper, options.askpass);
        return request;
    }

    ssh_connection_classification classify_ssh_connection_test(const int exit_code, const std::u8string_view standard_output, const std::u8string_view standard_error)
    {
        ssh_connection_classification classification {};
        const std::string combined { std::string { as_narrow(standard_output) } + "\n" + std::string { as_narrow(standard_error) } };
        const std::string lowered { lower_ascii(combined) };

        // 인사말에서 계정을 읽는다. GitHub는 `Hi alice!`, GitLab은 `Welcome to GitLab, @alice!`다.
        const auto read_user = [&combined](const std::string_view prefix) -> std::u8string {
            const std::size_t start { combined.find(prefix) };
            if (start == std::string::npos)
                return {};
            const std::size_t name_start { start + prefix.size() };
            const std::size_t name_end { combined.find_first_of("!\r\n ", name_start) };
            return to_u8(std::string_view { combined }.substr(name_start, name_end == std::string::npos ? std::string::npos : name_end - name_start));
        };

        const bool greeted { contains(lowered, "successfully authenticated") || contains(lowered, "welcome to gitlab") || contains(lowered, "authenticated via") || contains(lowered, "logged in as") };
        if (exit_code == 0 || (exit_code == 1 && greeted))
        {
            classification.result = ssh_connection_result::authenticated;
            classification.authenticated_user = contains(lowered, "welcome to gitlab") ? read_user("@") : read_user("Hi ");
            return classification;
        }

        if (contains(lowered, "remote host identification has changed"))
            classification.result = ssh_connection_result::host_key_changed;
        else if (contains(lowered, "host key verification failed"))
            classification.result = ssh_connection_result::host_key_rejected;
        else if (contains(lowered, "permission denied"))
            classification.result = ssh_connection_result::permission_denied;
        else if (contains(lowered, "could not resolve hostname") || contains(lowered, "connection timed out") || contains(lowered, "connection refused") || contains(lowered, "no route to host")
            || contains(lowered, "network is unreachable") || contains(lowered, "connection reset") || contains(lowered, "operation timed out") || contains(lowered, "unable to connect"))
            classification.result = ssh_connection_result::connection_failed;
        return classification;
    }

    // ---- 이번만 연결 ----

    std::vector<std::u8string> make_temporary_known_hosts_ssh_arguments(const std::filesystem::path& known_hosts_path)
    {
        return { u8"-o", u8"UserKnownHostsFile=" + known_hosts_path.generic_u8string(), u8"-o", u8"StrictHostKeyChecking=accept-new" };
    }

    std::u8string make_temporary_known_hosts_ssh_command(const std::u8string_view ssh_executable, const std::filesystem::path& known_hosts_path)
    {
        std::u8string command { quote_for_shell(forward_slashes(std::u8string { ssh_executable })) };
        for (const std::u8string& argument : make_temporary_known_hosts_ssh_arguments(known_hosts_path))
        {
            command.push_back(u8' ');
            command += quote_for_shell(argument);
        }
        return command;
    }
} // namespace cyella
