#include "infrastructure/git_signing_commands.h"

#include "infrastructure/git_extension_arguments.h"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>

namespace cyella {
    using namespace git_extension;

    namespace {
        constexpr std::string_view config_regexp { "^(commit\\.gpgsign|tag\\.gpgsign|gpg\\.format|user\\.signingkey|gpg\\.program|gpg\\.ssh\\.program|gpg\\.ssh\\.allowedsignersfile)$" };

        void push_texts(git_arguments& arguments, const std::initializer_list<std::string_view> values)
        {
            for (const std::string_view value : values)
                push_text(arguments, value);
        }

        bool is_hex_digit(const char value) noexcept
        {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
        }

        std::string_view require_config_value(const std::string_view value)
        {
            if (value.find('\0') != std::string_view::npos || value.find('\n') != std::string_view::npos || value.find('\r') != std::string_view::npos)
                reject("config value", "value contains NUL or a line break");
            return value;
        }
    } // namespace

    std::string_view signing_config_regexp() noexcept
    {
        return config_regexp;
    }

    git_arguments make_git_verify_signatures_args(const bool blank_allowed_signers)
    {
        git_arguments arguments {};
        if (blank_allowed_signers)
            push_texts(arguments, { "-c", "gpg.ssh.allowedSignersFile=" });
        push_texts(arguments, { "log", "--no-walk=unsorted", "--stdin", "-z", "--format=%H%x1f%G?%x1f%GS%x1f%GK" });
        return arguments;
    }

    std::string make_git_verify_signatures_input(const std::span<const std::string> shas)
    {
        std::string input {};
        for (const std::string& sha : shas)
        {
            // 객체 이름은 16진수 40자(SHA-1) 또는 64자(SHA-256)다. 줄바꿈이나 `-`가 든 값은 stdin의 다른 줄이 되므로 거절한다.
            if ((sha.size() != 40 && sha.size() != 64) || std::ranges::all_of(sha, is_hex_digit) == false)
                reject("commit", "not a full object name");
            input.append(sha).push_back('\n');
        }
        return input;
    }

    git_arguments make_git_signing_config_args()
    {
        git_arguments arguments {};
        push_texts(arguments, { "config", "-z", "--show-scope", "--get-regexp" });
        push_text(arguments, config_regexp);
        return arguments;
    }

    git_arguments make_git_allowed_signers_path_args()
    {
        git_arguments arguments {};
        push_texts(arguments, { "config", "--type=path", "--get", "gpg.ssh.allowedsignersfile" });
        return arguments;
    }

    git_arguments make_git_signing_config_set_args(const signing_key_id key, const std::string_view value, const bool global)
    {
        git_arguments arguments {};
        push_texts(arguments, { "config", global ? "--global" : "--local" });
        push_text(arguments, signing_key_name(key));
        push_text(arguments, require_config_value(value));
        return arguments;
    }

    git_arguments make_git_signing_config_unset_args(const signing_key_id key, const bool global)
    {
        git_arguments arguments {};
        push_texts(arguments, { "config", global ? "--global" : "--local", "--unset" });
        push_text(arguments, signing_key_name(key));
        return arguments;
    }

    git_arguments make_gpg_list_secret_keys_args()
    {
        git_arguments arguments {};
        push_texts(arguments, { "--list-secret-keys", "--with-colons", "--batch", "--fixed-list-mode", "--with-fingerprint" });
        return arguments;
    }
} // namespace cyella
