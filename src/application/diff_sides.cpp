#include "application/diff_sides.h"

#include <array>

namespace cyella {
    namespace {
        bool ends_with_ignoring_case(const std::string_view text, const std::string_view suffix) noexcept
        {
            if (text.size() < suffix.size())
                return false;
            const std::string_view tail { text.substr(text.size() - suffix.size()) };
            for (std::size_t index { 0 }; index < suffix.size(); ++index)
            {
                char letter { tail[index] };
                if (letter >= 'A' && letter <= 'Z')
                    letter = static_cast<char>(letter - 'A' + 'a');
                if (letter != suffix[index])
                    return false;
            }
            return true;
        }

        diff_side_spec side_of(const diff_side_kind kind, std::string revision, const std::string& path)
        {
            if (path.empty())
                return {};
            return { kind, std::move(revision), path };
        }
    } // namespace

    bool is_image_path(const std::string_view path) noexcept
    {
        constexpr std::array<std::string_view, 8> extensions { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", ".ico" };
        for (const std::string_view extension : extensions)
            if (ends_with_ignoring_case(path, extension))
                return true;
        return false;
    }

    diff_side_pair resolve_diff_sides(const diff_side_plan& plan)
    {
        const bool added { plan.status == 'A' || plan.old_path.empty() };
        const bool deleted { plan.status == 'D' || plan.new_path.empty() };
        // 이름이 바뀐 파일의 왼쪽은 옛 경로, 그 밖에는 양쪽이 같은 경로다.
        const std::string& left_path { plan.old_path.empty() ? plan.new_path : plan.old_path };
        const std::string& right_path { plan.new_path.empty() ? plan.old_path : plan.new_path };

        diff_side_pair pair {};
        switch (plan.view)
        {
        case diff_view::unstaged:
            pair.left = added || plan.untracked ? diff_side_spec {} : side_of(diff_side_kind::index, {}, left_path);
            pair.right = deleted ? diff_side_spec {} : side_of(diff_side_kind::worktree, {}, right_path);
            break;
        case diff_view::staged:
            pair.left = added ? diff_side_spec {} : side_of(diff_side_kind::revision, "HEAD", left_path);
            pair.right = deleted ? diff_side_spec {} : side_of(diff_side_kind::index, {}, right_path);
            break;
        case diff_view::commit:
            pair.left = added ? diff_side_spec {} : side_of(diff_side_kind::revision, plan.sha + "^", left_path);
            pair.right = deleted ? diff_side_spec {} : side_of(diff_side_kind::revision, plan.sha, right_path);
            break;
        case diff_view::range:
            pair.left = added ? diff_side_spec {} : side_of(diff_side_kind::revision, plan.base, left_path);
            pair.right = deleted ? diff_side_spec {} : side_of(diff_side_kind::revision, plan.sha, right_path);
            break;
        }
        return pair;
    }
} // namespace cyella
