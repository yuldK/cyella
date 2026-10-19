#include "domain/patch_builder.h"

#include <string_view>

namespace cyella {
    namespace {
        // patch에 실을 줄 하나다. text는 원본 hunk의 줄을 가리킨다.
        struct patch_line
        {
            char sign { ' ' };
            const std::string* text { nullptr };
            // 바로 뒤에 `\ No newline at end of file`이 붙는다.
            bool no_newline { false };
        };

        constexpr std::string_view default_mode { "100644" };

        bool is_change(const diff_line& line) noexcept
        {
            return line.kind == diff_line_kind::add || line.kind == diff_line_kind::del;
        }

        // git은 제어 문자, 큰따옴표, 역슬래시가 든 경로를 C 문자열처럼 따옴표로 싸서 쓴다. `git apply`도
        // 같은 규칙으로 읽는다. ASCII 밖(한글)은 그대로 둬도 읽는다.
        bool needs_quoting(const std::string_view path) noexcept
        {
            for (const char letter : path)
            {
                const auto code { static_cast<unsigned char>(letter) };
                if (code < 0x20 || code == 0x7F || letter == '"' || letter == '\\')
                    return true;
            }
            return false;
        }

        std::string quote_path(const std::string_view prefix, const std::string_view path)
        {
            std::string full { prefix };
            full.append(path);
            if (needs_quoting(path) == false)
                return full;

            std::string quoted { "\"" };
            for (const char letter : full)
            {
                const auto code { static_cast<unsigned char>(letter) };
                switch (letter)
                {
                case '"':
                    quoted += "\\\"";
                    break;
                case '\\':
                    quoted += "\\\\";
                    break;
                case '\n':
                    quoted += "\\n";
                    break;
                case '\t':
                    quoted += "\\t";
                    break;
                case '\r':
                    quoted += "\\r";
                    break;
                default:
                    if (code < 0x20 || code == 0x7F)
                    {
                        // 나머지 제어 문자는 세 자리 8진수다.
                        quoted.push_back('\\');
                        quoted.push_back(static_cast<char>('0' + ((code >> 6) & 0x7)));
                        quoted.push_back(static_cast<char>('0' + ((code >> 3) & 0x7)));
                        quoted.push_back(static_cast<char>('0' + (code & 0x7)));
                    }
                    else
                    {
                        quoted.push_back(letter);
                    }
                    break;
                }
            }
            quoted.push_back('"');
            return quoted;
        }

        // `---`·`+++` 줄의 이름이다. 공백이 든 이름은 git이 하듯 끝에 탭을 붙여 이름의 끝을 알린다.
        std::string marker_name(const std::string_view prefix, const std::string_view path)
        {
            std::string name { quote_path(prefix, path) };
            if (name.front() != '"' && path.find(' ') != std::string_view::npos)
                name.push_back('\t');
            return name;
        }

        // 개수가 0인 쪽의 시작은 "그 줄 다음에 끼운다"는 뜻의 앞 줄 번호라, 실제 첫 줄은 하나 뒤다.
        std::int32_t first_line(const std::int32_t start, const std::int32_t count) noexcept
        {
            return count == 0 ? start + 1 : start;
        }

        // 줄바꿈 없는 끝 줄을 가진 del과 그 뒤 add를 한 묶음으로 보고, 하나라도 골랐으면 모두 고른다.
        // 따로 고르면 줄바꿈 없는 줄 뒤에 다른 줄이 오는 patch가 되어 git이 거절한다.
        void close_end_of_file_groups(const diff_hunk& hunk, std::vector<bool>& selected)
        {
            const std::size_t count { hunk.lines.size() };
            for (std::size_t index { 0 }; index + 1 < count; ++index)
            {
                if (hunk.lines[index].kind != diff_line_kind::del || hunk.lines[index + 1].kind != diff_line_kind::no_newline)
                    continue;
                bool any { selected[index] };
                for (std::size_t later { index + 1 }; later < count && any == false; ++later)
                    any = hunk.lines[later].kind == diff_line_kind::add && selected[later];
                if (any == false)
                    continue;
                selected[index] = true;
                for (std::size_t later { index + 1 }; later < count; ++later)
                    if (hunk.lines[later].kind == diff_line_kind::add)
                        selected[later] = true;
            }
        }

        void append_range(std::string& patch, const std::int32_t start, const std::int32_t count)
        {
            patch += std::to_string(start);
            patch.push_back(',');
            patch += std::to_string(count);
        }
    } // namespace

    patch_apply_mode patch_apply_mode_for(const patch_direction direction) noexcept
    {
        switch (direction)
        {
        case patch_direction::stage:
            return { true, false };
        case patch_direction::unstage:
            return { true, true };
        case patch_direction::discard:
            return { false, true };
        }
        return {};
    }

    std::vector<std::size_t> changed_line_indices(const diff_hunk& hunk)
    {
        std::vector<std::size_t> indices {};
        for (std::size_t index { 0 }; index < hunk.lines.size(); ++index)
            if (is_change(hunk.lines[index]))
                indices.push_back(index);
        return indices;
    }

