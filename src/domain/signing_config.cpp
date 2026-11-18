#include "domain/signing_config.h"

#include "domain/text_catalog.h"

namespace cyella {
    namespace {
        char ascii_lowercase(const char value) noexcept
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        bool equals_ignoring_case(const std::string_view text, const std::string_view word) noexcept
        {
            if (text.size() != word.size())
                return false;
            for (std::size_t index { 0 }; index < text.size(); ++index)
                if (ascii_lowercase(text[index]) != word[index])
                    return false;
            return true;
        }

        int digit_value(const char value) noexcept
        {
            if (value >= '0' && value <= '9')
                return value - '0';
            if (value >= 'a' && value <= 'f')
                return value - 'a' + 10;
            if (value >= 'A' && value <= 'F')
                return value - 'A' + 10;
            return -1;
        }

        // git의 `git_parse_int`가 받는 정수 꼴인지 본다. 앞 공백, 부호, 10·8·16진수, 단위 k·m·g(1024배)를 받고 int
        // 범위를 넘거나 다른 글자가 붙으면 실패한다. 불리언 설정은 0인지 아닌지만 쓰므로 0인지만 돌려준다.
        bool parse_config_integer(const std::string_view text, bool& is_zero) noexcept
        {
            std::size_t index { 0 };
            while (index < text.size() && (text[index] == ' ' || (text[index] >= '\t' && text[index] <= '\r')))
                ++index;
            if (index < text.size() && (text[index] == '+' || text[index] == '-'))
                ++index;

            // strtoimax의 기수 0과 같다. `0x` 뒤에 16진 숫자가 있으면 16진수, `0`으로 시작하면 8진수다.
            int base { 10 };
            if (index < text.size() && text[index] == '0')
            {
                base = 8;
                if (index + 2 < text.size() && (text[index + 1] == 'x' || text[index + 1] == 'X') && digit_value(text[index + 2]) >= 0)
                {
                    base = 16;
                    index += 2;
                }
            }

            constexpr std::uint64_t limit { 0x7FFFFFFF };
            std::uint64_t magnitude { 0 };
            bool has_digit { false };
            while (index < text.size())
            {
                const int digit { digit_value(text[index]) };
                if (digit < 0 || digit >= base)
                    break;
                // 한도를 넘으면 더 키우지 않는다. 아래 범위 검사에서 걸러지고 64비트도 넘치지 않는다.
                if (magnitude <= limit)
                    magnitude = magnitude * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
                has_digit = true;
                ++index;
            }
            if (has_digit == false)
                return false;

            std::uint64_t factor { 1 };
            if (index < text.size())
            {
                if (index + 1 != text.size())
                    return false;
                switch (ascii_lowercase(text[index]))
                {
                case 'k':
                    factor = 1024;
                    break;
                case 'm':
                    factor = 1024 * 1024;
                    break;
                case 'g':
                    factor = 1024 * 1024 * 1024;
                    break;
                default:
                    return false;
                }
            }
            if (magnitude > limit / factor)
                return false;
            is_zero = magnitude == 0;
            return true;
        }

        bool read_bool(const signing_entry& entry) noexcept
        {
            if (entry.is_set() == false)
                return false;
            // `[commit] gpgsign`처럼 `=`가 없는 불리언은 참이다.
            if (entry.valueless)
                return true;
            bool value { false };
            return parse_config_bool(entry.value, value) && value;
        }
    } // namespace

    std::string_view signing_key_name(const signing_key_id key) noexcept
    {
        switch (key)
        {
        case signing_key_id::commit_gpgsign:
            return "commit.gpgsign";
        case signing_key_id::tag_gpgsign:
            return "tag.gpgsign";
        case signing_key_id::gpg_format:
            return "gpg.format";
        case signing_key_id::user_signingkey:
            return "user.signingkey";
        case signing_key_id::gpg_program:
            return "gpg.program";
        case signing_key_id::gpg_ssh_program:
            return "gpg.ssh.program";
        case signing_key_id::gpg_ssh_allowed_signers:
            break;
        }
        return "gpg.ssh.allowedsignersfile";
    }

    std::u8string config_origin_label(const config_origin origin)
    {
        switch (origin)
        {
        case config_origin::unset:
            return tr(u8"signing_config.origin.unset");
        case config_origin::system:
            return tr(u8"signing_config.origin.system");
        case config_origin::global:
            return tr(u8"signing_config.origin.global");
        case config_origin::local:
            return tr(u8"signing_config.origin.local");
        case config_origin::worktree:
            return u8"worktree";
        case config_origin::command:
            return tr(u8"signing_config.origin.command");
        }
        return tr(u8"signing_config.origin.unset");
    }

    std::string_view signing_format_name(const signing_format format) noexcept
    {
        return format == signing_format::ssh ? "ssh" : "openpgp";
    }

    bool parse_signing_format(const std::string_view text, signing_format& format) noexcept
    {
        if (equals_ignoring_case(text, "openpgp"))
        {
            format = signing_format::openpgp;
            return true;
        }
        if (equals_ignoring_case(text, "ssh"))
        {
            format = signing_format::ssh;
            return true;
        }
        return false;
    }

    bool parse_config_bool(const std::string_view text, bool& value) noexcept
    {
        for (const std::string_view word : { std::string_view { "true" }, std::string_view { "yes" }, std::string_view { "on" }, std::string_view { "1" } })
            if (equals_ignoring_case(text, word))
            {
                value = true;
                return true;
            }
        for (const std::string_view word : { std::string_view { "false" }, std::string_view { "no" }, std::string_view { "off" }, std::string_view { "0" }, std::string_view {} })
            if (equals_ignoring_case(text, word))
            {
                value = false;
                return true;
            }
        // git은 불리언 자리에 정수도 받는다. 0이 아니면 참이다 (`commit.gpgsign=2`도 서명한다).
        bool is_zero { false };
        if (parse_config_integer(text, is_zero))
        {
            value = is_zero == false;
            return true;
        }
        return false;
    }

    bool signing_config::commit_signing() const noexcept
    {
        return read_bool(at(signing_key_id::commit_gpgsign));
    }

    bool signing_config::tag_signing() const noexcept
    {
        return read_bool(at(signing_key_id::tag_gpgsign));
    }

    signing_format signing_config::format() const noexcept
    {
        signing_format format { signing_format::openpgp };
        const signing_entry& entry { at(signing_key_id::gpg_format) };
        if (entry.is_set())
            static_cast<void>(parse_signing_format(entry.value, format));
        return format;
    }

    bool signing_config::has_signing_key() const noexcept
    {
        const signing_entry& entry { at(signing_key_id::user_signingkey) };
        return entry.is_set() && entry.value.empty() == false;
    }
} // namespace cyella
