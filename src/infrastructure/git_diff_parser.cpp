#include "infrastructure/git_diff_parser.h"

#include "infrastructure/git_text.h"

#include <cstddef>
#include <unordered_map>
#include <utility>

namespace cyella {
    namespace {
        bool is_octal_digit(const char value) noexcept
        {
            return value >= '0' && value <= '7';
        }

        // 따옴표 안쪽 내용의 이스케이프를 푼다.
        std::string unescape(const std::string_view content)
        {
            std::string result {};
            result.reserve(content.size());
            for (std::size_t index { 0 }; index < content.size(); ++index)
            {
                const char value { content[index] };
                if (value != '\\' || index + 1 >= content.size())
                {
                    result.push_back(value);
                    continue;
                }

                const char escaped { content[++index] };
                switch (escaped)
                {
                case 'a':
                    result.push_back('\a');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'v':
                    result.push_back('\v');
                    break;
                default:
                    if (is_octal_digit(escaped))
                    {
                        unsigned int byte { static_cast<unsigned int>(escaped - '0') };
                        for (int digits { 1 }; digits < 3 && index + 1 < content.size() && is_octal_digit(content[index + 1]); ++digits)
                            byte = byte * 8u + static_cast<unsigned int>(content[++index] - '0');
                        result.push_back(static_cast<char>(byte & 0xFFu));
                    }
                    else
                    {
                        // `\"`, `\\` 및 알 수 없는 이스케이프는 문자 자체다.
                        result.push_back(escaped);
                    }
                    break;
                }
            }
            return result;
        }

        // rest가 `"`로 시작하면 닫는 따옴표까지를 풀어 out에 담고 rest를 그 뒤로 옮긴다.
        bool take_quoted(std::string_view& rest, std::string& out)
        {
            if (rest.empty() || rest.front() != '"')
                return false;
            std::size_t index { 1 };
            while (index < rest.size() && rest[index] != '"')
                index += rest[index] == '\\' ? 2 : 1;
            if (index >= rest.size())
                return false;
            out = unescape(rest.substr(1, index - 1));
            rest.remove_prefix(index + 1);
            return true;
        }

        // `a/`나 `b/` 접두어를 뗀다. 접두어가 없으면 그대로 둔다.
        std::string strip_prefix(std::string path, const char prefix_letter)
        {
            if (path.size() >= 2 && path[0] == prefix_letter && path[1] == '/')
                path.erase(0, 2);
            return path;
        }

        // `diff --git a/old b/new`의 두 경로를 읽는다. 공백이 든 경로는 모호하지만,
        // 이름 변경이 아니면 두 경로가 같다는 점으로 나눈다. 이름 변경은 `rename from/to`
        // 줄이 정확한 값을 주므로 여기서는 best-effort로 충분하다.
        void parse_diff_git_header(std::string_view rest, diff_file& file)
        {
            std::string old_path {};
            std::string new_path {};

            if (rest.empty() == false && rest.front() == '"')
            {
                if (take_quoted(rest, old_path) == false)
                    return;
                if (rest.empty() == false && rest.front() == ' ')
                    rest.remove_prefix(1);
                if (rest.empty() == false && rest.front() == '"')
                {
                    if (take_quoted(rest, new_path) == false)
                        return;
                }
                else
                {
                    new_path = std::string { rest };
                }
            }
            else if (const std::size_t quoted { rest.find(" \"b/") }; quoted != std::string_view::npos)
            {
                old_path = std::string { rest.substr(0, quoted) };
                rest.remove_prefix(quoted + 1);
                if (take_quoted(rest, new_path) == false)
                    return;
            }
            else
            {
                // 두 경로가 같으면 길이가 2n + 5(`a/` n ` b/` n)다.
                const std::size_t size { rest.size() };
                std::size_t split { std::string_view::npos };
                if (size >= 5 && (size - 5) % 2 == 0)
                {
                    const std::size_t path_size { (size - 5) / 2 };
                    if (rest.substr(0, 2) == "a/" && rest.substr(2 + path_size, 3) == " b/" && rest.substr(2, path_size) == rest.substr(size - path_size))
                        split = 2 + path_size;
                }
                if (split == std::string_view::npos)
                    split = rest.find(" b/");
                if (split == std::string_view::npos)
                    return;
                old_path = std::string { rest.substr(0, split) };
                new_path = std::string { rest.substr(split + 1) };
            }

            file.old_path = strip_prefix(std::move(old_path), 'a');
            file.new_path = strip_prefix(std::move(new_path), 'b');
        }

