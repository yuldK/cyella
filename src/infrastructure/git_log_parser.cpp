#include "infrastructure/git_log_parser.h"

#include "infrastructure/git_text.h"

#include <cstdint>
#include <utility>

namespace cyella {
    namespace {
        constexpr char field_separator { '\x1F' };

        bool is_hex_object_name(const std::string_view text) noexcept
        {
            if (text.size() != 40 && text.size() != 64)
                return false;
            for (const char value : text)
            {
                const bool is_hex { (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') };
                if (is_hex == false)
                    return false;
            }
            return true;
        }
    } // namespace

    git_log_parser::git_log_parser(commit_handler handler, const bool has_body)
        : handler_ { std::move(handler) }
        , has_body_ { has_body }
    {}

    void git_log_parser::feed(std::string_view chunk)
    {
        if (pending_.empty())
        {
            // 꼬리가 없으면 조각 안의 완결된 레코드를 복사 없이 바로 읽는다.
            std::size_t nul { chunk.find('\0') };
            while (nul != std::string_view::npos)
            {
                process_record(chunk.substr(0, nul));
                chunk.remove_prefix(nul + 1);
                nul = chunk.find('\0');
            }
            pending_.assign(chunk);
            scanned_ = pending_.size();
            return;
        }

        pending_.append(chunk);
        std::size_t consumed { 0 };
        std::size_t nul { pending_.find('\0', scanned_) };
        while (nul != std::string::npos)
        {
            process_record(std::string_view { pending_ }.substr(consumed, nul - consumed));
            consumed = nul + 1;
            nul = pending_.find('\0', consumed);
        }

        // 처리한 앞부분은 한 번에 지운다. 레코드마다 지우면 긴 꼬리를 여러 번 옮기게 된다.
        if (consumed != 0)
            pending_.erase(0, consumed);
        scanned_ = pending_.size();
    }

    void git_log_parser::finish()
    {
        if (pending_.empty() == false)
        {
            // 형식상 마지막 레코드도 NUL로 끝난다. 끝나지 않았다면 출력이 잘렸을 수 있으니
            // 해석되는 경우에만 받아들이고, 아니면 깨진 레코드로 센다.
            const std::string tail { std::move(pending_) };
            process_record(tail);
        }
        pending_.clear();
        scanned_ = 0;
    }

    void git_log_parser::process_record(std::string_view record)
    {
        // 일부 git 버전은 레코드 사이에 줄바꿈을 끼운다. 커밋 해시는 16진수라 안전하게 뗄 수 있다.
        while (record.empty() == false && (record.front() == '\n' || record.front() == '\r'))
            record.remove_prefix(1);
        if (record.empty())
            return;

        std::string_view sha {};
        std::string_view parents {};
        std::string_view author_name {};
        std::string_view author_email {};
        std::string_view author_time {};
        std::string_view committer_time {};
        std::string_view subject {};
        std::string_view body {};

        std::string_view rest { record };
        const bool complete = git_text::take_field(rest, field_separator, sha) && git_text::take_field(rest, field_separator, parents) && git_text::take_field(rest, field_separator, author_name)
            && git_text::take_field(rest, field_separator, author_email) && git_text::take_field(rest, field_separator, author_time) && git_text::take_field(rest, field_separator, committer_time);
        if (complete == false)
        {
            ++malformed_record_count_;
            return;
        }

        // 본문을 요청했다면 제목 뒤의 구분자 하나가 반드시 있어야 한다. 본문은 구분자를
        // 포함해도 남은 전부로 본다.
        if (has_body_)
        {
            if (git_text::take_field(rest, field_separator, subject) == false)
            {
                ++malformed_record_count_;
                return;
            }
            body = rest;
        }
        else
        {
            subject = rest;
        }

        const auto parsed_author_time { git_text::parse_integer<std::int64_t>(author_time) };
        const auto parsed_committer_time { git_text::parse_integer<std::int64_t>(committer_time) };
        if (is_hex_object_name(sha) == false || parsed_author_time.has_value() == false || parsed_committer_time.has_value() == false)
        {
            ++malformed_record_count_;
            return;
        }

        commit_info commit {};
        commit.sha = std::string { sha };
        while (parents.empty() == false)
        {
            std::string_view parent {};
            git_text::take_field(parents, ' ', parent);
            if (parent.empty() == false)
                commit.parents.emplace_back(parent);
        }
        commit.author_name = std::string { author_name };
        commit.author_email = std::string { author_email };
        commit.author_time = *parsed_author_time;
        commit.committer_time = *parsed_committer_time;
        commit.subject = std::string { subject };
        if (has_body_)
            commit.body = std::string { body };

        ++commit_count_;
        if (handler_)
            handler_(std::move(commit));
    }

    std::vector<commit_info> parse_git_log(const std::string_view bytes, const bool has_body)
    {
        std::vector<commit_info> commits {};
        git_log_parser parser { [&commits](commit_info&& commit) { commits.push_back(std::move(commit)); }, has_body };
        parser.feed(bytes);
        parser.finish();
        return commits;
    }
} // namespace cyella
