#include "presentation/shell_controller.h"

#include "domain/text_catalog.h"
#include "presentation/graph_geometry.h"

#include <algorithm>
#include <utility>

// diff 판의 blame·파일 이력 보기다 (ADR-010).
//  - 판은 늘 파일 하나의 diff(`tab_ui_state::diff`)로 서고, 머리의 전환이 그 파일의 blame과 이력으로 넘긴다. 그 diff가 "변경 내용"으로
//    돌아갈 자리라 file_view는 diff가 있는 동안만 선다.
//  - blame과 이력은 그 모드에 처음 들어갈 때 읽는다. 둘 다 읽기 operation이고 새 요청이 앞 요청을 취소한다 (`git_workspace`).
//  - 이력에서 고른 커밋의 변경 내용은 판 오른쪽의 웹뷰가 보인다. 그 요청(`history_diff`)은 판의 diff와 따로 둔다.
namespace cyella {
    namespace {
        // 로드된 그래프에 없는 커밋을 보이려 할 때, 화면 높이를 아직 모르면 쓰는 값이다.
        constexpr float assumed_reveal_viewport { 800.0f };

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string short_sha(const std::string_view sha)
        {
            const std::string_view head { sha.substr(0, 7) };
            return std::u8string { head.begin(), head.end() };
        }

        // 같은 파일의 같은 변경 내용인가. 문맥 줄 수는 설정이 채우는 값이라 보지 않는다.
        bool same_file_diff(const diff_file_request& left, const diff_file_request& right) noexcept
        {
            return left.path == right.path && left.view == right.view && left.sha == right.sha && left.base == right.base && left.untracked == right.untracked;
        }

        // 그 변경 내용 요청의 파일을 blame·이력으로 볼 리비전이다. 비면 작업 트리(blame)·HEAD(이력)다.
        std::string revision_of(const diff_file_request& diff, const repository_session& session)
        {
            switch (diff.view)
            {
            case diff_view::unstaged:
            case diff_view::staged:
                return {};
            case diff_view::commit: {
                // 그 커밋이 지운 파일은 그 커밋의 트리에 없다. 지우기 직전 판을 본다.
                const std::optional<commit_detail>& detail { session.selected_commit() };
                if (detail.has_value() && detail->commit.sha == diff.sha)
                    for (const file_change& file : detail->files)
                        if (file.path == diff.path && file.status == 'D')
                            return diff.sha + "^";
                return diff.sha;
            }
            case diff_view::range:
                return diff.sha;
            }
            return {};
        }
    } // namespace

