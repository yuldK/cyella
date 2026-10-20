#pragma once

#include "application/operation_events.h"
#include "domain/commit_graph_layout.h"
#include "domain/commit_info.h"
#include "domain/operation_status.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cyella {
    // `status_loaded`를 세션에 반영한 결과다.
    enum class status_apply_result
    {
        // 늦게 왔거나 세션이 아직 읽히지 않아 버렸다.
        ignored,
        applied,
        // 브랜치·HEAD가 바뀌었거나 WIP 행이 생기거나 사라져야 한다. 그래프 레이아웃은 온라인 배치라 고칠 수 없으므로 전체 refresh가 필요하다.
        needs_refresh,
    };

    // 탭 하나(저장소 하나)의 상태다. logic thread 하나가 소유하고, worker는 사건으로만 바꾼다.
    // frame은 이 값을 읽어 만든다. 그래프 레이아웃도 여기 있어 한 스레드에서만 `append`된다.
    //
    // generation 규율: refresh를 낼 때마다 `begin_refresh`가 generation을 하나 올린다. 사건에
    // 실린 generation이 마지막으로 낸 값과 다르면 늦게 온 결과라 버린다. 새 generation의
    // snapshot이 오면 레이아웃을 통째로 새로 만들고, 그 뒤의 log 묶음만 이어 붙인다.
    class repository_session
    {
    public:
        repository_session(std::string id, std::u8string path);

        [[nodiscard]] const std::string& id() const noexcept
        {
            return id_;
        }

        [[nodiscard]] const std::u8string& path() const noexcept
        {
            return path_;
        }

        // 첫 snapshot이 오기 전이면 false다.
        [[nodiscard]] bool loaded() const noexcept
        {
            return displayed_generation_ != 0;
        }

        [[nodiscard]] const repository_snapshot& snapshot() const noexcept
        {
            return snapshot_;
        }

        // 그래프 행과, 같은 순서의 커밋이다. 작업 트리가 더러우면 0번이 WIP 행이다.
        [[nodiscard]] const std::vector<graph_row>& graph_rows() const noexcept
        {
            return rows_;
        }

        [[nodiscard]] const std::vector<commit_info>& graph_commits() const noexcept
        {
            return row_commits_;
        }

        [[nodiscard]] bool has_wip_row() const noexcept
        {
            return has_wip_row_;
        }

        [[nodiscard]] std::uint16_t graph_lane_count() const noexcept
        {
            return layout_.max_lane_count();
        }

        // 마지막으로 낸 refresh의 generation과, 지금 화면에 있는 snapshot의 generation이다.
        [[nodiscard]] std::uint64_t requested_generation() const noexcept
        {
            return requested_generation_;
        }

        [[nodiscard]] std::uint64_t displayed_generation() const noexcept
        {
            return displayed_generation_;
        }

        // WIP 행을 뺀, log에서 받은 커밋 수다. load_more_log의 시작 위치다.
        [[nodiscard]] std::size_t loaded_commit_count() const noexcept
        {
            return loaded_commits_;
        }

        [[nodiscard]] bool log_page_loading() const noexcept
        {
            return page_loading_;
        }

        [[nodiscard]] bool history_complete() const noexcept
        {
            return history_complete_;
        }

        // 지금 화면 generation에서 받은 log 묶음 수다(test와 진단용).
        [[nodiscard]] std::size_t commit_batch_count() const noexcept
        {
            return batch_count_;
        }

        [[nodiscard]] const std::optional<commit_detail>& selected_commit() const noexcept
        {
            return commit_detail_;
        }

        [[nodiscard]] const std::optional<file_diff>& selected_diff() const noexcept
        {
            return file_diff_;
        }

        // 읽어 둔 서명 정보다. 아직 검증하지 않은 커밋은 null이다. SHA별로 한 번만 읽고 refresh가 지우지 않는다 (커밋은 바뀌지 않는다).
        // 서명 설정이 바뀌면 `forget_signatures`로 비운다.
        [[nodiscard]] const commit_signature* signature_of(const std::string& sha) const noexcept;
        // 읽어 둔 서명 전체다 (SHA별). 그래프 행의 서명 표지가 읽는다.
        [[nodiscard]] const std::map<std::string, commit_signature>& signatures() const noexcept
        {
            return signatures_;
        }
        // 검증 요청을 이미 냈거나 결과가 있는 커밋이다 (같은 커밋을 거듭 요청하지 않으려는 용도).
        [[nodiscard]] bool signature_requested(const std::string& sha) const noexcept;
        void note_signatures_requested(const std::vector<std::string>& shas);
        // 검증이 끝나지 못했다 (취소, 실패). 다음에 다시 요청할 수 있게 한다.
        void abandon_signatures_requested(const std::vector<std::string>& shas);
        void forget_signatures();

        [[nodiscard]] std::optional<bool> ancestry(const std::string& ancestor, const std::string& descendant) const;
        [[nodiscard]] std::optional<std::uint32_t> commit_count(const std::string& from, const std::string& to) const;

        // 충돌·체크아웃 정책을 기다리는 operation이다. 이어서 낸 작업이 지운다.
        [[nodiscard]] std::optional<operation_id> awaiting_input() const noexcept
        {
            return awaiting_input_;
        }

        // 상태가 바뀔 때마다 오른다. driver가 frame을 다시 지을지 정한다.
        [[nodiscard]] std::uint64_t revision() const noexcept
        {
            return revision_;
        }

        // ---------------------------------------------------------- workspace가 부른다

        [[nodiscard]] std::uint64_t begin_refresh() noexcept;
        void begin_log_page() noexcept;
        void expect_commit_detail(operation_id id) noexcept;
        void expect_file_diff(operation_id id) noexcept;
        void expect_status(operation_id id) noexcept;
        void set_awaiting_input(std::optional<operation_id> id) noexcept;

        // 받아들였으면 true다. 늦게 온 결과면 false이고 상태는 그대로다.
        bool apply(const snapshot_loaded& event);
        bool apply(const commits_loaded& event);
        bool apply(const remote_tags_loaded& event);
        bool apply(operation_id id, const commit_detail_loaded& event);
        bool apply(operation_id id, const file_diff_loaded& event);
        // 작업 트리 변경만 새 값으로 바꾼다. 그래프 행은 건드리지 않는다. 가장 최근에 낸 `refresh_status`의 결과만 받는다.
        status_apply_result apply(operation_id id, const status_loaded& event);
        bool apply(const ancestry_answered& event);
        bool apply(const commit_count_answered& event);
        bool apply(const signatures_verified& event);
        // 이 generation의 log 쪽이 실패로 끝났다. 더 기다리지 않는다.
        void abandon_log_page(std::uint64_t generation) noexcept;

    private:
        void apply_known_remote_tags();

        std::string id_ {};
        std::u8string path_ {};
        repository_snapshot snapshot_ {};
        commit_graph_layout layout_ {};
        std::vector<graph_row> rows_ {};
        std::vector<commit_info> row_commits_ {};
        bool has_wip_row_ { false };
        std::uint64_t requested_generation_ { 0 };
        std::uint64_t displayed_generation_ { 0 };
        std::size_t loaded_commits_ { 0 };
        std::size_t batch_count_ { 0 };
        bool page_loading_ { false };
        bool history_complete_ { false };
        // 원격마다 마지막으로 읽은 태그 목록이다. snapshot이 바뀌어도 다시 입힌다.
        std::map<std::string, std::vector<remote_tag>> remote_tags_ {};
        std::optional<commit_detail> commit_detail_ {};
        std::optional<file_diff> file_diff_ {};
        // 마지막으로 요청한 상세·diff의 operation이다. 그보다 앞선 요청의 결과는 버린다.
        operation_id expected_detail_ { 0 };
        operation_id expected_diff_ { 0 };
        // 마지막으로 낸 `refresh_status`다. 전체 refresh를 내면 0으로 돌려 앞선 status의 결과를 버린다.
        operation_id expected_status_ { 0 };
        std::map<std::pair<std::string, std::string>, bool> ancestry_ {};
        std::map<std::pair<std::string, std::string>, std::uint32_t> commit_counts_ {};
        std::map<std::string, commit_signature> signatures_ {};
        // 검증을 요청한 커밋이다. 결과가 오기 전에 같은 커밋을 다시 요청하지 않는다.
        std::set<std::string> requested_signatures_ {};
        std::optional<operation_id> awaiting_input_ {};
        std::uint64_t revision_ { 0 };
    };
} // namespace cyella
