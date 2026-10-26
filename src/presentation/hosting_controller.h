#pragma once

#include "application/git_operation.h"
#include "application/hosting_messages.h"
#include "domain/app_settings.h"
#include "domain/operation_status.h"
#include "presentation/hosting_intents.h"
#include "presentation/hosting_view.h"

#include "luil/ui/app_message.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cyella {
    // 컨트롤러가 저장소 탭에서 읽는 것이다. `shell_controller`가 세션(`repository_snapshot`)과 설정에서 짓는다.
    struct hosting_target
    {
        std::string repository_id {};
        // 원격 URL에서 읽은 호스팅 프로젝트다. 원격이 없거나 URL을 읽지 못했으면 값이 없다.
        std::optional<hosting_repository> repository {};
        std::string working_directory {};
        std::string current_branch {};
        bool detached { false };
        bool has_upstream { false };
        std::uint32_t ahead { 0 };
        std::string head_sha {};
        std::string head_subject {};
        // 원격 추적 브랜치 이름들이다 (`origin/main`). 비어 있으면 `dependencies::remote_branches_of`가 채운다.
        std::vector<std::string> remote_branches {};
        // 지금 고른 커밋이다 (WIP·없음이면 비어 있다).
        std::string selected_sha {};
    };

    // PR 만들기 대화 상자의 처음 값이다.
    struct create_pull_request_defaults
    {
        std::string target_branch {};
        std::string title {};
        std::string source_branch {};
        // 현재 브랜치가 원격에 없거나 앞서 있어 먼저 push해야 한다.
        bool needs_push { false };
        std::string push_remote {};
        // 열 수 없는 이유다. 비어 있으면 열 수 있다.
        std::u8string blocked_reason {};
    };

    // 원격이 없는 저장소를 호스팅 서버의 새 저장소로 게시하는 요청이다 (VS Code의 "GitHub에 게시"와 같은 흐름).
    struct publish_repository_request
    {
        // 소유자·이름이 빈 호스트 문맥이다 (`hosting_repository_for_host`).
        hosting_repository host {};
        create_repository_params params {};
        std::string remote_name { "origin" };
        // 원격 주소로 https 대신 ssh 주소를 쓴다.
        bool use_ssh { false };
        // 비어 있으면 원격만 더하고 push하지 않는다.
        std::string push_branch {};
    };

    // GitHub·GitLab 화면의 상태와 규칙이다. `shell_controller`가 소유하고 logic thread에서만 쓴다.
    //
    // 하는 일
    //  - 일감(`hosting_job`)을 내고 결과(`hosting_result_intent`)를 받아 저장소별 PR 목록·CI 캐시·로그인 상태를 갱신한다.
    //  - 로그인은 명시적이다: `request_sign_in`만 도우미에 묻는 일감을 낸다. 시작할 때나 탭을 열 때 조용히 부르지 않는다.
    //  - PR 체크아웃은 `fetch_request{refspec}` + `checkout_branch_request`를 `submit_git`으로 이어서 낸다.
    //  - PR 만들기는 필요하면 `push_execute`를 먼저 내고 그 operation이 성공해야 API를 부른다 (`after_events`가 지켜본다).
    class hosting_controller
    {
    public:
        struct dependencies
        {
            std::function<std::optional<hosting_target>(const std::string& repository_id)> target_of {};
            // 원격 추적 브랜치 이름들이다 (`origin/main`). PR 만들기 대화 상자의 대상 브랜치를 고를 때만 부른다 - 매 frame의 `target_of`에 refs 전체를 싣지 않으려는 것이다.
            // `hosting_target::remote_branches`가 이미 차 있으면(test) 그것을 쓴다.
            std::function<std::vector<std::string>(const std::string& repository_id)> remote_branches_of {};
            std::function<std::string()> selected_repository_id {};
            std::function<void(hosting_job)> submit_job {};
            std::function<void(const hosting_sign_out_request&)> sign_out {};
            std::function<std::int64_t()> now {};
            std::function<void(std::u8string text, bool warning)> notice {};
            std::function<void(const std::string& url)> open_url {};
            // 앞이 성공해야 이어서 내는 operation 열을 낸다. 첫 operation의 id를 돌려준다 (0이면 접수하지 못했다).
            std::function<operation_id(const std::string& repository_id, std::vector<git_operation> operations)> submit_git {};
            std::function<const operation_status*(operation_id id)> find_operation {};
        };

        explicit hosting_controller(dependencies dependencies);

        // 화면 메시지면 처리하고 true다 (결과, 상세 단추, 로그인·로그아웃).
        bool handle(const luil::app_message& message);
        // 사이드바 PULL REQUESTS 행의 동작이다 (`hosting_row_*` 또는 번호).
        void activate_row(const std::string& repository_id, const std::string& path);
        // 끝난 operation의 후속이다 (push 뒤 PR 만들기).
        void after_events();

        // 커밋을 골랐다. 로그인한 호스트면 그 커밋의 CI 상태를 읽는다 (조용한 로그인은 하지 않는다).
        void on_commit_selected(const std::string& repository_id, const std::string& sha);
        // 열려 있는 PR 상세를 닫는다 (커밋·WIP를 고르면 커밋 상세가 이어받는다).
        void clear_selection(const std::string& repository_id);
        void on_repository_closed(const std::string& repository_id);

        // 한 저장소 탭의 화면 값이다. `locked_branch`는 지금 체크아웃한 브랜치다.
        [[nodiscard]] hosting_repository_view view_of(const std::string& repository_id) const;
        // 커밋 상세에 붙는 CI 표시다. 로그인하지 않았거나 서비스를 모르면 값이 없다.
        [[nodiscard]] std::optional<ci_mark_view> ci_mark(const std::string& repository_id, const std::string& sha) const;
        // 환경 설정 화면의 호스트 표다. 설정의 표에 내장 호스트와 로그인한 호스트를 더한다.
        [[nodiscard]] std::vector<hosting_host_row_view> host_rows(const std::vector<hosting_host_setting>& configured) const;
        [[nodiscard]] bool has_pull_request_detail(const std::string& repository_id) const;

        // PR 만들기 대화 상자가 쓴다.
        [[nodiscard]] std::optional<create_pull_request_defaults> create_defaults(const std::string& repository_id) const;
        // 대화 상자의 [만들기]다. `push_first`면 현재 브랜치를 먼저 push한다.
        void submit_create(const std::string& repository_id, create_pull_request_params params, bool push_first);

        void request_sign_in(const std::string& repository_id);
        void sign_out(const std::string& host_key);

        // 게시: 로그인하지 않은 호스트면 먼저 로그인하고(사용자가 게시를 고른 것이 로그인의 명시적 요청이다), 저장소를 만든 뒤
        // `remote add` → `push --set-upstream`을 `submit_git`으로 잇는다. 같은 저장소의 게시가 진행 중이면 무시한다.
        void request_publish(const std::string& repository_id, publish_repository_request request);
        [[nodiscard]] bool publishing(const std::string& repository_id) const;

    private:
        struct host_session
        {
            bool signed_in { false };
            bool signing_in { false };
            std::string user {};
            std::string error_text {};
            rate_limit_info rate_limit {};
        };

        struct ci_entry
        {
            bool loading { false };
            bool has_status { false };
            ci_status_info status {};
            std::string error_text {};
            std::int64_t at { 0 };
        };

        struct created_info
        {
            std::uint64_t number { 0 };
            std::string url {};
        };

        struct repository_state
        {
            std::string host_key {};
            bool include_closed { false };
            bool loading { false };
            bool loaded { false };
            std::shared_ptr<const std::vector<pull_request_info>> pulls { std::make_shared<const std::vector<pull_request_info>>() };
            std::optional<std::uint32_t> next_page {};
            std::string error_text {};
            std::optional<std::uint64_t> selected {};
            // 선택한 PR의 마지막 모습이다. 목록이 바뀌어 사라져도 상세는 남는다 (방금 만든 PR, 닫힌 PR 숨기기).
            std::optional<pull_request_info> selected_copy {};
            std::optional<created_info> created {};
            std::uint64_t list_serial { 0 };
            std::map<std::string, ci_entry> ci {};
        };

        struct pending_job
        {
            enum class kind : std::uint8_t
            {
                sign_in,
                list,
                create,
                ci,
                create_repository,
            };

            kind what { kind::list };
            std::string repository_id {};
            std::string host_key {};
            std::string sha {};
            std::uint32_t page { 1 };
            bool append { false };
        };

        struct pending_create
        {
            std::string repository_id {};
            create_pull_request_params params {};
        };

        [[nodiscard]] host_session& session_of(const std::string& host_key);
        [[nodiscard]] const host_session* find_session(const std::string& host_key) const;
        [[nodiscard]] repository_state& state_of(const std::string& repository_id);
        [[nodiscard]] const repository_state* find_state(const std::string& repository_id) const;
        // 호스팅 프로젝트가 있고 서비스를 아는 대상이다. 아니면 이유를 알리고 값이 없다.
        [[nodiscard]] std::optional<hosting_target> supported_target(const std::string& repository_id, bool announce) const;
        // PR 만들기를 막는 이유다. 비어 있으면 만들 수 있다.
        [[nodiscard]] std::u8string create_blocked_reason(const hosting_target& target) const;
        [[nodiscard]] hosting_job make_job(const hosting_target& target, std::variant<hosting_sign_in_job, hosting_list_job, hosting_create_job, hosting_ci_job, hosting_create_repository_job> work);

        void request_list(const std::string& repository_id, std::uint32_t page, bool append);
        void request_ci(const std::string& repository_id, const std::string& sha, bool force);
        void send_create(const std::string& repository_id, const create_pull_request_params& params);
        void select_pull_request(const std::string& repository_id, std::uint64_t number);
        void checkout_pull_request(const std::string& repository_id);
        void open_in_browser(const std::string& repository_id);
        void apply_result(const hosting_result_intent& result);
        void apply_sign_in(const hosting_result_intent& result, const pending_job& job);
        void apply_list(const hosting_result_intent& result, const pending_job& job);
        void apply_create(const hosting_result_intent& result, const pending_job& job);
        void apply_ci(const hosting_result_intent& result, const pending_job& job);
        void apply_create_repository(const hosting_result_intent& result, const pending_job& job);
        void send_create_repository(const std::string& repository_id);
        // 게시 일감의 대상이다: 저장소 탭의 작업 폴더에 호스트 문맥을 얹는다.
        [[nodiscard]] hosting_target publish_target(const std::string& repository_id) const;
        void update_host(const hosting_result_intent& result);
        [[nodiscard]] static bool is_stale_ci(const ci_entry& entry, std::int64_t now) noexcept;
        [[nodiscard]] ci_mark_view mark_of(const hosting_repository& repository, const ci_entry* entry) const;
        [[nodiscard]] std::u8string rate_limit_text(const rate_limit_info& info) const;
        [[nodiscard]] static std::string local_branch_for(const hosting_repository& repository, std::uint64_t number);
        [[nodiscard]] static std::string refspec_for(const hosting_repository& repository, std::uint64_t number);
        [[nodiscard]] const pull_request_info* find_pull_request(const repository_state& state, std::uint64_t number) const;

        dependencies dependencies_ {};
        std::map<std::string, host_session> hosts_ {};
        std::map<std::string, repository_state> repositories_ {};
        std::map<std::uint64_t, pending_job> pending_ {};
        std::map<operation_id, pending_create> pending_creates_ {};
        // 저장소 id별로 진행 중인 게시다. 로그인 결과와 저장소 만들기 결과가 이것을 이어 간다.
        std::map<std::string, publish_repository_request> publishes_ {};
        // 저장소 만들기를 이미 보낸 게시다.
        std::set<std::string> creating_ {};
        std::uint64_t next_serial_ { 1 };
    };
} // namespace cyella
