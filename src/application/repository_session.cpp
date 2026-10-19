#include "application/repository_session.h"

#include <algorithm>
#include <utility>

namespace cyella {
    repository_session::repository_session(std::string id, std::u8string path)
        : id_ { std::move(id) }
        , path_ { std::move(path) }
    {}

    std::optional<bool> repository_session::ancestry(const std::string& ancestor, const std::string& descendant) const
    {
        const auto found { ancestry_.find({ ancestor, descendant }) };
        if (found == ancestry_.end())
            return std::nullopt;
        return found->second;
    }

    std::optional<std::uint32_t> repository_session::commit_count(const std::string& from, const std::string& to) const
    {
        const auto found { commit_counts_.find({ from, to }) };
        if (found == commit_counts_.end())
            return std::nullopt;
        return found->second;
    }

    std::uint64_t repository_session::begin_refresh() noexcept
    {
        ++revision_;
        return ++requested_generation_;
    }

    void repository_session::begin_log_page() noexcept
    {
        page_loading_ = true;
        ++revision_;
    }

    void repository_session::expect_commit_detail(const operation_id id) noexcept
    {
        expected_detail_ = id;
    }

    void repository_session::expect_file_diff(const operation_id id) noexcept
    {
        expected_diff_ = id;
    }

    void repository_session::set_awaiting_input(const std::optional<operation_id> id) noexcept
    {
        awaiting_input_ = id;
        ++revision_;
    }

    bool repository_session::apply(const snapshot_loaded& event)
    {
        // 더 새 refresh를 이미 냈으면 이 snapshot은 곧 덮인다. 섞어 그리지 않게 버린다.
        if (event.generation != requested_generation_)
            return false;

        snapshot_ = event.snapshot;
        // 원격 목록에서 빠진 원격의 태그 기록은 더 이상 뜻이 없다.
        for (auto entry { remote_tags_.begin() }; entry != remote_tags_.end();)
        {
            const bool listed { std::any_of(snapshot_.summary.remotes.begin(), snapshot_.summary.remotes.end(), [&entry](const remote_info& remote) { return remote.name == entry->first; }) };
            entry = listed ? std::next(entry) : remote_tags_.erase(entry);
        }
        apply_known_remote_tags();

        // 레이아웃은 온라인 배치라 행을 고칠 수 없다. 새 generation은 처음부터 다시 쌓는다.
        layout_ = commit_graph_layout {};
        rows_.clear();
        row_commits_.clear();
        has_wip_row_ = false;
        loaded_commits_ = 0;
        batch_count_ = 0;
        page_loading_ = true;
        history_complete_ = false;
        displayed_generation_ = event.generation;

        // HEAD를 이 시점에 알아야 WIP 행을 첫 쪽 앞에 끼울 수 있다. snapshot이 log보다 먼저 온다.
        if (snapshot_.summary.changed_file_count > 0)
        {
            const graph_commit wip { make_wip_commit(snapshot_.summary.head_sha) };
            std::vector<graph_row> appended { layout_.append(std::span<const graph_commit> { &wip, 1 }) };
            rows_.insert(rows_.end(), std::make_move_iterator(appended.begin()), std::make_move_iterator(appended.end()));
            commit_info commit {};
            commit.sha = wip.sha;
            commit.parents = wip.parent_shas;
            row_commits_.push_back(std::move(commit));
            has_wip_row_ = true;
        }
        ++revision_;
        return true;
    }

    bool repository_session::apply(const commits_loaded& event)
    {
        if (event.generation != requested_generation_ || event.generation != displayed_generation_)
            return false;
        // 쪽 경계가 어긋났다(같은 쪽을 두 번 읽었거나 앞 묶음이 빠졌다). 이어 붙이면 행이 틀린다.
        if (event.first_index != loaded_commits_)
            return false;

        std::vector<graph_commit> commits {};
        commits.reserve(event.commits.size());
        for (const commit_info& commit : event.commits)
            commits.push_back(graph_commit { commit.sha, commit.parents });
        std::vector<graph_row> appended { layout_.append(commits) };
        rows_.insert(rows_.end(), std::make_move_iterator(appended.begin()), std::make_move_iterator(appended.end()));
        row_commits_.insert(row_commits_.end(), event.commits.begin(), event.commits.end());
        loaded_commits_ += event.commits.size();
        ++batch_count_;
        if (event.page_complete)
        {
            page_loading_ = false;
            history_complete_ = event.history_complete;
        }
        ++revision_;
        return true;
    }

    bool repository_session::apply(const remote_tags_loaded& event)
    {
        remote_tags_[event.remote] = event.tags;
        apply_remote_tags(snapshot_.refs, event.remote, event.tags);
        ++revision_;
        return true;
    }

    bool repository_session::apply(const operation_id id, const commit_detail_loaded& event)
    {
        // 사용자가 다른 커밋을 이미 골랐으면 앞선 선택의 결과를 버린다.
        if (id != expected_detail_)
            return false;
        commit_detail_ = event.detail;
        ++revision_;
        return true;
    }

    bool repository_session::apply(const operation_id id, const file_diff_loaded& event)
    {
        if (id != expected_diff_)
            return false;
        file_diff_ = event.diff;
        ++revision_;
        return true;
    }

    bool repository_session::apply(const ancestry_answered& event)
    {
        ancestry_[{ event.ancestor, event.descendant }] = event.is_ancestor;
        ++revision_;
        return true;
    }

    bool repository_session::apply(const commit_count_answered& event)
    {
        commit_counts_[{ event.from, event.to }] = event.count;
        ++revision_;
        return true;
    }

    void repository_session::abandon_log_page(const std::uint64_t generation) noexcept
    {
        if (generation != displayed_generation_)
            return;
        page_loading_ = false;
        ++revision_;
    }

    void repository_session::apply_known_remote_tags()
    {
        for (const auto& [remote, tags] : remote_tags_)
            apply_remote_tags(snapshot_.refs, remote, tags);
    }
} // namespace cyella
