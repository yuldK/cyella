#include "presentation/shell_controller.h"

#include "presentation/graph_geometry.h"
#include "presentation/signing_intents.h"

#include <algorithm>
#include <cstddef>

// 커밋 서명의 화면 쪽 규칙이다: 커밋 상자의 서명 토글과, 보이는 커밋의 서명 검증 요청.
namespace cyella {
    namespace {
        // 보이는 행 앞뒤로 더 요청해 두는 행 수다. 한 칸씩 스크롤할 때마다 operation이 나가지 않게 여유를 두고 묶어 낸다.
        constexpr std::size_t rows_before_viewport { 8 };
        constexpr std::size_t rows_after_viewport { 24 };
        constexpr std::size_t maximum_rows_per_request { 40 };
        constexpr float assumed_viewport { 800.0f };
    } // namespace

    bool shell_controller::handle_signing_message(const luil::app_message& message)
    {
        if (message.get<toggle_sign_intent>() == nullptr)
            return false;
        tab_ui_state* const tab { selected_tab_state() };
        const repository_session* const session { selected_session() };
        if (tab != nullptr && session != nullptr)
            tab->sign = commit_sign_enabled(*tab, session->snapshot()) == false;
        return true;
    }

    std::u8string shell_controller::selected_repository_path() const
    {
        if (dependencies_.workspace == nullptr)
            return {};
        const repository_session* const session { dependencies_.workspace->find_session(selected_repository_id()) };
        return session == nullptr ? std::u8string {} : session->path();
    }

    void shell_controller::on_signing_changed(const bool global)
    {
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        std::vector<std::string> targets {};
        if (global)
            targets = workspace->repository_ids();
        else if (const std::string selected { selected_repository_id() }; selected.empty() == false)
            targets.push_back(selected);
        for (const std::string& id : targets)
        {
            if (repository_session* const session { workspace->find_session(id) }; session != nullptr)
                session->forget_signatures();
            // refresh가 설정의 실효 값을 snapshot에 다시 담는다. 설정을 바꾼 사실이 서명 상태 읽기에도 영향을 주므로 자동 operation으로 낸다.
            static_cast<void>(workspace->submit(id, refresh_request {}, true));
        }
    }

    bool shell_controller::commit_sign_enabled(const tab_ui_state& tab, const repository_snapshot& snapshot) const
    {
        return tab.sign.value_or(snapshot.signing.commit_signing());
    }

    std::optional<bool> shell_controller::commit_sign_request(const tab_ui_state& tab, const repository_snapshot& snapshot) const
    {
        if (commit_sign_enabled(tab, snapshot))
            return true;
        // 설정이 서명하도록 되어 있는데 사용자가 껐으면 설정을 덮는다. 설정을 따를 때는 아무것도 싣지 않는다.
        if (snapshot.signing.commit_signing())
            return false;
        return std::nullopt;
    }

    void shell_controller::request_signatures(const std::vector<std::string>& shas)
    {
        if (shas.empty())
            return;
        std::vector<std::string> wanted {};
        for (const std::string& sha : shas)
            if (sha.empty() == false && sha != graph_wip_sha)
                wanted.push_back(sha);
        if (wanted.empty())
            return;
        // 화면을 채우는 읽기이고 사용자가 기다리는 작업이 아니다. 자동 operation이라 상태 줄·작업 로그에 보이지 않는다.
        const std::string id { selected_repository_id() };
        if (dependencies_.workspace != nullptr && id.empty() == false)
            static_cast<void>(dependencies_.workspace->submit(id, verify_signatures_request { std::move(wanted) }, true));
    }

    void shell_controller::request_visible_signatures()
    {
        const repository_session* const session { selected_session() };
        const tab_ui_state* const tab { selected_tab_state() };
        if (session == nullptr || tab == nullptr || session->loaded() == false)
            return;
        const std::vector<commit_info>& commits { session->graph_commits() };
        if (commits.empty())
            return;
        const float viewport { feedback_.graph_viewport > 0.0f ? feedback_.graph_viewport : assumed_viewport };
        const auto first_visible { static_cast<std::size_t>(std::max(tab->graph_scroll, 0.0f) / graph_row_height) };
        const auto last_visible { static_cast<std::size_t>((std::max(tab->graph_scroll, 0.0f) + viewport) / graph_row_height) };
        const std::size_t first { first_visible > rows_before_viewport ? first_visible - rows_before_viewport : 0 };
        const std::size_t last { std::min(last_visible + rows_after_viewport, commits.size()) };

        std::vector<std::string> wanted {};
        for (std::size_t index { first }; index < last && wanted.size() < maximum_rows_per_request; ++index)
            if (commits[index].sha != graph_wip_sha && session->signature_requested(commits[index].sha) == false)
                wanted.push_back(commits[index].sha);
        request_signatures(wanted);
    }
} // namespace cyella
