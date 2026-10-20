#include "domain/signing_config.h"

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

    std::u8string_view config_origin_label(const config_origin origin) noexcept
    {
        switch (origin)
        {
        case config_origin::unset:
            return u8"설정 없음";
        case config_origin::system:
            return u8"시스템";
        case config_origin::global:
            return u8"전역";
        case config_origin::local:
            return u8"이 저장소";
        case config_origin::worktree:
            return u8"worktree";
        case config_origin::command:
            return u8"명령줄";
        }
        return u8"설정 없음";
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
