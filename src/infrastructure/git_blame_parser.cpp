#include "infrastructure/git_blame_parser.h"

#include "infrastructure/git_diff_parser.h"
#include "infrastructure/git_text.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        bool is_hex_object_name(const std::string_view text) noexcept
        {
            if (text.size() != 40 && text.size() != 64)
                return false;
            return std::all_of(text.begin(), text.end(), [](const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
        }

        // 커밋하지 않은 줄이다. git은 저장소의 객체 형식 길이(40자·64자)만큼 0을 낸다.
        bool is_null_object_name(const std::string_view text) noexcept
        {
            return std::all_of(text.begin(), text.end(), [](const char value) { return value == '0'; });
        }
    } // namespace

    void git_blame_parser::feed(std::string_view chunk)
    {
        if (pending_.empty())
        {
            // 꼬리가 없으면 조각 안의 완결된 줄을 복사 없이 바로 읽는다.
            std::size_t newline { chunk.find('\n') };
            while (newline != std::string_view::npos)
            {
                process_line(chunk.substr(0, newline));
                chunk.remove_prefix(newline + 1);
                newline = chunk.find('\n');
            }
            pending_.assign(chunk);
            scanned_ = pending_.size();
            return;
        }

        pending_.append(chunk);
        std::size_t consumed { 0 };
        std::size_t newline { pending_.find('\n', scanned_) };
        while (newline != std::string::npos)
        {
            process_line(std::string_view { pending_ }.substr(consumed, newline - consumed));
            consumed = newline + 1;
            newline = pending_.find('\n', consumed);
        }

        // 처리한 앞부분은 한 번에 지운다. 줄마다 지우면 긴 꼬리를 여러 번 옮기게 된다.
        if (consumed != 0)
            pending_.erase(0, consumed);
        scanned_ = pending_.size();
    }

    git_blame_result git_blame_parser::finish()
    {
        // git은 마지막 줄도 `\n`으로 끝낸다. 꼬리가 남았거나 머리만 오고 내용 줄이 없으면 출력이 잘린 것이다.
        if (pending_.empty() == false)
            ++result_.malformed_line_count;
        if (header_.active)
            ++result_.malformed_line_count;

        git_blame_result result { std::move(result_) };
        result_ = {};
        pending_.clear();
        scanned_ = 0;
        header_ = {};
        commits_.clear();
        group_sha_.clear();
        group_origin_ = 0;
        group_valid_ = false;
        return result;
    }

    void git_blame_parser::process_line(const std::string_view line)
    {
        // 내용 줄을 가장 먼저 가른다. 내용은 아무 byte나 담을 수 있지만 머리와 `키 값` 줄은 탭으로 시작하지 않는다.
        if (line.empty() == false && line.front() == '\t')
        {
            take_content(line.substr(1));
            return;
        }
        if (begin_header(line))
            return;
        if (header_.active)
        {
            read_detail(line);
            return;
        }
        ++result_.malformed_line_count;
    }

    bool git_blame_parser::begin_header(const std::string_view line)
    {
        std::string_view rest { line };
        std::string_view sha {};
        std::string_view original_field {};
        std::string_view final_field {};
        if (git_text::take_field(rest, ' ', sha) == false || is_hex_object_name(sha) == false || git_text::take_field(rest, ' ', original_field) == false)
            return false;
        // 셋째 필드 뒤에 남은 것이 있으면 묶음의 줄 수이고, 이 줄이 묶음의 첫 줄이다.
        const bool starts_group { git_text::take_field(rest, ' ', final_field) };
        const auto original_line { git_text::parse_integer<std::uint32_t>(original_field) };
        const auto final_line { git_text::parse_integer<std::uint32_t>(final_field) };
        if (original_line.has_value() == false || final_line.has_value() == false || (starts_group && git_text::parse_integer<std::uint32_t>(rest).has_value() == false))
            return false;

        // 앞 머리의 내용 줄이 오지 않았다.
        if (header_.active)
            ++result_.malformed_line_count;
        header_.active = true;
        header_.sha.assign(sha);
        header_.original_line = *original_line;
        header_.final_line = *final_line;
        header_.starts_group = starts_group;
        header_.has_filename = false;
        header_.details = {};
        return true;
    }

    void git_blame_parser::read_detail(const std::string_view line)
    {
        std::string_view value { line };
        std::string_view key {};
        git_text::take_field(value, ' ', key);
        blame_origin& details { header_.details };
        if (key == "author")
        {
            details.author_name.assign(value);
        }
        else if (key == "author-mail")
        {
            if (value.size() >= 2 && value.front() == '<' && value.back() == '>')
                value = value.substr(1, value.size() - 2);
            details.author_email.assign(value);
        }
        else if (key == "author-time")
        {
            details.author_time = git_text::parse_integer<std::int64_t>(value).value_or(0);
        }
        else if (key == "summary")
        {
            details.summary.assign(value);
        }
        else if (key == "previous")
        {
            std::string_view sha {};
            git_text::take_field(value, ' ', sha);
            details.previous_sha.assign(sha);
            details.previous_path = unquote_git_path(value);
        }
        else if (key == "filename")
        {
            details.path = unquote_git_path(value);
            header_.has_filename = true;
        }
        // 그 밖의 키(`committer`, `boundary`, `ignored` 등)는 쓰지 않는다. 새 키가 생겨도 건너뛴다.
    }

    void git_blame_parser::take_content(const std::string_view content)
    {
        if (header_.active == false)
        {
            ++result_.malformed_line_count;
            return;
        }
        header_.active = false;
        std::uint32_t origin { 0 };
        if (resolve_origin(origin) == false)
        {
            ++result_.malformed_line_count;
            return;
        }
        if (result_.binary)
            return;
        if (content.find('\0') != std::string_view::npos)
        {
            // 줄로 보일 수 없는 파일이다. 이미 모은 줄은 버리고 남은 출력은 주인만 읽는다.
            result_.binary = true;
            result_.lines = {};
            return;
        }
        result_.lines.push_back(blame_line { origin, header_.original_line, header_.final_line, std::string { content } });
    }

    bool git_blame_parser::resolve_origin(std::uint32_t& origin)
    {
        // 묶음의 둘째 줄부터는 머리에 아무 정보가 없다. 묶음의 주인을 그대로 쓴다.
        if (header_.starts_group == false && group_valid_ && header_.sha == group_sha_)
        {
            origin = group_origin_;
            return true;
        }

        auto commit { commits_.find(std::string_view { header_.sha }) };
        if (header_.has_filename == false)
        {
            // 이미 나온 커밋이고 경로가 하나뿐이면 git은 `filename`을 다시 내지 않는다.
            if (commit == commits_.end())
                return false;
            origin = commit->second.latest;
        }
        else
        {
            if (commit == commits_.end())
                commit = commits_.emplace(header_.sha, commit_origins {}).first;
            commit_origins& known { commit->second };
            const auto same_path { std::find_if(known.indices.begin(), known.indices.end(), [this](const std::uint32_t index) { return result_.origins[index].path == header_.details.path; }) };
            if (same_path != known.indices.end())
            {
                origin = *same_path;
            }
            else
            {
                blame_origin created { std::move(header_.details) };
                header_.details = {};
                created.sha = header_.sha;
                created.uncommitted = is_null_object_name(header_.sha);
                // 커밋 정보는 그 커밋이 처음 나올 때만 온다. 같은 커밋의 다른 경로는 첫 주인에게서 가져온다.
                // `previous`는 경로마다 다르므로 이 묶음의 것만 쓴다.
                if (known.indices.empty() == false)
                {
                    const blame_origin& first { result_.origins[known.indices.front()] };
                    created.author_name = first.author_name;
                    created.author_email = first.author_email;
                    created.author_time = first.author_time;
                    created.summary = first.summary;
                }
                origin = static_cast<std::uint32_t>(result_.origins.size());
                result_.origins.push_back(std::move(created));
                known.indices.push_back(origin);
            }
            known.latest = origin;
        }

        group_sha_ = header_.sha;
        group_origin_ = origin;
        group_valid_ = true;
        return true;
    }

    git_blame_result parse_git_blame(const std::string_view bytes)
    {
        git_blame_parser parser {};
        parser.feed(bytes);
        return parser.finish();
    }
} // namespace cyella
