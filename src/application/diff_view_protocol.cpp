#include "application/diff_view_protocol.h"

#include "domain/text_catalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <initializer_list>

namespace cyella {
    namespace {
        using json = nlohmann::json;

        constexpr std::size_t maximum_hunk_index { 1'000'000'000 };

        // 올바른 UTF-8 한 글자의 바이트 수다. 올바르지 않으면 0이다.
        std::size_t utf8_sequence_length(const std::string_view bytes, const std::size_t offset) noexcept
        {
            const auto at = [&bytes](const std::size_t position) { return static_cast<unsigned char>(bytes[position]); };
            const unsigned char lead { at(offset) };
            if (lead < 0x80)
                return 1;
            const auto continuation
                = [&](const std::size_t position, const unsigned char low, const unsigned char high) { return position < bytes.size() && at(position) >= low && at(position) <= high; };
            if (lead >= 0xC2 && lead <= 0xDF)
                return continuation(offset + 1, 0x80, 0xBF) ? 2 : 0;
            if (lead >= 0xE0 && lead <= 0xEF)
            {
                // E0은 과잉 인코딩을, ED는 대리 쌍(U+D800~DFFF)을 막는다.
                const unsigned char low { lead == 0xE0 ? static_cast<unsigned char>(0xA0) : static_cast<unsigned char>(0x80) };
                const unsigned char high { lead == 0xED ? static_cast<unsigned char>(0x9F) : static_cast<unsigned char>(0xBF) };
                return continuation(offset + 1, low, high) && continuation(offset + 2, 0x80, 0xBF) ? 3 : 0;
            }
            if (lead >= 0xF0 && lead <= 0xF4)
            {
                // F0은 과잉 인코딩을, F4는 U+10FFFF 초과를 막는다.
                const unsigned char low { lead == 0xF0 ? static_cast<unsigned char>(0x90) : static_cast<unsigned char>(0x80) };
                const unsigned char high { lead == 0xF4 ? static_cast<unsigned char>(0x8F) : static_cast<unsigned char>(0xBF) };
                return continuation(offset + 1, low, high) && continuation(offset + 2, 0x80, 0xBF) && continuation(offset + 3, 0x80, 0xBF) ? 4 : 0;
            }
            return 0;
        }

        // 줄 하나다. 올바른 UTF-8이면 문자열 그대로, 아니면 base64와 표지 1을 덧붙인다.
        json line_entry(const diff_line& line)
        {
            char kind { 'c' };
            switch (line.kind)
            {
            case diff_line_kind::context:
                kind = 'c';
                break;
            case diff_line_kind::add:
                kind = 'a';
                break;
            case diff_line_kind::del:
                kind = 'd';
                break;
            case diff_line_kind::no_newline:
                kind = 'n';
                break;
            }
            json entry { json::array() };
            entry.push_back(std::string(1, kind));
            entry.push_back(line.old_line);
            entry.push_back(line.new_line);
            if (is_valid_utf8(line.content))
            {
                entry.push_back(line.content);
            }
            else
            {
                entry.push_back(base64_encode(line.content));
                entry.push_back(1);
            }
            return entry;
        }

        // 줄 수 상한 안에서 파일을 JSON으로 옮긴다. 상한에 닿으면 그 hunk의 앞부분까지만 싣고 멈춘다.
        json file_entry(const diff_file& file, const std::size_t line_limit, std::size_t& shown)
        {
            json hunks { json::array() };
            shown = 0;
            for (const diff_hunk& hunk : file.hunks)
            {
                if (line_limit != 0 && shown >= line_limit)
                    break;
                json lines { json::array() };
                for (const diff_line& line : hunk.lines)
                {
                    if (line_limit != 0 && shown >= line_limit)
                        break;
                    lines.push_back(line_entry(line));
                    ++shown;
                }
                json entry {};
                entry["header"] = to_valid_utf8(hunk.header);
                entry["old_start"] = hunk.old_start;
                entry["old_count"] = hunk.old_count;
                entry["new_start"] = hunk.new_start;
                entry["new_count"] = hunk.new_count;
                entry["section"] = to_valid_utf8(hunk.section);
                entry["lines"] = std::move(lines);
                hunks.push_back(std::move(entry));
            }

            json result {};
            result["old_path"] = to_valid_utf8(file.old_path);
            result["new_path"] = to_valid_utf8(file.new_path);
            result["status"] = std::string(1, file.status);
            result["similarity"] = file.similarity;
            result["old_mode"] = to_valid_utf8(file.old_mode);
            result["new_mode"] = to_valid_utf8(file.new_mode);
            result["lines_added"] = file.lines_added;
            result["lines_removed"] = file.lines_removed;
            result["hunks"] = std::move(hunks);
            return result;
        }

        bool is_safe_token_name(const std::string_view name) noexcept
        {
            if (name.size() < 3 || name.size() > 64 || name.starts_with("--") == false)
                return false;
            return std::ranges::all_of(name.substr(2), [](const char letter) { return (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '-'; });
        }

        // CSS 값에 들어갈 수 있는 짧은 색 표기만 받는다. 선언을 끝내거나 블록을 여는 글자는 막는다.
        bool is_safe_token_value(const std::string_view value) noexcept
        {
            if (value.empty() || value.size() > 128)
                return false;
            return std::ranges::all_of(value, [](const char letter) {
                return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') || (letter >= '0' && letter <= '9') || letter == '#' || letter == '(' || letter == ')' || letter == ','
                    || letter == '.' || letter == ' ' || letter == '%' || letter == '-';
            });
        }

        json theme_entry(const diff_theme& theme)
        {
            json tokens { json::object() };
            for (const auto& [name, value] : theme.tokens)
                if (is_safe_token_name(name) && is_safe_token_value(value))
                    tokens[name] = value;
            json result {};
            result["mode"] = theme.mode == diff_theme_mode::light ? "light" : "dark";
            result["tokens"] = std::move(tokens);
            return result;
        }

        // 내용 메시지가 공통으로 싣는 부분이다.
        void add_state(json& message, const diff_view_state& state)
        {
            message["layout"] = std::string { diff_layout_name(state.layout) };
            message["image_mode"] = std::string { image_compare_mode_name(state.image_mode) };
            message["theme"] = theme_entry(state.theme);
            message["toolbar"] = state.toolbar;
            message["strings"] = state.strings;
            message["lang"] = state.language;
        }

        json optional_size(const std::optional<std::uint64_t>& size)
        {
            return size.has_value() ? json(*size) : json(nullptr);
        }

        std::string dump(const json& message)
        {
            // 값은 모두 올바른 UTF-8로 만들어 넣으므로 replace는 만일의 안전망이다. 던지는 쪽보다 낫다.
            return message.dump(-1, ' ', false, json::error_handler_t::replace);
        }

        // 정확히 이 키들만 가진 객체인가.
        bool has_exactly(const json& object, const std::initializer_list<std::string_view> keys)
        {
            if (object.size() != keys.size())
                return false;
            return std::ranges::all_of(keys, [&object](const std::string_view key) { return object.contains(std::string { key }); });
        }

        std::optional<std::uint64_t> read_unsigned(const json& object, const char* key)
        {
            const auto found { object.find(key) };
            if (found == object.end() || found->is_number_unsigned() == false)
                return std::nullopt;
            return found->get<std::uint64_t>();
        }

        std::optional<std::size_t> read_index(const json& object, const char* key)
        {
            const std::optional<std::uint64_t> value { read_unsigned(object, key) };
            if (value.has_value() == false || *value > maximum_hunk_index)
                return std::nullopt;
            return static_cast<std::size_t>(*value);
        }

        std::optional<std::string> read_text(const json& object, const char* key)
        {
            const auto found { object.find(key) };
            if (found == object.end() || found->is_string() == false)
                return std::nullopt;
            return found->get<std::string>();
        }

        diff_action_parse_result reject(std::string error)
        {
            return { std::nullopt, std::move(error) };
        }

        diff_action_parse_result accept(diff_view_action action)
        {
            return { std::move(action), {} };
        }

        std::optional<diff_hunk_command> command_of(const std::string_view type) noexcept
        {
            if (type.starts_with("stage_"))
                return diff_hunk_command::stage;
            if (type.starts_with("unstage_"))
                return diff_hunk_command::unstage;
            if (type.starts_with("discard_"))
                return diff_hunk_command::discard;
            return std::nullopt;
        }
    } // namespace

    std::map<std::string, std::string> diff_viewer_strings()
    {
        std::map<std::string, std::string> result {};
        for (const auto& [key, pattern] : text_patterns_with_prefix(u8"diff_viewer."))
            result.emplace(std::string { key.begin(), key.end() }, std::string { pattern.begin(), pattern.end() });
        return result;
    }

    std::string diff_viewer_language()
    {
        return active_ui_language() == ui_language::english ? "en" : "ko";
    }

    bool is_valid_utf8(const std::string_view bytes) noexcept
    {
        std::size_t offset { 0 };
        while (offset < bytes.size())
        {
            const std::size_t length { utf8_sequence_length(bytes, offset) };
            if (length == 0)
                return false;
            offset += length;
        }
        return true;
    }

    std::string to_valid_utf8(const std::string_view bytes)
    {
        std::string result {};
        result.reserve(bytes.size());
        std::size_t offset { 0 };
        while (offset < bytes.size())
        {
            const std::size_t length { utf8_sequence_length(bytes, offset) };
            if (length == 0)
            {
                result += "\xEF\xBF\xBD";
                ++offset;
                continue;
            }
            result.append(bytes.substr(offset, length));
            offset += length;
        }
        return result;
    }

    std::string base64_encode(const std::string_view bytes)
    {
        constexpr std::string_view alphabet { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };
        std::string encoded {};
        encoded.reserve((bytes.size() + 2) / 3 * 4);
        std::size_t offset { 0 };
        while (offset + 3 <= bytes.size())
        {
            const std::uint32_t group { (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8)
                | static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) };
            encoded.push_back(alphabet[(group >> 18) & 0x3F]);
            encoded.push_back(alphabet[(group >> 12) & 0x3F]);
            encoded.push_back(alphabet[(group >> 6) & 0x3F]);
            encoded.push_back(alphabet[group & 0x3F]);
            offset += 3;
        }
        const std::size_t rest { bytes.size() - offset };
        if (rest == 1)
        {
            const std::uint32_t group { static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 16 };
            encoded.push_back(alphabet[(group >> 18) & 0x3F]);
            encoded.push_back(alphabet[(group >> 12) & 0x3F]);
            encoded += "==";
        }
        else if (rest == 2)
        {
            const std::uint32_t group { (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 16)
                | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) };
            encoded.push_back(alphabet[(group >> 18) & 0x3F]);
            encoded.push_back(alphabet[(group >> 12) & 0x3F]);
            encoded.push_back(alphabet[(group >> 6) & 0x3F]);
            encoded.push_back('=');
        }
        return encoded;
    }

