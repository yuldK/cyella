#include "infrastructure/git_signing_parser.h"

#include "infrastructure/git_text.h"

#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace cyella {
    namespace {
        constexpr char unit_separator { '\x1F' };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        char ascii_lowercase(const char value) noexcept
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        bool equals_ignoring_case(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
                return false;
            for (std::size_t index { 0 }; index < left.size(); ++index)
                if (ascii_lowercase(left[index]) != ascii_lowercase(right[index]))
                    return false;
            return true;
        }

        config_origin origin_from_scope(const std::string_view scope) noexcept
        {
            if (scope == "system")
                return config_origin::system;
            if (scope == "global")
                return config_origin::global;
            if (scope == "local")
                return config_origin::local;
            if (scope == "worktree")
                return config_origin::worktree;
            if (scope == "command")
                return config_origin::command;
            return config_origin::unset;
        }

        bool find_signing_key(const std::string_view name, signing_key_id& key) noexcept
        {
            constexpr std::array keys {
                signing_key_id::commit_gpgsign,
                signing_key_id::tag_gpgsign,
                signing_key_id::gpg_format,
                signing_key_id::user_signingkey,
                signing_key_id::gpg_program,
                signing_key_id::gpg_ssh_program,
                signing_key_id::gpg_ssh_allowed_signers,
            };
            for (const signing_key_id candidate : keys)
                if (equals_ignoring_case(name, signing_key_name(candidate)))
                {
                    key = candidate;
                    return true;
                }
            return false;
        }

        // gpg의 콜론 출력은 `:`와 `\`를 `\x3a`, `\x5c`처럼 이스케이프한다.
        std::u8string unescape_colon_field(const std::string_view field)
        {
            std::string text {};
            text.reserve(field.size());
            for (std::size_t index { 0 }; index < field.size(); ++index)
            {
                if (field[index] == '\\' && index + 3 < field.size() && field[index + 1] == 'x')
                {
                    const auto high { field[index + 2] };
                    const auto low { field[index + 3] };
                    const auto digit = [](const char value) -> int {
                        if (value >= '0' && value <= '9')
                            return value - '0';
                        if (value >= 'a' && value <= 'f')
                            return value - 'a' + 10;
                        if (value >= 'A' && value <= 'F')
                            return value - 'A' + 10;
                        return -1;
                    };
                    if (digit(high) >= 0 && digit(low) >= 0)
                    {
                        text.push_back(static_cast<char>(digit(high) * 16 + digit(low)));
                        index += 3;
                        continue;
                    }
                }
                text.push_back(field[index]);
            }
            return to_u8(text);
        }

        std::vector<std::string_view> split_colons(const std::string_view line)
        {
            std::vector<std::string_view> fields {};
            std::string_view rest { line };
            for (;;)
            {
                std::string_view field {};
                const bool more { git_text::take_field(rest, ':', field) };
                fields.push_back(field);
                if (more == false)
                    break;
            }
            return fields;
        }

        std::string_view field_at(const std::vector<std::string_view>& fields, const std::size_t index) noexcept
        {
            return index < fields.size() ? fields[index] : std::string_view {};
        }

        // gpg 공개 키 알고리즘 번호와 곡선 이름·비트 수를 사람이 읽는 이름으로 바꾼다.
        std::string algorithm_name(const std::string_view algorithm, const std::string_view bits, const std::string_view curve)
        {
            if (curve.empty() == false)
                return std::string { curve };
            if (algorithm == "1" || algorithm == "2" || algorithm == "3")
                return "rsa" + std::string { bits };
            if (algorithm == "17")
                return "dsa" + std::string { bits };
            if (algorithm == "16" || algorithm == "20")
                return "elgamal" + std::string { bits };
            if (algorithm == "22")
                return "ed25519";
            return std::string { algorithm };
        }

        bool has_signing_capability(const std::string_view capabilities) noexcept
        {
            return capabilities.find('s') != std::string_view::npos || capabilities.find('S') != std::string_view::npos;
        }
    } // namespace

    std::vector<commit_signature> parse_git_signature_records(const std::string_view bytes)
    {
        std::vector<commit_signature> signatures {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view record {};
            git_text::take_field(rest, '\0', record);
            // `--format`은 항목 사이에 줄바꿈을 끼우지 않지만 앞의 줄바꿈은 흘려 보낸다.
            while (record.empty() == false && (record.front() == '\n' || record.front() == '\r'))
                record.remove_prefix(1);
            if (record.empty())
                continue;

            std::string_view sha {};
            std::string_view status {};
            std::string_view signer {};
            std::string_view key {};
            std::string_view fields { record };
            if (git_text::take_field(fields, unit_separator, sha) == false || git_text::take_field(fields, unit_separator, status) == false
                || git_text::take_field(fields, unit_separator, signer) == false)
                continue;
            key = fields;
            if (sha.empty() || status.size() != 1)
                continue;
            commit_signature signature {};
            signature.sha = std::string { sha };
            signature.status = signature_status_from_code(status.front());
            signature.signer = to_u8(signer);
            signature.key = to_u8(key);
            signatures.push_back(std::move(signature));
        }
        return signatures;
    }

    signing_config_layers parse_git_signing_config(const std::string_view bytes)
    {
        signing_config_layers layers {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view scope {};
            if (git_text::take_field(rest, '\0', scope) == false)
                break;
            std::string_view item {};
            git_text::take_field(rest, '\0', item);

            std::string_view key_text {};
            std::string_view value_text { item };
            const bool has_value { git_text::take_field(value_text, '\n', key_text) };

            signing_key_id key {};
            if (find_signing_key(key_text, key) == false)
                continue;
            signing_entry entry {};
            entry.value = std::string { value_text };
            entry.origin = origin_from_scope(scope);
            entry.valueless = has_value == false;
            if (entry.origin == config_origin::unset)
                continue;
            layers.effective.at(key) = entry;
            if (entry.origin == config_origin::local)
                layers.local.at(key) = entry;
            else if (entry.origin == config_origin::global)
                layers.global.at(key) = entry;
        }
        return layers;
    }

    std::vector<gpg_secret_key> parse_gpg_secret_keys(const std::string_view colon_listing, const std::int64_t now)
    {
        std::vector<gpg_secret_key> keys {};
        std::string_view rest { colon_listing };
        bool in_primary { false };
        // 주 키 다음의 `fpr` 한 줄이 주 키의 지문이고, 하위 키 다음의 `fpr`은 하위 키의 것이다.
        bool next_fingerprint_is_primary { false };
        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty())
                continue;
            const std::vector<std::string_view> fields { split_colons(line) };
            const std::string_view type { fields.front() };

            if (type == "sec")
            {
                gpg_secret_key key {};
                const std::string_view validity { field_at(fields, 1) };
                key.revoked = validity.find('r') != std::string_view::npos;
                key.expired = validity.find('e') != std::string_view::npos;
                key.disabled = validity.find('d') != std::string_view::npos;
                key.key_id = std::string { field_at(fields, 4) };
                key.created = git_text::parse_integer<std::int64_t>(field_at(fields, 5)).value_or(0);
                key.expires = git_text::parse_integer<std::int64_t>(field_at(fields, 6)).value_or(0);
                if (key.expires != 0 && key.expires <= now)
                    key.expired = true;
                key.algorithm = algorithm_name(field_at(fields, 3), field_at(fields, 2), field_at(fields, 16));
                key.can_sign = has_signing_capability(field_at(fields, 11));
                keys.push_back(std::move(key));
                in_primary = true;
                next_fingerprint_is_primary = true;
            }
            else if (type == "ssb")
            {
                if (in_primary && has_signing_capability(field_at(fields, 11)))
                    keys.back().can_sign = true;
                next_fingerprint_is_primary = false;
            }
            else if (type == "fpr")
            {
                if (in_primary && next_fingerprint_is_primary && keys.back().fingerprint.empty())
                    keys.back().fingerprint = std::string { field_at(fields, 9) };
            }
            else if (type == "uid")
            {
                if (in_primary)
                    keys.back().user_ids.push_back(unescape_colon_field(field_at(fields, 9)));
            }
        }
        return keys;
    }
} // namespace cyella