        // `--- a/path`, `+++ b/path`의 경로 부분이다. 공백이 든 경로 뒤에는 TAB이 붙는다.
        std::string read_marker_path(std::string_view text, const char prefix_letter)
        {
            std::string path {};
            if (text.empty() == false && text.front() == '"')
            {
                std::string_view quoted { text };
                if (take_quoted(quoted, path) == false)
                    path = std::string { text };
            }
            else
            {
                const std::size_t tab { text.find('\t') };
                path = std::string { tab == std::string_view::npos ? text : text.substr(0, tab) };
            }
            if (path == "/dev/null")
                return {};
            return strip_prefix(std::move(path), prefix_letter);
        }

        // rename/copy from·to 줄의 경로에는 접두어가 없다.
        std::string read_plain_path(const std::string_view text)
        {
            if (text.empty() == false && text.front() == '"')
                return unquote_git_path(text);
            return std::string { text };
        }

        class line_reader
        {
        public:
            explicit line_reader(const std::string_view text) noexcept
                : text_ { text }
            {}

            [[nodiscard]] bool at_end() const noexcept
            {
                return position_ >= text_.size();
            }

            // 줄 끝 '\n'은 떼고 '\r'은 남긴다.
            bool next(std::string_view& line) noexcept
            {
                if (at_end())
                    return false;
                line = peek_line();
                position_ += line.size() + 1;
                return true;
            }

            bool peek(std::string_view& line) const noexcept
            {
                if (at_end())
                    return false;
                line = peek_line();
                return true;
            }

        private:
            [[nodiscard]] std::string_view peek_line() const noexcept
            {
                const std::size_t end { text_.find('\n', position_) };
                return text_.substr(position_, end == std::string_view::npos ? std::string_view::npos : end - position_);
            }

            std::string_view text_ {};
            std::size_t position_ { 0 };
        };

        // `@@ -a[,b] +c[,d] @@[ section]`
        bool parse_hunk_header(const std::string_view line, diff_hunk& hunk)
        {
            const auto read_range = [](std::string_view& rest, const char marker, std::int32_t& start, std::int32_t& count) noexcept {
                if (rest.empty() || rest.front() != marker)
                    return false;
                rest.remove_prefix(1);

                std::size_t digits { 0 };
                while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
                    ++digits;
                const auto start_value { git_text::parse_integer<std::int32_t>(rest.substr(0, digits)) };
                if (start_value.has_value() == false)
                    return false;
                start = *start_value;
                count = 1;
                rest.remove_prefix(digits);

                if (rest.empty() == false && rest.front() == ',')
                {
                    rest.remove_prefix(1);
                    digits = 0;
                    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
                        ++digits;
                    const auto count_value { git_text::parse_integer<std::int32_t>(rest.substr(0, digits)) };
                    if (count_value.has_value() == false)
                        return false;
                    count = *count_value;
                    rest.remove_prefix(digits);
                }
                return true;
            };

            std::string_view rest { line };
            if (git_text::starts_with(rest, "@@ ") == false)
                return false;
            rest.remove_prefix(3);
            if (read_range(rest, '-', hunk.old_start, hunk.old_count) == false)
                return false;
            if (rest.empty() || rest.front() != ' ')
                return false;
            rest.remove_prefix(1);
            if (read_range(rest, '+', hunk.new_start, hunk.new_count) == false)
                return false;
            if (git_text::starts_with(rest, " @@") == false)
                return false;
            rest.remove_prefix(3);

            hunk.header = std::string { line };
            if (rest.empty() == false && rest.front() == ' ')
                rest.remove_prefix(1);
            hunk.section = std::string { rest };
            return true;
        }

        // 역슬래시로 시작하는 "No newline at end of file" 줄에서 표지와 공백을 뗀 문구다.
        std::string_view no_newline_text(const std::string_view line) noexcept
        {
            return line.substr(line.size() < 2 ? line.size() : 2);
        }

