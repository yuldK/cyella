#pragma once

#include "domain/ref_name.h"
#include "infrastructure/git_command_builder.h"

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

// submodule·worktree·Git LFS 명령 조립기가 함께 쓰는 작은 도구다. 같은 규칙(옵션처럼 보이는 이름과 빈 경로 거절)을
// `git_command_builder.cpp`의 것과 맞춘다. 그쪽 도구는 파일 안에 숨어 있어 새 조립기는 여기서 쓴다.
namespace cyella::git_extension {
    inline void push_text(git_arguments& arguments, const std::string_view value)
    {
        arguments.emplace_back(value.begin(), value.end());
    }

    [[noreturn]] inline void reject(const std::string_view what, const std::string_view reason)
    {
        std::string message { what };
        message.append(": ").append(reason);
        throw std::invalid_argument { message };
    }

    inline std::string_view require_revision(const std::string_view value, const std::string_view what)
    {
        if (const auto problem { find_revision_problem(value) })
            reject(what, ref_name_problem_text(*problem));
        return value;
    }

    inline std::string_view require_new_ref_name(const std::string_view value, const std::string_view what)
    {
        if (const auto problem { find_ref_name_problem(value) })
            reject(what, ref_name_problem_text(*problem));
        return value;
    }

    inline std::string_view require_path(const std::string_view value, const std::string_view what = "path")
    {
        if (value.empty())
            reject(what, "path is empty");
        if (value.find('\0') != std::string_view::npos)
            reject(what, "path contains NUL");
        return value;
    }

    // `-- <paths>`를 붙인다. 비어 있으면 아무것도 붙이지 않는다(전체 대상).
    inline void push_optional_paths(git_arguments& arguments, const std::span<const std::string> paths)
    {
        if (paths.empty())
            return;
        push_text(arguments, "--");
        for (const std::string& path : paths)
            push_text(arguments, require_path(path));
    }
} // namespace cyella::git_extension
