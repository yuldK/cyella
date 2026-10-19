#include "domain/credential_prompt.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace cyella {
    namespace {
        // 분류는 ASCII 문구만 다루므로 char 기반 view로 바꿔 쓴다. u8string과 byte 표현이 같다.
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

        std::string_view trim_trailing(std::string_view text) noexcept
        {
            while (text.empty() == false && is_space(text.back()))
                text.remove_suffix(1);
            return text;
        }

        std::string_view trim(std::string_view text) noexcept
        {
            text = trim_trailing(text);
            while (text.empty() == false && is_space(text.front()))
                text.remove_prefix(1);
            return text;
        }

        bool consume_prefix(std::string_view& text, const std::string_view prefix) noexcept
        {
            if (text.starts_with(prefix) == false)
                return false;
            text.remove_prefix(prefix.size());
            return true;
        }

        std::string_view last_line(const std::string_view text) noexcept
        {
            const std::size_t newline { text.rfind('\n') };
            std::string_view line { newline == std::string_view::npos ? text : text.substr(newline + 1) };
            return trim_trailing(line);
        }

        // 줄 단위로 훑으며 줄 끝의 `\r`을 무시한다. `\r\n` 줄 끝이어도 같은 결과를 낸다.
        template<typename visitor>
        void for_each_line(const std::string_view text, visitor&& visit)
        {
            std::size_t begin { 0 };
            while (begin <= text.size())
            {
                std::size_t end { text.find('\n', begin) };
                if (end == std::string_view::npos)
                    end = text.size();
                visit(trim_trailing(text.substr(begin, end - begin)));
                begin = end + 1;
            }
        }

        // `scheme://user:pw@host:port/path`에서 사용자 이름과 host를 꺼낸다. 비밀번호 부분은 버린다.
        void split_url(std::string_view url, std::string_view& user, std::string_view& host) noexcept
        {
            user = {};
            const std::size_t scheme_end { url.find("://") };
            if (scheme_end != std::string_view::npos)
                url.remove_prefix(scheme_end + 3);
            const std::size_t path_start { url.find('/') };
            std::string_view authority { path_start == std::string_view::npos ? url : url.substr(0, path_start) };
            const std::size_t at { authority.rfind('@') };
            if (at != std::string_view::npos)
            {
                const std::string_view user_info { authority.substr(0, at) };
                const std::size_t colon { user_info.find(':') };
                user = colon == std::string_view::npos ? user_info : user_info.substr(0, colon);
                authority.remove_prefix(at + 1);
            }
            host = authority;
        }

        // 앞뒤를 같은 따옴표로 감싼 값을 벗기고 text는 닫는 따옴표 뒤로 옮긴다.
        bool read_quoted(std::string_view& text, std::string_view& quoted) noexcept
        {
            if (text.empty() || (text.front() != '\'' && text.front() != '"'))
                return false;
            const char quote { text.front() };
            const std::size_t close { text.find(quote, 1) };
            if (close == std::string_view::npos)
                return false;
            quoted = text.substr(1, close - 1);
            text.remove_prefix(close + 1);
            return true;
        }

        bool classify_host_key(const std::string_view text, credential_prompt& prompt)
        {
            if (last_line(text).starts_with("Are you sure you want to continue connecting") == false)
                return false;

            prompt.kind = credential_prompt_kind::ssh_host_key_confirm;
            for_each_line(text, [&prompt](const std::string_view line) {
                constexpr std::string_view host_prefix { "The authenticity of host '" };
                constexpr std::string_view fingerprint_marker { " key fingerprint is" };
                if (line.starts_with(host_prefix))
                {
                    const std::string_view rest { line.substr(host_prefix.size()) };
                    const std::size_t close { rest.find('\'') };
                    if (close == std::string_view::npos)
                        return;
                    std::string_view target { rest.substr(0, close) };
                    // `github.com (140.82.112.3)`는 호스트 이름과 주소를 함께 적는다.
                    const std::size_t open { target.find(" (") };
                    if (open != std::string_view::npos && target.back() == ')')
                    {
                        prompt.address = to_u8(target.substr(open + 2, target.size() - open - 3));
                        target = target.substr(0, open);
                    }
                    prompt.host = to_u8(target);
                    return;
                }
                const std::size_t marker { line.find(fingerprint_marker) };
                if (marker == std::string_view::npos)
                    return;
                // Git for Windows의 ssh는 `is:`, 시스템 OpenSSH는 `is`다. 끝의 마침표는 문장 부호다.
                std::string_view rest { line.substr(marker + fingerprint_marker.size()) };
                if (rest.starts_with(':'))
                    rest.remove_prefix(1);
                rest = trim(rest);
                if (rest.empty() == false && rest.back() == '.')
                    rest.remove_suffix(1);
                prompt.key_type = to_u8(line.substr(0, marker));
                prompt.fingerprint = to_u8(rest);
            });
            return true;
        }

        bool classify_passphrase(std::string_view text, credential_prompt& prompt)
        {
            if (text.starts_with("Enter same passphrase again"))
            {
                prompt.kind = credential_prompt_kind::ssh_new_key_passphrase_confirm;
                return true;
            }
            // 새 키의 passphrase는 "(empty for no passphrase)"가 표지다. 파일 이름이 있을 수도 없을 수도 있다.
            if ((text.starts_with("Enter passphrase") || text.starts_with("Enter new passphrase")) && text.find("(empty for no passphrase)") != std::string_view::npos)
            {
                prompt.kind = credential_prompt_kind::ssh_new_key_passphrase;
                if (consume_prefix(text, "Enter passphrase for "))
                {
                    std::string_view path {};
                    if (read_quoted(text, path))
                        prompt.key_path = to_u8(path);
                }
                return true;
            }

            if (consume_prefix(text, "Enter passphrase") == false)
                return false;

            // 파일 이름이 없는 `Enter passphrase:`는 `ssh-keygen -y` 등이 쓴다.
            if (text == ":")
            {
                prompt.kind = credential_prompt_kind::ssh_key_passphrase;
                return true;
            }
            if (consume_prefix(text, " for ") == false)
                return false;
            // ssh는 `key '<경로>'`, ssh-keygen은 `"<경로>"`, ssh-add는 따옴표 없는 경로를 쓴다.
            consume_prefix(text, "key ");

            std::string_view path {};
            if (read_quoted(text, path))
            {
                if (text != ":")
                    return false;
            }
            else
            {
                // 여는 따옴표만 있고 닫는 따옴표가 없는 문구는 경로로 읽지 않는다.
                if (text.ends_with(':') == false || text.empty() || text.front() == '\'' || text.front() == '"')
                    return false;
                text.remove_suffix(1);
                constexpr std::string_view confirm_suffix { " (will confirm each use)" };
                if (text.ends_with(confirm_suffix))
                    text.remove_suffix(confirm_suffix.size());
                path = text;
            }
            prompt.kind = credential_prompt_kind::ssh_key_passphrase;
            prompt.key_path = to_u8(path);
            return true;
        }

        bool classify_https(std::string_view text, credential_prompt& prompt)
        {
            credential_prompt_kind kind { credential_prompt_kind::unknown };
            if (consume_prefix(text, "Username for '"))
                kind = credential_prompt_kind::https_username;
            else if (consume_prefix(text, "Password for '"))
                kind = credential_prompt_kind::https_password;
            else
                return false;

            if (text.ends_with("':") == false)
                return false;
            text.remove_suffix(2);

            std::string_view user {};
            std::string_view host {};
            split_url(text, user, host);
            prompt.kind = kind;
            prompt.url = to_u8(text);
            prompt.user_name = to_u8(user);
            prompt.host = to_u8(host);
            return true;
        }

        bool classify_ssh_password(const std::string_view text, credential_prompt& prompt)
        {
            constexpr std::string_view suffix { "'s password:" };
            if (text.ends_with(suffix) == false)
                return false;
            const std::string_view target { text.substr(0, text.size() - suffix.size()) };
            if (target.empty() || target.find_first_of(" \t\r\n") != std::string_view::npos)
                return false;

            const std::size_t at { target.rfind('@') };
            prompt.kind = credential_prompt_kind::ssh_password;
            prompt.user_name = at == std::string_view::npos ? std::u8string {} : to_u8(target.substr(0, at));
            prompt.host = to_u8(at == std::string_view::npos ? target : target.substr(at + 1));
            return true;
        }
    } // namespace

    credential_prompt classify_credential_prompt(const std::u8string_view text)
    {
        credential_prompt prompt {};
        prompt.raw_text = std::u8string { text };

        const std::string_view trimmed { trim_trailing(as_narrow(text)) };
        const std::string_view line { last_line(trimmed) };
        // 문구 접두어가 서로 겹치지 않아 분류기 순서는 결과를 바꾸지 않는다.
        if (classify_host_key(trimmed, prompt) || classify_passphrase(line, prompt) || classify_https(line, prompt) || classify_ssh_password(line, prompt))
            return prompt;

        prompt.kind = credential_prompt_kind::unknown;
        return prompt;
    }

    bool credential_prompt_expects_secret(const credential_prompt_kind kind) noexcept
    {
        switch (kind)
        {
        case credential_prompt_kind::ssh_key_passphrase:
        case credential_prompt_kind::ssh_new_key_passphrase:
        case credential_prompt_kind::ssh_new_key_passphrase_confirm:
        case credential_prompt_kind::ssh_password:
        case credential_prompt_kind::https_password:
            return true;
        case credential_prompt_kind::unknown:
        case credential_prompt_kind::ssh_host_key_confirm:
        case credential_prompt_kind::https_username:
            return false;
        }
        return false;
    }

    bool credential_prompt_expects_yes_no(const credential_prompt_kind kind) noexcept
    {
        return kind == credential_prompt_kind::ssh_host_key_confirm;
    }

    std::u8string_view credential_prompt_kind_name(const credential_prompt_kind kind) noexcept
    {
        switch (kind)
        {
        case credential_prompt_kind::unknown:
            return u8"unknown";
        case credential_prompt_kind::ssh_key_passphrase:
            return u8"ssh_key_passphrase";
        case credential_prompt_kind::ssh_new_key_passphrase:
            return u8"ssh_new_key_passphrase";
        case credential_prompt_kind::ssh_new_key_passphrase_confirm:
            return u8"ssh_new_key_passphrase_confirm";
        case credential_prompt_kind::ssh_host_key_confirm:
            return u8"ssh_host_key_confirm";
        case credential_prompt_kind::ssh_password:
            return u8"ssh_password";
        case credential_prompt_kind::https_username:
            return u8"https_username";
        case credential_prompt_kind::https_password:
            return u8"https_password";
        }
        return u8"unknown";
    }
} // namespace cyella