        // 헤더 줄 다음에 오는 hunk 본문을 헤더의 개수만큼 읽는다.
        void read_hunk_body(line_reader& reader, diff_hunk& hunk)
        {
            std::int32_t old_remaining { hunk.old_count };
            std::int32_t new_remaining { hunk.new_count };
            std::int32_t old_line { hunk.old_start };
            std::int32_t new_line { hunk.new_start };

            std::string_view line {};
            while ((old_remaining > 0 || new_remaining > 0) && reader.peek(line))
            {
                diff_line entry {};
                // 일부 도구가 후행 공백을 지운 빈 context 줄도 받아 준다.
                const char marker { line.empty() ? ' ' : line.front() };
                if (marker == ' ' && old_remaining > 0 && new_remaining > 0)
                {
                    entry.kind = diff_line_kind::context;
                    entry.old_line = old_line++;
                    entry.new_line = new_line++;
                    --old_remaining;
                    --new_remaining;
                }
                else if (marker == '-' && old_remaining > 0)
                {
                    entry.kind = diff_line_kind::del;
                    entry.old_line = old_line++;
                    --old_remaining;
                }
                else if (marker == '+' && new_remaining > 0)
                {
                    entry.kind = diff_line_kind::add;
                    entry.new_line = new_line++;
                    --new_remaining;
                }
                else if (marker == '\\')
                {
                    entry.kind = diff_line_kind::no_newline;
                }
                else
                {
                    // 헤더 개수와 맞지 않는 줄이다. 다음 구역의 시작으로 보고 멈춘다.
                    break;
                }

                if (entry.kind == diff_line_kind::no_newline)
                    entry.content = std::string { no_newline_text(line) };
                else
                    entry.content = std::string { line.empty() ? line : line.substr(1) };
                hunk.lines.push_back(std::move(entry));
                reader.next(line);
            }

            // 개수를 다 채운 뒤에 마지막 줄을 꾸미는 `\ No newline` 표지가 올 수 있다.
            while (reader.peek(line) && line.empty() == false && line.front() == '\\')
            {
                diff_line entry {};
                entry.kind = diff_line_kind::no_newline;
                entry.content = std::string { no_newline_text(line) };
                hunk.lines.push_back(std::move(entry));
                reader.next(line);
            }
        }

        void finish_file(diff_file& file)
        {
            for (const diff_hunk& hunk : file.hunks)
                for (const diff_line& line : hunk.lines)
                    if (line.kind == diff_line_kind::add)
                        ++file.lines_added;
                    else if (line.kind == diff_line_kind::del)
                        ++file.lines_removed;
        }

        // 파일 머리말의 한 줄을 반영한다.
        void apply_header_line(const std::string_view line, diff_file& file)
        {
            if (git_text::starts_with(line, "old mode "))
            {
                file.old_mode = std::string { line.substr(9) };
            }
            else if (git_text::starts_with(line, "new mode "))
            {
                file.new_mode = std::string { line.substr(9) };
            }
            else if (git_text::starts_with(line, "deleted file mode "))
            {
                file.status = 'D';
                file.old_mode = std::string { line.substr(18) };
            }
            else if (git_text::starts_with(line, "new file mode "))
            {
                file.status = 'A';
                file.new_mode = std::string { line.substr(14) };
            }
            else if (git_text::starts_with(line, "similarity index "))
            {
                std::string_view value { line.substr(17) };
                if (value.empty() == false && value.back() == '%')
                    value.remove_suffix(1);
                file.similarity = git_text::parse_integer<std::int32_t>(value).value_or(0);
            }
            else if (git_text::starts_with(line, "rename from "))
            {
                file.status = 'R';
                file.old_path = read_plain_path(line.substr(12));
            }
            else if (git_text::starts_with(line, "rename to "))
            {
                file.status = 'R';
                file.new_path = read_plain_path(line.substr(10));
            }
            else if (git_text::starts_with(line, "copy from "))
            {
                file.status = 'C';
                file.old_path = read_plain_path(line.substr(10));
            }
            else if (git_text::starts_with(line, "copy to "))
            {
                file.status = 'C';
                file.new_path = read_plain_path(line.substr(8));
            }
            else if (git_text::starts_with(line, "index "))
            {
                // `index <old>..<new>[ <mode>]`
                std::string_view value { line.substr(6) };
                std::string_view blobs {};
                git_text::take_field(value, ' ', blobs);
                const std::size_t dots { blobs.find("..") };
                if (dots != std::string_view::npos)
                {
                    file.old_blob = std::string { blobs.substr(0, dots) };
                    file.new_blob = std::string { blobs.substr(dots + 2) };
                }
                if (value.empty() == false && file.old_mode.empty() && file.new_mode.empty())
                {
                    file.old_mode = std::string { value };
                    file.new_mode = std::string { value };
                }
            }
            else if (git_text::starts_with(line, "--- "))
            {
                file.old_path = read_marker_path(line.substr(4), 'a');
            }
            else if (git_text::starts_with(line, "+++ "))
            {
                file.new_path = read_marker_path(line.substr(4), 'b');
            }
            else if (git_text::starts_with(line, "Binary files ") || line == "GIT binary patch")
            {
                file.is_binary = true;
            }
        }
    } // namespace

