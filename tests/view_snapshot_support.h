#pragma once

// 화면 snapshot test가 함께 쓰는 세션·입력 조립기다. git 없이 세션에 사건을 직접 넣는다.
#include "application/operation_events.h"
#include "application/repository_session.h"
#include "application/repository_tabs.h"
#include "presentation/build_view_snapshot.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cyella::test::snapshot {
    inline std::string sha_of(const std::size_t index)
    {
        char buffer[41] {};
        std::snprintf(buffer, sizeof(buffer), "%08zx%032zx", index + 1, index + 1);
        return buffer;
    }

    // 처음 커밋이 맨 아래인 한 줄 이력이다. `count`개를 topo 순서(자식 먼저)로 돌려준다.
    inline std::vector<commit_info> linear_history(const std::size_t count, const std::int64_t time = 1700000000)
    {
        std::vector<commit_info> commits {};
        commits.reserve(count);
        for (std::size_t index { count }; index > 0; --index)
        {
            commit_info commit {};
            commit.sha = sha_of(index - 1);
            if (index > 1)
                commit.parents = { sha_of(index - 2) };
            commit.subject = "commit " + std::to_string(index);
            commit.author_name = "Lusain";
            commit.author_time = time;
            commits.push_back(std::move(commit));
        }
        return commits;
    }

    inline repository_snapshot make_snapshot(const std::string& head, const std::uint32_t changed)
    {
        repository_snapshot snapshot {};
        snapshot.summary.current_branch = "main";
        snapshot.summary.head_sha = head;
        snapshot.summary.has_upstream = true;
        snapshot.summary.upstream = "origin/main";
        snapshot.summary.ahead = 1;
        snapshot.summary.changed_file_count = changed;
        snapshot.summary.remotes = { { .name = "origin", .fetch_url = "https://example.com/repo.git" } };
        snapshot.refs = {
            { .kind = ref_kind::local, .name = "main", .full_name = "refs/heads/main", .target_sha = head, .upstream = "origin/main", .ahead = 1, .is_current = true },
            { .kind = ref_kind::local, .name = "feature/graph", .full_name = "refs/heads/feature/graph", .target_sha = sha_of(0) },
            { .kind = ref_kind::remote, .name = "origin/main", .full_name = "refs/remotes/origin/main", .target_sha = sha_of(0) },
            { .kind = ref_kind::tag, .name = "v1.0", .full_name = "refs/tags/v1.0", .target_sha = sha_of(0) },
        };
        if (changed > 0)
        {
            snapshot.unstaged = { { .path = "src/a.cpp", .status = 'M', .side = change_side::unstaged, .lines_added = 3, .lines_removed = 1 } };
            snapshot.staged = { { .path = "README.md", .status = 'A', .side = change_side::staged } };
        }
        return snapshot;
    }

    inline void load(repository_session& session, std::vector<commit_info> commits, const std::uint32_t changed)
    {
        const std::uint64_t generation { session.begin_refresh() };
        REQUIRE(session.apply(snapshot_loaded { generation, make_snapshot(commits.front().sha, changed) }));
        REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = std::move(commits), .page_complete = true, .history_complete = true }));
    }

    struct snapshot_harness
    {
        repository_tabs tabs {};
        app_settings settings {};
        ui_state ui {};
        std::map<repository_tab_id, std::string> tab_repositories {};
        std::map<std::string, std::unique_ptr<repository_session>> sessions {};
        std::vector<operation_status> operations {};

        repository_tab_id open(const std::u8string& path, const std::string& id)
        {
            const repository_tab_id tab { tabs.open_repository(path).id };
            tab_repositories[tab] = id;
            sessions[id] = std::make_unique<repository_session>(id, path);
            return tab;
        }

        [[nodiscard]] view_snapshot build(const float width = 1440.0f) const
        {
            view_inputs inputs {};
            inputs.width = width;
            inputs.height = 900.0f;
            inputs.scale = 1.0f;
            inputs.tabs = &tabs;
            inputs.settings = &settings;
            inputs.ui = &ui;
            inputs.tab_repositories = &tab_repositories;
            inputs.find_session = [this](const std::string& id) -> const repository_session* {
                const auto found { sessions.find(id) };
                return found == sessions.end() ? nullptr : found->second.get();
            };
            for (const operation_status& status : operations)
                inputs.operations.push_back(&status);
            inputs.exists = [](const std::u8string_view path) { return path.find(u8"missing") == std::u8string_view::npos; };
            inputs.git_version = u8"git version 2.52.0";
            inputs.now = std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } };
            return build_view_snapshot(inputs);
        }
    };

    inline luil::text::text_edit_state text(std::u8string value)
    {
        luil::text::text_edit_state state {};
        state.caret = value.size();
        state.anchor = value.size();
        state.text = std::move(value);
        return state;
    }
} // namespace cyella::test::snapshot
