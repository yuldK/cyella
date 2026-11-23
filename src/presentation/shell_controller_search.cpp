#include "presentation/shell_controller.h"

#include "domain/hangul_search.h"
#include "domain/text_catalog.h"
#include "presentation/graph_geometry.h"

#include <algorithm>
#include <utility>

// 그래프의 커밋 검색이다 (ADR-011).
//  - 검색 막대는 그래프 위에 서고, 맞는 커밋의 행을 그래프에서 칠한다. 그래프와 갈래는 그대로다.
//  - 읽은 커밋은 치는 즉시 여기서 훑는다 (제목·작성자·SHA 앞부분, 초성 포함). 그래프의 log는 본문을 싣지 않고 처음에 1만 개까지만
//    읽으므로, 본문과 아직 읽지 않은 이력은 git이 뒤에서 찾아(`search_commits_request`) 결과에 더한다.
//  - 앞·뒤 이동은 그 커밋을 고르고 보인다. 아직 읽지 않은 커밋이면 찾을 때까지 이력을 이어 읽는다 (`reveal_commit`).
namespace cyella {
    namespace {
        // 치기를 멈추고 이만큼 지나면 git에 묻는다. 글자마다 git을 띄우지 않는다.
        constexpr std::chrono::milliseconds search_debounce { 250 };
        // 한 글자 질의는 거의 모든 커밋에 맞는다. 읽은 커밋은 그래도 훑지만 git에는 두 글자부터 묻는다.
        constexpr std::size_t minimum_git_query_length { 2 };
        // SHA 앞부분으로 보는 최소 길이다. git의 줄인 해시 하한과 같다.
        constexpr std::size_t minimum_sha_prefix_length { 4 };

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string trimmed(const std::u8string_view text)
        {
            const auto blank = [](const char8_t value) { return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n'; };
            std::size_t begin { 0 };
            std::size_t end { text.size() };
            while (begin < end && blank(text[begin]))
                ++begin;
            while (end > begin && blank(text[end - 1]))
                --end;
            return std::u8string { text.substr(begin, end - begin) };
        }

        char fold_ascii(const char value) noexcept
        {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        }

        // 바이트로 보는 부분 일치다. ASCII만 대소문자를 접는다. UTF-8에서 한 글자의 바이트열은 다른 글자의 한가운데와 겹치지 않으므로
        // 한글 음절도 이것으로 맞는다. 초성 자모가 없는 질의는 풀어 쓰지 않고 이 길로 간다.
        bool contains_folded(const std::string_view text, const std::string_view folded_query) noexcept
        {
            if (folded_query.size() > text.size())
                return false;
            for (std::size_t start { 0 }; start + folded_query.size() <= text.size(); ++start)
            {
                std::size_t offset { 0 };
                while (offset < folded_query.size() && fold_ascii(text[start + offset]) == folded_query[offset])
                    ++offset;
                if (offset == folded_query.size())
                    return true;
            }
            return false;
        }

        // 읽은 커밋 하나가 질의에 맞는가를 정하는 값이다. 질의마다 한 번 만들어 커밋마다 쓴다.
        struct local_matcher
        {
            std::string folded {};
            // 초성 자모가 든 질의다. 그때만 글을 코드 포인트로 풀어 초성으로 맞춘다.
            std::u32string decoded {};
            bool choseong { false };
            // 질의가 SHA 앞부분일 수 있다 (16진수 넷 이상).
            bool sha_prefix { false };

            explicit local_matcher(const std::u8string_view query)
            {
                folded.reserve(query.size());
                sha_prefix = query.size() >= minimum_sha_prefix_length;
                for (const char8_t value : query)
                {
                    const char letter { fold_ascii(static_cast<char>(value)) };
                    folded.push_back(letter);
                    if (((letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f')) == false)
                        sha_prefix = false;
                }
                decoded = decode_utf8(query);
                choseong = std::ranges::any_of(decoded, [](const char32_t value) { return is_choseong_jamo(value); });
            }

            [[nodiscard]] bool text_matches(const std::string& text) const
            {
                if (choseong == false)
                    return contains_folded(text, folded);
                return contains_search_text(decoded, decode_utf8(std::u8string_view { reinterpret_cast<const char8_t*>(text.data()), text.size() }));
            }

            [[nodiscard]] bool matches(const commit_info& commit) const
            {
                if (sha_prefix && commit.sha.starts_with(folded))
                    return true;
                return text_matches(commit.subject) || text_matches(commit.author_name) || text_matches(commit.author_email);
            }
        };
    } // namespace

    bool shell_controller::handle_search_message(const luil::app_message& message)
    {
        if (message.get<open_graph_search_intent>() != nullptr)
        {
            tab_ui_state* const tab { selected_tab_state() };
            if (tab == nullptr || selected_session() == nullptr)
                return true;
            // 검색 막대는 그래프 위에 선다. 파일 판이 그래프를 덮고 있으면 닫아 그래프를 보인다.
            tab->diff.reset();
            if (tab->search.has_value() == false)
                tab->search = commit_search_state {};
            // 이미 열려 있어도 칸으로 초점을 돌린다 (Ctrl+F를 다시 누른 것이다).
            ui_.popup_focus = popup_focus_request { .id = ++popup_focus_revision_, .target = popup_focus_target::graph_search };
            return true;
        }
        if (message.get<close_graph_search_intent>() != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                close_graph_search(*tab);
            return true;
        }
        if (const auto* const intent { message.get<graph_search_step_intent>() }; intent != nullptr)
        {
            step_graph_search(intent->forward);
            return true;
        }
        return false;
    }

    void shell_controller::close_graph_search(tab_ui_state& tab)
    {
        if (tab.search.has_value() == false)
            return;
        // 돌고 있는 git 검색을 붙잡아 두지 않는다.
        if (git_workspace* const workspace { dependencies_.workspace }; workspace != nullptr && tab.search->operation != 0)
            if (const operation_status* const status { workspace->find_operation(tab.search->operation) }; status != nullptr && status->finished() == false)
                static_cast<void>(workspace->cancel(tab.search->operation));
        tab.search.reset();
    }

    void shell_controller::graph_search_changed(tab_ui_state& tab)
    {
        if (tab.search.has_value() == false)
            return;
        commit_search_state& search { *tab.search };
        // 질의가 바뀌면 "지금 자리"는 뜻이 없다. 다음 이동은 고른 커밋에서 다시 센다.
        search.current.clear();
        const std::u8string query { trimmed(search.query.text) };
        if (to_text(query) == search.submitted)
        {
            search.due.reset();
            return;
        }
        search.due = dependencies_.steady_clock() + search_debounce;
    }

    void shell_controller::submit_due_searches(const std::chrono::steady_clock::time_point now)
    {
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        for (auto& [id, tab] : ui_.tabs)
        {
            if (tab.search.has_value() == false || tab.search->due.has_value() == false || now < *tab.search->due)
                continue;
            commit_search_state& search { *tab.search };
            search.due.reset();
            const auto repository { tab_repositories_.find(id) };
            if (repository == tab_repositories_.end())
                continue;
            const std::u8string query { trimmed(search.query.text) };
            search.submitted = to_text(query);
            // 질의가 짧아졌으면 앞 질의의 git 검색은 쓸모없다.
            if (search.operation != 0)
                if (const operation_status* const status { workspace->find_operation(search.operation) }; status != nullptr && status->finished() == false)
                    static_cast<void>(workspace->cancel(search.operation));
            search.operation = 0;
            if (decode_utf8(query).size() >= minimum_git_query_length)
                search.operation = submit_to(repository->second, search_commits_request { .query = search.submitted });
        }
    }

    std::optional<std::chrono::steady_clock::time_point> shell_controller::search_deadline() const noexcept
    {
        std::optional<std::chrono::steady_clock::time_point> next {};
        for (const auto& [id, tab] : ui_.tabs)
            if (tab.search.has_value() && tab.search->due.has_value() && (next.has_value() == false || *tab.search->due < *next))
                next = tab.search->due;
        return next;
    }

    const commit_search_matches& shell_controller::graph_search_matches(const repository_tab_id tab_id, const tab_ui_state& tab, const repository_session& session)
    {
        commit_search_matches& cached { search_matches_[tab_id] };
        const std::u8string query { tab.search.has_value() ? trimmed(tab.search->query.text) : std::u8string {} };
        const std::optional<commit_search>& found { session.selected_search() };
        // git의 결과는 지금 질의의 것일 때만 쓴다. 치는 도중에는 앞 질의의 결과가 와 있을 수 있다.
        const bool git_ready { found.has_value() && query.empty() == false && found->request.query == to_text(query) };
        const operation_status* status { nullptr };
        if (tab.search.has_value() && tab.search->operation != 0 && dependencies_.workspace != nullptr)
            status = dependencies_.workspace->find_operation(tab.search->operation);
        const bool searching { tab.search.has_value() && query.empty() == false && (tab.search->due.has_value() || (status != nullptr && status->finished() == false)) };
        if (cached.hits != nullptr && cached.query == query && cached.revision == session.revision() && cached.git_ready == git_ready && cached.searching == searching)
            return cached;

        cached.query = query;
        cached.revision = session.revision();
        cached.git_ready = git_ready;
        cached.searching = searching;
        cached.truncated = git_ready && found->truncated;
        cached.ordered.clear();
        auto hits { std::make_shared<std::unordered_set<std::string>>() };
        if (query.empty() == false)
        {
            if (git_ready)
                for (const commit_info& commit : found->commits)
                    hits->insert(commit.sha);
            // 읽은 커밋은 그래프의 행 순서로 앞에 선다. git이 본문에서 찾은 읽은 커밋도 그 자리에 낀다.
            const local_matcher matcher { query };
            std::unordered_set<std::string> loaded {};
            for (const commit_info& commit : session.graph_commits())
            {
                if (commit.sha == graph_wip_sha)
                    continue;
                if (hits->contains(commit.sha) == false && matcher.matches(commit) == false)
                    continue;
                hits->insert(commit.sha);
                cached.ordered.push_back(commit.sha);
                loaded.insert(commit.sha);
            }
            // 아직 읽지 않은 커밋은 그 뒤에, git이 준 순서(새 것부터)로 선다.
            if (git_ready)
                for (const commit_info& commit : found->commits)
                    if (loaded.contains(commit.sha) == false)
                        cached.ordered.push_back(commit.sha);
        }
        cached.hits = std::move(hits);
        return cached;
    }

    void shell_controller::step_graph_search(const bool forward)
    {
        tab_ui_state* const tab { selected_tab_state() };
        const repository_session* const session { selected_session() };
        if (tab == nullptr || session == nullptr || tab->search.has_value() == false)
            return;
        const std::vector<std::string> ordered { graph_search_matches(tabs_.selected().id, *tab, *session).ordered };
        if (ordered.empty())
            return;
        // 지금 자리에서 한 칸이다. 자리가 없으면(질의를 막 쳤다) 고른 커밋에서, 그것도 결과에 없으면 처음(뒤로는 끝)에서 시작한다.
        auto at { std::ranges::find(ordered, tab->search->current) };
        if (at == ordered.end())
            at = std::ranges::find(ordered, tab->selected_sha);
        std::size_t index { forward ? std::size_t { 0 } : ordered.size() - 1 };
        if (at != ordered.end())
        {
            const auto position { static_cast<std::size_t>(at - ordered.begin()) };
            // 고른 커밋이 결과에 있어도 아직 "지금 자리"가 아니면 그 커밋이 첫 걸음이다.
            if (tab->search->current.empty())
                index = position;
            else
                index = forward ? (position + 1) % ordered.size() : (position + ordered.size() - 1) % ordered.size();
        }
        const std::string sha { ordered[index] };
        tab->search->current = sha;
        reveal_commit(sha);
    }

    void shell_controller::fill_graph_search(view_snapshot& view)
    {
        if (view.repository.has_value() == false)
            return;
        const repository_tab& selected { tabs_.selected() };
        const auto state { ui_.tabs.find(selected.id) };
        const repository_session* const session { selected_session() };
        if (state == ui_.tabs.end() || session == nullptr || state->second.search.has_value() == false)
            return;
        const tab_ui_state& tab { state->second };
        const commit_search_matches& matches { graph_search_matches(selected.id, tab, *session) };
        graph_search_view search {};
        search.input = luil::make_text_input_view(tab.search->query, ui_.composition, to_luil_target(text_target::graph_search));
        search.hits = matches.hits;
        search.current = tab.search->current;
        search.can_step = matches.ordered.empty() == false;
        const std::size_t count { matches.ordered.size() };
        if (matches.query.empty())
            search.status.clear();
        else if (count == 0)
            search.status = matches.searching ? tr(u8"graph_search.searching") : tr(u8"graph_search.none");
        else
        {
            // git이 상한에서 끊었으면 실제로는 더 있다. 수 뒤에 그렇다고 적는다.
            const auto at { std::ranges::find(matches.ordered, tab.search->current) };
            if (at != matches.ordered.end())
            {
                const std::size_t position { static_cast<std::size_t>(at - matches.ordered.begin()) + 1 };
                search.status = matches.truncated ? tr(u8"graph_search.position_more", { { u8"position", position }, { u8"count", count } })
                                                  : tr(u8"graph_search.position", { { u8"position", position }, { u8"count", count } });
            }
            else
                search.status = matches.truncated ? tr(u8"graph_search.count_more", { { u8"count", count } }) : tr(u8"graph_search.count", { { u8"count", count } });
        }
        view.repository->graph.search = std::move(search);
    }
} // namespace cyella