    std::string unquote_git_path(const std::string_view text)
    {
        std::string_view rest { text };
        std::string result {};
        if (take_quoted(rest, result) == false)
            return std::string { text };
        return result;
    }

    std::vector<diff_file> parse_git_diff(const std::string_view text)
    {
        std::vector<diff_file> files {};
        line_reader reader { text };

        // 구역: 아직 파일 밖, 파일 머리말, 건너뛰는 본문(`diff --cc`)
        enum class section_state
        {
            outside,
            header,
            skipped,
        };
        section_state state { section_state::outside };

        const auto close_current = [&files]() {
            if (files.empty() == false)
                finish_file(files.back());
        };

        std::string_view line {};
        while (reader.next(line))
        {
            if (git_text::starts_with(line, "diff --git "))
            {
                close_current();
                files.emplace_back();
                parse_diff_git_header(line.substr(11), files.back());
                state = section_state::header;
                continue;
            }
            if (git_text::starts_with(line, "diff --cc ") || git_text::starts_with(line, "diff --combined "))
            {
                close_current();
                state = section_state::skipped;
                continue;
            }
            if (state != section_state::header)
                continue;

            diff_file& file { files.back() };
            if (git_text::starts_with(line, "@@ "))
            {
                diff_hunk hunk {};
                if (parse_hunk_header(line, hunk) == false)
                    continue;
                read_hunk_body(reader, hunk);
                file.hunks.push_back(std::move(hunk));
                continue;
            }
            apply_header_line(line, file);
            if (line == "GIT binary patch")
                state = section_state::skipped;
        }
        close_current();
        return files;
    }

    std::vector<numstat_entry> parse_git_numstat(const std::string_view bytes)
    {
        std::vector<numstat_entry> entries {};
        std::string_view rest { bytes };

        while (rest.empty() == false)
        {
            std::string_view record {};
            git_text::take_field(rest, '\0', record);
            while (record.empty() == false && (record.front() == '\n' || record.front() == '\r'))
                record.remove_prefix(1);
            if (record.empty())
                continue;

            std::string_view added {};
            std::string_view removed {};
            if (git_text::take_field(record, '\t', added) == false || git_text::take_field(record, '\t', removed) == false)
                continue;

            numstat_entry entry {};
            if (added == "-" && removed == "-")
            {
                entry.is_binary = true;
            }
            else
            {
                entry.lines_added = git_text::parse_integer<std::uint32_t>(added);
                entry.lines_removed = git_text::parse_integer<std::uint32_t>(removed);
                if (entry.lines_added.has_value() == false || entry.lines_removed.has_value() == false)
                    continue;
            }

            if (record.empty())
            {
                // 이름 변경·복사는 경로 자리가 비고 다음 두 레코드가 원래·새 경로다.
                std::string_view original {};
                std::string_view renamed {};
                if (rest.empty())
                    continue;
                git_text::take_field(rest, '\0', original);
                git_text::take_field(rest, '\0', renamed);
                entry.original_path = std::string { original };
                entry.path = std::string { renamed };
            }
            else
            {
                entry.path = std::string { record };
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    void apply_numstat(const std::span<file_change> changes, const std::span<const numstat_entry> entries)
    {
        std::unordered_map<std::string_view, const numstat_entry*> by_path {};
        by_path.reserve(entries.size());
        for (const numstat_entry& entry : entries)
            by_path.emplace(entry.path, &entry);

        for (file_change& change : changes)
        {
            const auto found { by_path.find(change.path) };
            if (found == by_path.end())
                continue;
            const numstat_entry& entry { *found->second };
            change.is_binary = entry.is_binary;
            change.lines_added = entry.lines_added;
            change.lines_removed = entry.lines_removed;
        }
    }
} // namespace cyella