    bool shell_controller::handle_file_view_message(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<set_file_view_mode_intent>() }; intent != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                set_file_view_mode(*tab, intent->mode);
            return true;
        }
        if (const auto* const intent { message.get<open_file_view_intent>() }; intent != nullptr)
        {
            tab_ui_state* const tab { selected_tab_state() };
            if (tab == nullptr)
                return true;
            // 판이 그 파일의 변경 내용으로 먼저 선다. 이미 그 파일을 보고 있으면 다시 읽지 않는다.
            if (tab->diff.has_value() == false || same_file_diff(*tab->diff, intent->diff) == false)
            {
                // 보던 보기를 새 파일로 가져가지 않는다. 가져가면 이력을 열려는데 blame부터 읽는다.
                close_file_view(*tab);
                static_cast<void>(handle(luil::app_message { select_file_intent { intent->diff } }));
            }
            set_file_view_mode(*tab, intent->mode);
            return true;
        }
        if (const auto* const intent { message.get<select_file_history_intent>() }; intent != nullptr)
        {
            tab_ui_state* const tab { selected_tab_state() };
            const repository_session* const session { selected_session() };
            if (tab == nullptr || session == nullptr || tab->file_view.has_value() == false || session->selected_history().has_value() == false)
                return true;
            const std::string key { to_text(intent->key) };
            for (const file_history_entry& entry : session->selected_history()->entries)
            {
                if (entry.commit.sha.compare(0, key.size(), key) != 0)
                    continue;
                select_history_entry(*tab, entry);
                break;
            }
            return true;
        }
        if (const auto* const intent { message.get<reveal_commit_intent>() }; intent != nullptr)
        {
            // 웹뷰의 동작은 보이는 blame의 저장소로 온다. 그 사이 탭을 옮겼으면 다른 저장소의 그래프를 건드리지 않는다.
            if (intent->repository_id.empty() || intent->repository_id == selected_repository_id())
                reveal_commit(intent->sha);
            return true;
        }
        if (const auto* const intent { message.get<open_blame_revision_intent>() }; intent != nullptr)
        {
            tab_ui_state* const tab { selected_tab_state() };
            if (tab == nullptr || tab->file_view.has_value() == false || tab->file_view->mode != file_view_mode::blame)
                return true;
            if (intent->repository_id.empty() == false && intent->repository_id != selected_repository_id())
                return true;
            file_view_state& view { *tab->file_view };
            if (view.blame == intent->request)
                return true;
            view.blame_back.push_back(view.blame);
            view.blame = intent->request;
            view.blame_focus.clear();
            view.blame_operation = submit(view.blame);
            return true;
        }
        if (message.get<blame_back_intent>() != nullptr)
        {
            tab_ui_state* const tab { selected_tab_state() };
            if (tab == nullptr || tab->file_view.has_value() == false || tab->file_view->blame_back.empty())
                return true;
            file_view_state& view { *tab->file_view };
            view.blame = std::move(view.blame_back.back());
            view.blame_back.pop_back();
            view.blame_focus.clear();
            view.blame_operation = submit(view.blame);
            return true;
        }
        return false;
    }

    void shell_controller::set_file_view_mode(tab_ui_state& tab, const file_view_mode mode)
    {
        const repository_session* const session { selected_session() };
        if (tab.diff.has_value() == false || session == nullptr)
            return;
        if (mode == file_view_mode::changes)
        {
            if (tab.file_view.has_value() == false)
                return;
            // 이력이 다른 커밋의 diff를 읽어 두었으면 세션의 마지막 diff가 판의 diff가 아니다. 다시 읽는다.
            const bool reload { tab.file_view->history_diff.has_value() };
            close_file_view(tab);
            if (reload)
                static_cast<void>(submit(*tab.diff));
            return;
        }
        // 추적하지 않는 파일에는 이력이 없고 git blame도 실패한다. 전환 단추가 꺼져 있지만 메뉴·팔레트로도 올 수 있다.
        if (tab.diff->untracked)
        {
            show_notice(tr(u8"shell.file_view.untracked"));
            return;
        }
        if (tab.file_view.has_value() == false)
        {
            const std::string revision { revision_of(*tab.diff, *session) };
            tab.file_view = file_view_state {};
            tab.file_view->blame = blame_file_request { .path = tab.diff->path, .revision = revision };
            tab.file_view->history = file_history_request { .path = tab.diff->path, .revision = revision };
        }
        file_view_state& view { *tab.file_view };
        const file_view_mode before { view.mode };
        view.mode = mode;
        if (mode == file_view_mode::blame)
        {
            // 이력에서 고른 커밋이 있으면 그 커밋의 줄을 고른 채로 연다. 작업 트리·더 새 판의 blame에 그 커밋의 줄이 남아 있을 때만 보인다.
            if (before == file_view_mode::history)
                view.blame_focus = view.history_selected;
            if (view.blame_operation == 0)
                view.blame_operation = submit(view.blame);
            return;
        }
        if (view.history_operation == 0)
            view.history_operation = submit(view.history);
        else if (view.history_diff.has_value())
            static_cast<void>(submit(*view.history_diff));
    }

    void shell_controller::close_file_view(tab_ui_state& tab)
    {
        if (tab.file_view.has_value() == false)
            return;
        // 끝나지 않은 blame·이력 읽기를 붙잡아 두지 않는다. 큰 저장소의 blame은 읽기 worker를 오래 쥔다.
        if (git_workspace* const workspace { dependencies_.workspace }; workspace != nullptr)
        {
            for (const operation_id id : { tab.file_view->blame_operation, tab.file_view->history_operation })
                if (const operation_status* const status { workspace->find_operation(id) }; id != 0 && status != nullptr && status->finished() == false)
                    static_cast<void>(workspace->cancel(id));
        }
        tab.file_view.reset();
    }

    void shell_controller::select_history_entry(tab_ui_state& tab, const file_history_entry& entry)
    {
        if (tab.file_view.has_value() == false)
            return;
        file_view_state& view { *tab.file_view };
        if (view.history_selected == entry.commit.sha && view.history_diff.has_value())
            return;
        view.history_selected = entry.commit.sha;
        // 그 커밋에서의 경로로 읽는다. 이름이 바뀌기 전의 커밋은 옛 경로에 있다.
        view.history_diff = diff_file_request { .path = entry.path, .view = diff_view::commit, .sha = entry.commit.sha, .context_lines = settings().diff.context_lines };
        tab.diff_scroll = 0.0f;
        static_cast<void>(submit(*view.history_diff));
    }

    void shell_controller::settle_file_views()
    {
        const git_workspace* const workspace { dependencies_.workspace };
        for (auto& [id, tab] : ui_.tabs)
        {
            // 판을 닫는 길은 여럿이다 (커밋 고르기, Esc, 보기 전환). 어느 길로 닫혔든 여기서 따라 닫는다.
            if (tab.diff.has_value() == false)
            {
                close_file_view(tab);
                continue;
            }
            if (tab.file_view.has_value() == false || workspace == nullptr)
                continue;
            file_view_state& view { *tab.file_view };
            if (view.mode != file_view_mode::history || view.history_selected.empty() == false)
                continue;
            const auto repository { tab_repositories_.find(id) };
            if (repository == tab_repositories_.end() || repository->second != selected_repository_id())
                continue;
            // 이력이 오면 가장 새 커밋을 골라 둔다. 목록만 서고 오른쪽이 비어 있으면 무엇을 눌러야 하는지 한 번 더 생각하게 한다.
            const repository_session* const session { workspace->find_session(repository->second) };
            if (session == nullptr || session->selected_history().has_value() == false || session->selected_history()->request != view.history
                || session->selected_history()->entries.empty())
                continue;
            select_history_entry(tab, session->selected_history()->entries.front());
        }
        settle_reveal();
    }

    std::optional<blame_target> shell_controller::current_blame_target() const
    {
        const repository_tab& tab { tabs_.selected() };
        if (tab.kind != repository_tab_kind::repository)
            return std::nullopt;
        const auto state { ui_.tabs.find(tab.id) };
        const auto repository { tab_repositories_.find(tab.id) };
        if (state == ui_.tabs.end() || repository == tab_repositories_.end() || state->second.diff.has_value() == false || state->second.file_view.has_value() == false
            || state->second.file_view->mode != file_view_mode::blame)
            return std::nullopt;
        return blame_target { repository->second, state->second.file_view->blame, state->second.file_view->blame_focus };
    }

    void shell_controller::reveal_commit(const std::string& sha)
    {
        tab_ui_state* const tab { selected_tab_state() };
        if (tab == nullptr || sha.empty())
            return;
        tab->pending_reveal = sha;
        tab->reveal_operation = 0;
        if (settle_reveal() == false)
            show_notice(tr(u8"shell.file_view.searching_commit", { { u8"sha", short_sha(sha) } }));
    }

    bool shell_controller::settle_reveal()
    {
        tab_ui_state* const tab { selected_tab_state() };
        repository_session* const session { selected_session() };
        if (tab == nullptr || session == nullptr || tab->pending_reveal.empty() || session->loaded() == false)
            return tab == nullptr || tab->pending_reveal.empty();
        const std::string sha { tab->pending_reveal };
        const std::vector<commit_info>& commits { session->graph_commits() };
        const auto found { std::find_if(commits.begin(), commits.end(), [&sha](const commit_info& commit) { return commit.sha == sha; }) };
        if (found == commits.end())
        {
            // 아직 읽지 않은 쪽에 있을 수 있다. 찾을 때까지 한 쪽씩 이어 읽는다 — 쪽이 올 때마다 `after_events`가 다시 온다.
            if (session->history_complete())
            {
                tab->pending_reveal.clear();
                tab->reveal_operation = 0;
                show_notice(tr(u8"shell.file_view.commit_not_in_graph", { { u8"sha", short_sha(sha) } }), true);
                return true;
            }
            // 내가 낸 읽기가 끝나지 못했으면(실패·취소) 그만둔다. 이 함수는 메시지마다 불리므로 다시 내면 같은 실패를 되풀이한다.
            if (tab->reveal_operation != 0)
            {
                const operation_status* const status { dependencies_.workspace != nullptr ? dependencies_.workspace->find_operation(tab->reveal_operation) : nullptr };
                if (status != nullptr && status->finished() == false)
                    return false;
                const bool read { status != nullptr && status->state == operation_state::succeeded };
                tab->reveal_operation = 0;
                if (read == false)
                {
                    tab->pending_reveal.clear();
                    show_notice(tr(u8"shell.file_view.reveal_failed", { { u8"sha", short_sha(sha) } }), true);
                    return true;
                }
            }
            // 다른 길(스크롤)이 낸 쪽 읽기가 돌고 있으면 그 결과를 기다린다.
            if (session->log_page_loading())
                return false;
            tab->reveal_operation = submit(load_more_log_request {});
            if (tab->reveal_operation == 0)
            {
                tab->pending_reveal.clear();
                show_notice(tr(u8"shell.file_view.reveal_failed", { { u8"sha", short_sha(sha) } }), true);
                return true;
            }
            return false;
        }
        tab->pending_reveal.clear();
        tab->reveal_operation = 0;
        // 고르면 판이 닫히고 상세 패널이 그 커밋으로 바뀐다. 그래프는 그 행이 창의 위쪽 3분의 1쯤에 오게 흘린다.
        // 이미 고른 커밋이면 고르기가 아무것도 하지 않으므로 판은 여기서 닫는다.
        tab->diff.reset();
        close_file_view(*tab);
        handle_select_commit(select_commit_intent { graph_item_key(sha) });
        const float viewport { feedback_.graph_viewport > 0.0f ? feedback_.graph_viewport : assumed_reveal_viewport };
        const float row { static_cast<float>(found - commits.begin()) * graph_row_height };
        tab->graph_scroll = std::max(0.0f, row - viewport / 3.0f);
        return true;
    }
} // namespace cyella