    std::string make_file_url(const std::u8string_view absolute_path)
    {
        if (absolute_path.empty())
            return {};

        std::string normalized {};
        normalized.reserve(absolute_path.size());
        for (const char8_t letter : absolute_path)
            normalized.push_back(letter == u8'\\' ? '/' : static_cast<char>(letter));

        // 드라이브 경로(`C:/dir`)는 `file:///C:/dir`, 서버 경로(`//server/share`)는 `file://server/share`다.
        std::string url { "file://" };
        if (normalized.starts_with("//"))
            normalized.erase(0, 2);
        else if (normalized.starts_with('/') == false)
            url.push_back('/');

        constexpr std::string_view digits { "0123456789ABCDEF" };
        for (const char letter : normalized)
        {
            const auto code { static_cast<unsigned char>(letter) };
            const bool plain { (letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') || letter == '-' || letter == '_' || letter == '.'
                || letter == '~' || letter == '/' || letter == ':' };
            if (plain)
            {
                url.push_back(letter);
                continue;
            }
            url.push_back('%');
            url.push_back(digits[code >> 4]);
            url.push_back(digits[code & 0x0F]);
        }
        return url;
    }

    std::string_view diff_layout_name(const diff_layout layout) noexcept
    {
        return layout == diff_layout::split ? "split" : "inline";
    }

    std::string_view image_compare_mode_name(const image_compare_mode mode) noexcept
    {
        switch (mode)
        {
        case image_compare_mode::side_by_side:
            return "side_by_side";
        case image_compare_mode::onion_skin:
            return "onion";
        case image_compare_mode::swipe:
            return "swipe";
        }
        return "side_by_side";
    }

    std::string_view diff_view_name(const diff_view view) noexcept
    {
        switch (view)
        {
        case diff_view::unstaged:
            return "unstaged";
        case diff_view::staged:
            return "staged";
        case diff_view::commit:
            return "commit";
        }
        return "unstaged";
    }

    std::size_t count_diff_lines(const diff_file& file) noexcept
    {
        std::size_t total { 0 };
        for (const diff_hunk& hunk : file.hunks)
            total += hunk.lines.size();
        return total;
    }

    std::string serialize_show_diff(const show_diff_message& message)
    {
        json result {};
        result["type"] = "show_diff";
        result["seq"] = message.seq;
        result["view"] = std::string { diff_view_name(message.view) };
        add_state(result, message.state);
        result["can_stage"] = message.can_stage;
        result["can_unstage"] = message.can_unstage;
        result["can_discard"] = message.can_discard;

        const diff_file empty {};
        const diff_file& file { message.file != nullptr ? *message.file : empty };
        const std::size_t total { count_diff_lines(file) };
        std::size_t shown { 0 };
        result["file"] = file_entry(file, message.line_limit, shown);
        result["truncated"] = shown < total;
        result["total_lines"] = total;
        result["shown_lines"] = shown;
        return dump(result);
    }

    std::string serialize_show_image(const show_image_message& message)
    {
        json result {};
        result["type"] = "show_image";
        result["seq"] = message.seq;
        add_state(result, message.state);
        result["path"] = to_valid_utf8(message.path);
        result["status"] = std::string(1, message.status);
        result["left_url"] = message.left_url.empty() ? json(nullptr) : json(message.left_url);
        result["right_url"] = message.right_url.empty() ? json(nullptr) : json(message.right_url);
        result["left_size"] = optional_size(message.left_size);
        result["right_size"] = optional_size(message.right_size);
        result["can_open_external"] = message.can_open_external;
        return dump(result);
    }

    std::string serialize_show_binary(const show_binary_message& message)
    {
        json result {};
        result["type"] = "show_binary";
        result["seq"] = message.seq;
        add_state(result, message.state);
        result["path"] = to_valid_utf8(message.path);
        result["status"] = std::string(1, message.status);
        result["old_size"] = optional_size(message.old_size);
        result["new_size"] = optional_size(message.new_size);
        result["can_open_external"] = message.can_open_external;
        return dump(result);
    }

    std::string serialize_clear(const std::uint64_t seq, const diff_view_state& state)
    {
        json result {};
        result["type"] = "clear";
        result["seq"] = seq;
        add_state(result, state);
        return dump(result);
    }

    std::string serialize_set_layout(const std::uint64_t seq, const diff_layout layout)
    {
        json result {};
        result["type"] = "set_layout";
        result["seq"] = seq;
        result["layout"] = std::string { diff_layout_name(layout) };
        return dump(result);
    }

    std::string serialize_set_theme(const std::uint64_t seq, const diff_theme& theme)
    {
        json result {};
        result["type"] = "set_theme";
        result["seq"] = seq;
        result["theme"] = theme_entry(theme);
        return dump(result);
    }

    std::string serialize_set_image_mode(const std::uint64_t seq, const image_compare_mode mode)
    {
        json result {};
        result["type"] = "set_image_mode";
        result["seq"] = seq;
        result["mode"] = std::string { image_compare_mode_name(mode) };
        return dump(result);
    }

    std::optional<std::vector<std::size_t>> expand_line_ranges(const diff_hunk& hunk, const std::vector<diff_line_range>& ranges)
    {
        std::vector<bool> chosen(hunk.lines.size(), false);
        for (const diff_line_range& range : ranges)
        {
            if (range.first > range.last || range.last >= hunk.lines.size())
                return std::nullopt;
            for (std::size_t index { range.first }; index <= range.last; ++index)
                if (hunk.lines[index].kind == diff_line_kind::add || hunk.lines[index].kind == diff_line_kind::del)
                    chosen[index] = true;
        }
        std::vector<std::size_t> indices {};
        for (std::size_t index { 0 }; index < chosen.size(); ++index)
            if (chosen[index])
                indices.push_back(index);
        return indices;
    }

    diff_action_parse_result parse_diff_view_action(const std::string_view text)
    {
        const json root { json::parse(text.begin(), text.end(), nullptr, false) };
        if (root.is_discarded())
            return reject("not valid JSON");
        if (root.is_object() == false)
            return reject("not an object");
        const std::optional<std::string> type { read_text(root, "type") };
        if (type.has_value() == false)
            return reject("missing type");

        if (*type == "ready")
        {
            if (has_exactly(root, { "type", "protocol" }) == false)
                return reject("ready: unexpected fields");
            const std::optional<std::uint64_t> protocol { read_unsigned(root, "protocol") };
            if (protocol.has_value() == false || *protocol > 1000)
                return reject("ready: bad protocol");
            return accept(ready_action { static_cast<int>(*protocol) });
        }

        if (*type == "stage_hunk" || *type == "unstage_hunk" || *type == "discard_hunk")
        {
            if (has_exactly(root, { "type", "seq", "hunk" }) == false)
                return reject(*type + ": unexpected fields");
            const std::optional<std::uint64_t> seq { read_unsigned(root, "seq") };
            const std::optional<std::size_t> hunk { read_index(root, "hunk") };
            if (seq.has_value() == false || hunk.has_value() == false)
                return reject(*type + ": bad seq or hunk");
            return accept(hunk_action { *command_of(*type), *seq, *hunk });
        }

        if (*type == "stage_lines" || *type == "unstage_lines" || *type == "discard_lines")
        {
            if (has_exactly(root, { "type", "seq", "hunk", "ranges" }) == false)
                return reject(*type + ": unexpected fields");
            const std::optional<std::uint64_t> seq { read_unsigned(root, "seq") };
            const std::optional<std::size_t> hunk { read_index(root, "hunk") };
            if (seq.has_value() == false || hunk.has_value() == false)
                return reject(*type + ": bad seq or hunk");
            const json& ranges_value { root.at("ranges") };
            if (ranges_value.is_array() == false || ranges_value.empty() || ranges_value.size() > diff_view_maximum_ranges)
                return reject(*type + ": bad ranges");
            std::vector<diff_line_range> ranges {};
            ranges.reserve(ranges_value.size());
            for (const json& pair : ranges_value)
            {
                if (pair.is_array() == false || pair.size() != 2 || pair[0].is_number_unsigned() == false || pair[1].is_number_unsigned() == false)
                    return reject(*type + ": bad range");
                const std::uint64_t first { pair[0].get<std::uint64_t>() };
                const std::uint64_t last { pair[1].get<std::uint64_t>() };
                if (first > last || last > maximum_hunk_index)
                    return reject(*type + ": bad range");
                ranges.push_back({ static_cast<std::size_t>(first), static_cast<std::size_t>(last) });
            }
            return accept(lines_action { *command_of(*type), *seq, *hunk, std::move(ranges) });
        }

        if (*type == "open_external" || *type == "show_full")
        {
            if (has_exactly(root, { "type", "seq" }) == false)
                return reject(*type + ": unexpected fields");
            const std::optional<std::uint64_t> seq { read_unsigned(root, "seq") };
            if (seq.has_value() == false)
                return reject(*type + ": bad seq");
            if (*type == "open_external")
                return accept(open_external_action { *seq });
            return accept(show_full_action { *seq });
        }

        if (*type == "copy")
        {
            if (has_exactly(root, { "type", "seq", "text" }) == false)
                return reject("copy: unexpected fields");
            const std::optional<std::uint64_t> seq { read_unsigned(root, "seq") };
            std::optional<std::string> copied { read_text(root, "text") };
            if (seq.has_value() == false || copied.has_value() == false)
                return reject("copy: bad seq or text");
            if (copied->empty() || copied->size() > diff_view_maximum_copy_bytes)
                return reject("copy: bad text size");
            return accept(copy_action { *seq, std::move(*copied) });
        }

        if (*type == "layout_changed")
        {
            if (has_exactly(root, { "type", "layout" }) == false)
                return reject("layout_changed: unexpected fields");
            const std::optional<std::string> layout { read_text(root, "layout") };
            if (layout == "inline")
                return accept(layout_changed_action { diff_layout::inline_view });
            if (layout == "split")
                return accept(layout_changed_action { diff_layout::split });
            return reject("layout_changed: bad layout");
        }

        if (*type == "image_mode_changed")
        {
            if (has_exactly(root, { "type", "mode" }) == false)
                return reject("image_mode_changed: unexpected fields");
            const std::optional<std::string> mode { read_text(root, "mode") };
            if (mode == "side_by_side")
                return accept(image_mode_changed_action { image_compare_mode::side_by_side });
            if (mode == "onion")
                return accept(image_mode_changed_action { image_compare_mode::onion_skin });
            if (mode == "swipe")
                return accept(image_mode_changed_action { image_compare_mode::swipe });
            return reject("image_mode_changed: bad mode");
        }

        return reject("unknown type");
    }
} // namespace cyella