    std::optional<std::string> build_hunk_patch(const diff_file& file, const std::size_t hunk_index, const patch_direction direction)
    {
        if (hunk_index >= file.hunks.size())
            return std::nullopt;
        return build_line_patch(file, hunk_index, changed_line_indices(file.hunks[hunk_index]), direction);
    }

    std::optional<std::string> build_line_patch(const diff_file& file, const std::size_t hunk_index, const std::vector<std::size_t>& selected_lines, const patch_direction direction)
    {
        if (file.is_binary || hunk_index >= file.hunks.size())
            return std::nullopt;
        const diff_hunk& hunk { file.hunks[hunk_index] };

        std::vector<bool> selected(hunk.lines.size(), false);
        for (const std::size_t index : selected_lines)
        {
            if (index >= hunk.lines.size() || is_change(hunk.lines[index]) == false)
                return std::nullopt;
            selected[index] = true;
        }
        close_end_of_file_groups(hunk, selected);

        // stage는 옛쪽(index나 작업 트리의 변경 전)에 적용하므로 고르지 않은 del이 남아 context가 된다.
        // unstage·discard는 -R이 부호를 뒤집어 새쪽에 적용하므로 고르지 않은 add가 남아 context가 된다.
        const bool target_is_old_side { direction == patch_direction::stage };

        std::vector<patch_line> lines {};
        lines.reserve(hunk.lines.size());
        bool previous_emitted { false };
        for (std::size_t index { 0 }; index < hunk.lines.size(); ++index)
        {
            const diff_line& line { hunk.lines[index] };
            switch (line.kind)
            {
            case diff_line_kind::context:
                lines.push_back({ ' ', &line.content, false });
                previous_emitted = true;
                break;
            case diff_line_kind::add:
                if (selected[index])
                    lines.push_back({ '+', &line.content, false });
                else if (target_is_old_side == false)
                    lines.push_back({ ' ', &line.content, false });
                previous_emitted = selected[index] || target_is_old_side == false;
                break;
            case diff_line_kind::del:
                if (selected[index])
                    lines.push_back({ '-', &line.content, false });
                else if (target_is_old_side)
                    lines.push_back({ ' ', &line.content, false });
                previous_emitted = selected[index] || target_is_old_side;
                break;
            case diff_line_kind::no_newline:
                // 표지는 바로 앞 줄을 꾸민다. 그 줄이 빠졌으면 표지도 뺀다.
                if (previous_emitted && lines.empty() == false)
                    lines.back().no_newline = true;
                break;
            }
        }

        std::int32_t old_count { 0 };
        std::int32_t new_count { 0 };
        bool has_change { false };
        for (const patch_line& line : lines)
        {
            if (line.sign != '+')
                ++old_count;
            if (line.sign != '-')
                ++new_count;
            has_change = has_change || line.sign != ' ';
        }
        if (has_change == false)
            return std::nullopt;

        const bool creation { file.status == 'A' && old_count == 0 };
        const bool deletion { file.status == 'D' && new_count == 0 };

        // 적용 대상 쪽의 hunk 첫 줄이 patch의 두 쪽 모두에서 시작 위치다. 다른 hunk를 싣지 않으므로
        // 앞선 변경이 만드는 어긋남이 없다.
        const std::int32_t anchor { target_is_old_side ? first_line(hunk.old_start, hunk.old_count) : first_line(hunk.new_start, hunk.new_count) };
        const std::int32_t old_start { old_count == 0 ? anchor - 1 : anchor };
        const std::int32_t new_start { new_count == 0 ? anchor - 1 : anchor };

        // 이름이 바뀐 파일도 적용 대상에 있는 쪽의 이름 하나로 쓴다. 이름 변경 자체는 되돌리지 않는다.
        const std::string& old_name { file.old_path.empty() ? file.new_path : file.old_path };
        const std::string& new_name { file.new_path.empty() ? file.old_path : file.new_path };
        const std::string& path { target_is_old_side ? old_name : new_name };

        std::string patch {};
        patch += "diff --git " + quote_path("a/", path) + " " + quote_path("b/", path) + "\n";
        if (creation)
        {
            patch += "new file mode " + (file.new_mode.empty() ? std::string { default_mode } : file.new_mode) + "\n";
            patch += "--- /dev/null\n";
            patch += "+++ " + marker_name("b/", path) + "\n";
        }
        else if (deletion)
        {
            patch += "deleted file mode " + (file.old_mode.empty() ? std::string { default_mode } : file.old_mode) + "\n";
            patch += "--- " + marker_name("a/", path) + "\n";
            patch += "+++ /dev/null\n";
        }
        else
        {
            patch += "--- " + marker_name("a/", path) + "\n";
            patch += "+++ " + marker_name("b/", path) + "\n";
        }

        patch += "@@ -";
        append_range(patch, old_start, old_count);
        patch += " +";
        append_range(patch, new_start, new_count);
        patch += " @@";
        if (hunk.section.empty() == false)
            patch += " " + hunk.section;
        patch += "\n";

        for (const patch_line& line : lines)
        {
            patch.push_back(line.sign);
            patch += *line.text;
            patch.push_back('\n');
            if (line.no_newline)
                patch += "\\ No newline at end of file\n";
        }
        return patch;
    }
} // namespace cyella
