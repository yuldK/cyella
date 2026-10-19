#include "domain/ref_name.h"

#include <cstddef>

namespace cyella {
    namespace {
        bool is_control(const char value) noexcept
        {
            const auto byte { static_cast<unsigned char>(value) };
            return byte < 0x20u || byte == 0x7Fu;
        }

        // 슬래시 사이의 조각 하나에 적용되는 규칙이다.
        std::optional<ref_name_problem> find_component_problem(const std::string_view component) noexcept
        {
            if (component.empty())
                return std::nullopt;
            if (component.front() == '.')
                return ref_name_problem::component_starts_with_dot;
            if (component.size() >= 5 && component.substr(component.size() - 5) == ".lock")
                return ref_name_problem::component_ends_with_lock;
            return std::nullopt;
        }
    } // namespace

    std::optional<ref_name_problem> find_ref_name_problem(const std::string_view name) noexcept
    {
        if (name.empty())
            return ref_name_problem::empty;
        // 옵션으로 해석되는 것을 가장 먼저 막는다. git도 브랜치 이름의 `-` 시작을 거절한다.
        if (name.front() == '-')
            return ref_name_problem::leading_dash;
        if (name.front() == '/')
            return ref_name_problem::leading_slash;
        if (name.back() == '/')
            return ref_name_problem::trailing_slash;
        if (name.back() == '.')
            return ref_name_problem::trailing_dot;
        if (name == "@")
            return ref_name_problem::is_single_at;
        if (name == "HEAD")
            return ref_name_problem::is_head;

        for (std::size_t index { 0 }; index < name.size(); ++index)
        {
            const char value { name[index] };
            const bool has_next { index + 1 < name.size() };
            if (is_control(value))
                return ref_name_problem::control_character;
            if (value == '\\')
                return ref_name_problem::backslash;
            if (value == ' ' || value == '~' || value == '^' || value == ':' || value == '?' || value == '*' || value == '[')
                return ref_name_problem::forbidden_character;
            if (value == '.' && has_next && name[index + 1] == '.')
                return ref_name_problem::contains_double_dot;
            if (value == '@' && has_next && name[index + 1] == '{')
                return ref_name_problem::contains_at_brace;
            if (value == '/' && has_next && name[index + 1] == '/')
                return ref_name_problem::consecutive_slashes;
        }

        std::size_t begin { 0 };
        while (begin <= name.size())
        {
            std::size_t end { name.find('/', begin) };
            if (end == std::string_view::npos)
                end = name.size();
            if (const auto problem { find_component_problem(name.substr(begin, end - begin)) })
                return problem;
            begin = end + 1;
        }
        return std::nullopt;
    }

    std::optional<ref_name_problem> find_revision_problem(const std::string_view revision) noexcept
    {
        if (revision.empty())
            return ref_name_problem::empty;
        if (revision.front() == '-')
            return ref_name_problem::leading_dash;
        for (const char value : revision)
            if (is_control(value))
                return ref_name_problem::control_character;
        return std::nullopt;
    }

    std::string_view ref_name_problem_text(const ref_name_problem problem) noexcept
    {
        switch (problem)
        {
        case ref_name_problem::empty:
            return "name is empty";
        case ref_name_problem::leading_dash:
            return "name starts with '-'";
        case ref_name_problem::leading_slash:
            return "name starts with '/'";
        case ref_name_problem::trailing_slash:
            return "name ends with '/'";
        case ref_name_problem::consecutive_slashes:
            return "name contains '//'";
        case ref_name_problem::component_starts_with_dot:
            return "a path component starts with '.'";
        case ref_name_problem::component_ends_with_lock:
            return "a path component ends with '.lock'";
        case ref_name_problem::trailing_dot:
            return "name ends with '.'";
        case ref_name_problem::contains_double_dot:
            return "name contains '..'";
        case ref_name_problem::contains_at_brace:
            return "name contains '@{'";
        case ref_name_problem::is_single_at:
            return "name is '@'";
        case ref_name_problem::is_head:
            return "name is 'HEAD'";
        case ref_name_problem::forbidden_character:
            return "name contains a forbidden character (space ~ ^ : ? * [)";
        case ref_name_problem::control_character:
            return "name contains a control character";
        case ref_name_problem::backslash:
            return "name contains a backslash";
        }
        return "invalid name";
    }
} // namespace cyella
