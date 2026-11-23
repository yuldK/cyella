#include "infrastructure/git_file_log_parser.h"

#include "infrastructure/git_log_parser.h"

#include <utility>

namespace cyella {
    namespace {
        constexpr char record_start { '\x1E' };
    } // namespace

    git_file_log_parser::git_file_log_parser(entry_handler handler, std::string path)
        : handler_ { std::move(handler) }
        , followed_path_ { path }
        , initial_path_ { std::move(path) }
    {}

    void git_file_log_parser::feed(std::string_view chunk)
    {
        if (pending_.empty())
        {
            // 꼬리가 없으면 조각 안의 완결된 토큰을 복사 없이 바로 읽는다.
            std::size_t nul { chunk.find('\0') };
            while (nul != std::string_view::npos)
            {
                process_token(chunk.substr(0, nul));
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
            process_token(std::string_view { pending_ }.substr(consumed, nul - consumed));
            consumed = nul + 1;
            nul = pending_.find('\0', consumed);
        }

        if (consumed != 0)
            pending_.erase(0, consumed);
        scanned_ = pending_.size();
    }

    void git_file_log_parser::finish()
    {
        // 모든 토큰은 NUL로 끝난다. 꼬리가 남았으면 출력이 잘린 것이다. 줄바꿈만 남은 꼬리는 내용이 없어 무시한다.
        const bool truncated { paths_wanted_ != 0 || pending_.find_first_not_of('\n') != std::string::npos };
        if (truncated)
        {
            ++malformed_record_count_;
            // 경로를 다 받지 못한 레코드는 어느 파일의 것인지 알 수 없다.
            if (paths_wanted_ != 0 && keep_paths_)
                current_.reset();
        }
        emit_current();

        pending_.clear();
        scanned_ = 0;
        current_.reset();
        current_has_status_ = false;
        paths_wanted_ = 0;
        keep_paths_ = false;
        skipping_ = false;
        followed_path_ = initial_path_;
    }

    void git_file_log_parser::process_token(std::string_view token)
    {
        // 경로는 개수로 센다. 어떤 byte로 시작하든 경로다.
        if (paths_wanted_ != 0)
        {
            --paths_wanted_;
            if (keep_paths_ == false)
                return;
            // 이름 바꾸기·복사는 옛 경로가 먼저 온다.
            if (paths_wanted_ != 0)
                current_->previous_path.assign(token);
            else
                current_->path.assign(token);
            return;
        }

        // 머리 다음의 상태는 줄바꿈 뒤에 온다.
        while (token.empty() == false && token.front() == '\n')
            token.remove_prefix(1);
        if (token.empty())
            return;
        if (token.front() == record_start)
        {
            begin_record(token.substr(1));
            return;
        }
        if (skipping_ == false)
            begin_status(token);
    }

    void git_file_log_parser::begin_record(const std::string_view header)
    {
        emit_current();
        // 머리의 필드는 그래프 log와 같다. 같은 해석기로 읽어 검증을 한 곳에 둔다.
        std::vector<commit_info> commits { parse_git_log(header) };
        if (commits.size() != 1)
        {
            ++malformed_record_count_;
            skipping_ = true;
            return;
        }
        skipping_ = false;
        current_.emplace();
        current_->commit = std::move(commits.front());
        current_has_status_ = false;
    }

    void git_file_log_parser::begin_status(const std::string_view status)
    {
        const char letter { status.front() };
        if (letter < 'A' || letter > 'Z')
        {
            // 상태가 아니면 뒤따르는 경로 수를 알 수 없다. 이 레코드를 버리고 다음 머리에서 다시 맞춘다.
            ++malformed_record_count_;
            current_.reset();
            skipping_ = true;
            return;
        }
        paths_wanted_ = letter == 'R' || letter == 'C' ? 2 : 1;
        keep_paths_ = current_.has_value() && current_has_status_ == false;
        if (keep_paths_)
        {
            current_->status = letter;
            current_has_status_ = true;
        }
    }

    void git_file_log_parser::emit_current()
    {
        if (current_.has_value() == false)
            return;
        git_file_log_entry entry { std::move(*current_) };
        current_.reset();
        // name-status가 없는 커밋은 더 새 커밋에서 이어지는 경로에 있다.
        if (current_has_status_ == false)
            entry.path = followed_path_;
        followed_path_ = entry.previous_path.empty() ? entry.path : entry.previous_path;
        current_has_status_ = false;
        ++entry_count_;
        if (handler_)
            handler_(std::move(entry));
    }

    std::vector<git_file_log_entry> parse_git_file_log(const std::string_view bytes, const std::string_view path)
    {
        std::vector<git_file_log_entry> entries {};
        git_file_log_parser parser { [&entries](git_file_log_entry&& entry) { entries.push_back(std::move(entry)); }, std::string { path } };
        parser.feed(bytes);
        parser.finish();
        return entries;
    }
} // namespace cyella
