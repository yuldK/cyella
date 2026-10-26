// GitHub·GitLab 화면의 controller·사이드바·대화 상자 규칙이다. 네트워크도 git도 없이 가짜 일감 수신기와 가짜 operation 표로 돈다.
#include "presentation/form_hosting.h"
#include "presentation/hosting_controller.h"
#include "presentation/hosting_sidebar.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {
    using namespace cyella;

    constexpr std::int64_t base_time { 1700000000 };

    struct harness
    {
        std::map<std::string, hosting_target> targets {};
        std::vector<hosting_job> jobs {};
        std::vector<hosting_sign_out_request> sign_outs {};
        std::vector<std::pair<std::u8string, bool>> notices {};
        std::vector<std::string> opened_urls {};
        std::vector<std::pair<std::string, std::vector<git_operation>>> git_submissions {};
        std::map<operation_id, operation_status> operations {};
        operation_id next_operation { 100 };
        std::int64_t now { base_time };
        std::string selected { "repo-1" };
        hosting_controller controller;

        harness()
            : controller { make_dependencies() }
        {
            hosting_target target {};
            target.repository_id = "repo-1";
            target.repository = resolve_hosting_repository("git@github.com:octo/hello.git", "origin", {});
            target.working_directory = "C:\\work\\hello";
            target.current_branch = "feature/x";
            target.has_upstream = false;
            target.head_sha = std::string(40, 'a');
            target.head_subject = "Add the feature";
            target.remote_branches = { "origin/main", "origin/develop", "origin/HEAD" };
            targets["repo-1"] = target;
        }

        hosting_controller::dependencies make_dependencies()
        {
            hosting_controller::dependencies result {};
            result.target_of = [this](const std::string& id) -> std::optional<hosting_target> {
                const auto found { targets.find(id) };
                return found == targets.end() ? std::nullopt : std::optional<hosting_target> { found->second };
            };
            result.selected_repository_id = [this] { return selected; };
            result.submit_job = [this](hosting_job job) { jobs.push_back(std::move(job)); };
            result.sign_out = [this](const hosting_sign_out_request& request) { sign_outs.push_back(request); };
            result.now = [this] { return now; };
            result.notice = [this](std::u8string text, const bool warning) { notices.emplace_back(std::move(text), warning); };
            result.open_url = [this](const std::string& url) { opened_urls.push_back(url); };
            result.submit_git = [this](const std::string& id, std::vector<git_operation> operations_in) {
                const operation_id operation { next_operation++ };
                operation_status status {};
                status.id = operation;
                status.state = operation_state::running;
                operations[operation] = status;
                git_submissions.emplace_back(id, std::move(operations_in));
                return operation;
            };
            result.find_operation = [this](const operation_id id) -> const operation_status* {
                const auto found { operations.find(id) };
                return found == operations.end() ? nullptr : &found->second;
            };
            return result;
        }

        hosting_target& target()
        {
            return targets["repo-1"];
        }

        void deliver(const hosting_job& job, hosting_payload payload, const bool signed_in = true, const std::string& user = "octocat")
        {
            hosting_result_intent result {};
            result.serial = job.serial;
            result.repository_id = job.repository_id;
            result.host_key = job.context.repository.host_key;
            result.payload = std::move(payload);
            result.signed_in = signed_in;
            result.user = user;
            result.at = now;
            REQUIRE(controller.handle(luil::app_message { result }));
        }

        void sign_in()
        {
            controller.request_sign_in("repo-1");
            REQUIRE(jobs.size() >= 1);
            deliver(jobs.back(), hosting_result<std::string> { .value = std::string { "octocat" } });
        }

        // 마지막 일감에 목록 쪽을 답한다.
        void deliver_list(const std::vector<pull_request_info>& items, const std::optional<std::uint32_t> next = std::nullopt)
        {
            const hosting_job job { jobs.back() };
            REQUIRE(std::holds_alternative<hosting_list_job>(job.work));
            pull_request_page page {};
            page.items = items;
            page.page = std::get<hosting_list_job>(job.work).query.page;
            page.next_page = next;
            deliver(job, hosting_result<pull_request_page> { .value = std::move(page) });
        }
    };

    pull_request_info make_pull(const std::uint64_t number, std::string title, const pull_request_state state = pull_request_state::open)
    {
        pull_request_info info {};
        info.number = number;
        info.title = std::move(title);
        info.author = "octocat";
        info.source_branch = "feature/" + std::to_string(number);
        info.target_branch = "main";
        info.state = state;
        info.head_sha = std::string(40, static_cast<char>('a' + number % 6));
        info.web_url = "https://github.com/octo/hello/pull/" + std::to_string(number);
        info.body = "line one\nline two";
        info.updated_at = base_time - 3600;
        return info;
    }

    const sidebar_row_view* find_row(const sidebar_rows& rows, const std::u8string& key)
    {
        const auto found { std::ranges::find_if(rows.rows, [&key](const sidebar_row_view& row) { return row.key == key; }) };
        return found == rows.rows.end() ? nullptr : &*found;
    }

    sidebar_rows rows_of(const hosting_repository_view& view, const std::u8string_view filter = {}, const std::uint8_t collapsed = 0)
    {
        sidebar_rows rows {};
        append_hosting_sidebar_rows(rows, view, filter, collapsed);
        return rows;
    }
} // namespace

TEST_CASE("without an explicit sign-in no hosting job is ever submitted", "[hosting][controller]")
{
    harness test {};
    static_cast<void>(test.controller.view_of("repo-1"));
    test.controller.on_commit_selected("repo-1", std::string(40, 'a'));
    test.controller.after_events();
    test.controller.activate_row("repo-1", hosting_row_refresh);
    test.controller.activate_row("repo-1", hosting_row_toggle_closed);
    test.controller.activate_row("repo-1", "12");
    CHECK(test.jobs.empty());
    CHECK(test.controller.ci_mark("repo-1", std::string(40, 'a')).has_value() == false);
    const hosting_repository_view view { test.controller.view_of("repo-1") };
    CHECK(view.support == hosting_support::supported);
    CHECK(view.signed_in == false);
    const sidebar_rows rows { rows_of(view) };
    REQUIRE(find_row(rows, u8"pr:login") != nullptr);
    CHECK(find_row(rows, u8"pr:login")->path == hosting_row_login);
    CHECK(find_row(rows, u8"pr:n:1") == nullptr);
}

TEST_CASE("repositories without a usable remote show an explanatory row", "[hosting][controller]")
{
    harness test {};
    test.targets["repo-2"] = hosting_target { .repository_id = "repo-2" };
    CHECK(test.controller.view_of("repo-2").support == hosting_support::no_remote);
    CHECK(test.controller.view_of("missing").support == hosting_support::no_remote);
    test.targets["repo-3"] = hosting_target { .repository_id = "repo-3", .repository = resolve_hosting_repository("https://git.example.com/a/b.git", "origin", {}) };
    const hosting_repository_view unsupported { test.controller.view_of("repo-3") };
    CHECK(unsupported.support == hosting_support::unsupported);

    const sidebar_rows rows { rows_of(unsupported) };
    REQUIRE(find_row(rows, u8"pr:info") != nullptr);
    CHECK(find_row(rows, u8"pr:info")->path.empty());
    CHECK(rows.rows.front().header);
    CHECK(rows.rows.front().label == u8"PULL REQUESTS");
    // 로그인을 눌러도 일감은 나가지 않고 이유를 알린다.
    test.controller.request_sign_in("repo-3");
    CHECK(test.jobs.empty());
    REQUIRE(test.notices.empty() == false);
    CHECK(test.notices.back().second);
}

TEST_CASE("signing in submits one job and then loads the pull request list", "[hosting][controller]")
{
    harness test {};
    test.controller.request_sign_in("repo-1");
    REQUIRE(test.jobs.size() == 1);
    CHECK(std::holds_alternative<hosting_sign_in_job>(test.jobs[0].work));
    CHECK(test.jobs[0].context.repository.host_key == "github.com");
    CHECK(test.jobs[0].context.working_directory == "C:\\work\\hello");
    CHECK(test.controller.view_of("repo-1").signing_in);
    // 로그인 중에 또 눌러도 겹치지 않는다.
    test.controller.request_sign_in("repo-1");
    CHECK(test.jobs.size() == 1);

    test.deliver(test.jobs[0], hosting_result<std::string> { .value = std::string { "octocat" } });
    REQUIRE(test.jobs.size() == 2);
    REQUIRE(std::holds_alternative<hosting_list_job>(test.jobs[1].work));
    CHECK(std::get<hosting_list_job>(test.jobs[1].work).query.include_closed == false);
    CHECK(std::get<hosting_list_job>(test.jobs[1].work).query.page == 1);
    const hosting_repository_view view { test.controller.view_of("repo-1") };
    CHECK(view.signed_in);
    CHECK(view.signing_in == false);
    CHECK(view.user == u8"octocat");
    CHECK(view.loading);

    test.deliver_list({ make_pull(12, "Add feature"), make_pull(11, "Fix bug") });
    const hosting_repository_view loaded { test.controller.view_of("repo-1") };
    CHECK(loaded.loading == false);
    CHECK(loaded.loaded);
    REQUIRE(loaded.pulls->size() == 2);
    const sidebar_rows rows { rows_of(loaded) };
    REQUIRE(find_row(rows, u8"pr:n:12") != nullptr);
    CHECK(find_row(rows, u8"pr:n:12")->label == u8"#12 Add feature");
    CHECK(find_row(rows, u8"pr:n:12")->path == "12");
    CHECK(find_row(rows, u8"pr:n:12")->detail == u8"feature/12 → main");
    CHECK(find_row(rows, u8"pr:login") == nullptr);
    CHECK(rows.counts[static_cast<std::size_t>(sidebar_section::pull_requests)] == 2);
    CHECK(rows.rows.front().count == 2);
    REQUIRE(find_row(rows, u8"pr:create") != nullptr);
    REQUIRE(find_row(rows, u8"pr:refresh") != nullptr);
    CHECK(find_row(rows, u8"pr:closed")->badge == u8"끔");
}

TEST_CASE("a failed sign-in is reported and leaves the repository signed out", "[hosting][controller]")
{
    harness test {};
    test.controller.request_sign_in("repo-1");
    hosting_result<std::string> failed {};
    failed.error.kind = hosting_error_kind::credential_unavailable;
    failed.error.message = "no helper";
    test.deliver(test.jobs[0], failed, false, "");
    CHECK(test.jobs.size() == 1);
    const hosting_repository_view view { test.controller.view_of("repo-1") };
    CHECK(view.signed_in == false);
    CHECK(view.signing_in == false);
    REQUIRE(test.notices.empty() == false);
    CHECK(test.notices.back().second);
    // 다시 로그인할 수 있다.
    test.controller.request_sign_in("repo-1");
    CHECK(test.jobs.size() == 2);
}

TEST_CASE("the closed toggle reloads and a late list result is ignored", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    test.deliver_list({ make_pull(1, "one") });
    test.controller.activate_row("repo-1", hosting_row_refresh);
    const hosting_job stale { test.jobs.back() };
    test.controller.activate_row("repo-1", hosting_row_toggle_closed);
    REQUIRE(std::holds_alternative<hosting_list_job>(test.jobs.back().work));
    CHECK(std::get<hosting_list_job>(test.jobs.back().work).query.include_closed);
    CHECK(test.controller.view_of("repo-1").include_closed);

    // 먼저 낸 요청의 늦은 답은 버려진다.
    pull_request_page old_page {};
    old_page.items = { make_pull(99, "stale") };
    test.deliver(stale, hosting_result<pull_request_page> { .value = std::move(old_page) });
    CHECK(test.controller.view_of("repo-1").loading);
    test.deliver_list({ make_pull(2, "two", pull_request_state::merged), make_pull(1, "one") });
    const hosting_repository_view view { test.controller.view_of("repo-1") };
    REQUIRE(view.pulls->size() == 2);
    CHECK((*view.pulls)[0].number == 2);
    const sidebar_rows rows { rows_of(view) };
    CHECK(find_row(rows, u8"pr:closed")->badge == u8"켬");
    CHECK(find_row(rows, u8"pr:n:2")->badge == u8"병합됨");
    CHECK(find_row(rows, u8"pr:n:2")->dimmed);
}

TEST_CASE("more pages are appended without duplicates", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    test.deliver_list({ make_pull(3, "three"), make_pull(2, "two") }, 2);
    CHECK(test.controller.view_of("repo-1").has_more);
    REQUIRE(find_row(rows_of(test.controller.view_of("repo-1")), u8"pr:more") != nullptr);
    test.controller.activate_row("repo-1", hosting_row_more);
    REQUIRE(std::holds_alternative<hosting_list_job>(test.jobs.back().work));
    CHECK(std::get<hosting_list_job>(test.jobs.back().work).query.page == 2);
    pull_request_info updated { make_pull(2, "two (edited)") };
    test.deliver_list({ updated, make_pull(1, "one") });
    const hosting_repository_view view { test.controller.view_of("repo-1") };
    REQUIRE(view.pulls->size() == 3);
    CHECK((*view.pulls)[1].title == "two (edited)");
    CHECK(view.has_more == false);
    CHECK(find_row(rows_of(view), u8"pr:more") == nullptr);
}

TEST_CASE("list errors show the reason and a lost token clears private data", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    test.deliver_list({ make_pull(1, "one") });
    test.controller.activate_row("repo-1", hosting_row_refresh);
    hosting_result<pull_request_page> limited {};
    limited.error.kind = hosting_error_kind::rate_limited;
    limited.error.status_code = 403;
    limited.error.rate_limit.remaining = 0;
    limited.error.rate_limit.reset_at = base_time + 600;
    test.deliver(test.jobs.back(), limited);
    hosting_repository_view view { test.controller.view_of("repo-1") };
    CHECK(view.error_text.find(u8"남은 횟수 0") != std::u8string::npos);
    CHECK(view.error_text.find(u8"10분 후") != std::u8string::npos);
    // 이전 목록은 남고 오류 행이 선다.
    CHECK(view.pulls->size() == 1);
    CHECK(find_row(rows_of(view), u8"pr:error") != nullptr);

    // 401로 토큰을 잃으면 목록·CI·상세를 화면에서 치우고 로그인 행으로 돌아간다.
    test.controller.activate_row("repo-1", hosting_row_refresh);
    hosting_result<pull_request_page> unauthorized {};
    unauthorized.error.kind = hosting_error_kind::unauthorized;
    test.deliver(test.jobs.back(), unauthorized, false, "");
    view = test.controller.view_of("repo-1");
    CHECK(view.signed_in == false);
    CHECK(view.pulls->empty());
    CHECK(find_row(rows_of(view), u8"pr:login") != nullptr);
}

TEST_CASE("opening a pull request reads its CI and shows the detail", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    pull_request_info pull { make_pull(12, "Add feature") };
    pull.draft = true;
    test.deliver_list({ pull });
    const std::size_t before { test.jobs.size() };
    test.controller.activate_row("repo-1", "12");
    REQUIRE(test.jobs.size() == before + 1);
    REQUIRE(std::holds_alternative<hosting_ci_job>(test.jobs.back().work));
    CHECK(std::get<hosting_ci_job>(test.jobs.back().work).sha == pull.head_sha);
    CHECK(test.controller.has_pull_request_detail("repo-1"));

    hosting_repository_view view { test.controller.view_of("repo-1") };
    REQUIRE(view.detail.has_value());
    CHECK(view.detail->number == 12);
    CHECK(view.detail->state_text == u8"초안");
    CHECK(view.detail->local_branch == u8"pr/12");
    CHECK(view.detail->ci.loading);
    CHECK(view.detail->updated_text.empty() == false);

    ci_status_info status {};
    status.sha = pull.head_sha;
    status.state = ci_state::failure;
    status.checks.push_back({ "build", ci_state::failure, "https://ci/1" });
    test.deliver(test.jobs.back(), hosting_result<ci_status_info> { .value = status });
    view = test.controller.view_of("repo-1");
    CHECK(view.detail->ci.state == ci_state::failure);
    CHECK(view.detail->ci.text == u8"CI 실패");
    REQUIRE(view.detail->ci.checks.size() == 1);
    // 목록 행에도 캐시된 CI가 붙는다.
    const sidebar_rows rows { rows_of(view) };
    CHECK(find_row(rows, u8"pr:n:12")->badge.find(u8"CI ✗") != std::u8string::npos);
    CHECK(find_row(rows, u8"pr:n:12")->tone == badge_tone::danger);

    // 같은 커밋의 CI는 캐시가 신선한 동안 다시 읽지 않는다.
    const std::size_t jobs_now { test.jobs.size() };
    test.controller.on_commit_selected("repo-1", pull.head_sha);
    CHECK(test.jobs.size() == jobs_now);
    const std::optional<ci_mark_view> mark { test.controller.ci_mark("repo-1", pull.head_sha) };
    REQUIRE(mark.has_value());
    CHECK(mark->state == ci_state::failure);

    // 시간이 지나면 다시 읽는다. 선택한 PR을 닫으면 상세가 사라진다.
    test.now += 1000;
    test.controller.on_commit_selected("repo-1", pull.head_sha);
    CHECK(test.jobs.size() == jobs_now + 1);
    test.controller.clear_selection("repo-1");
    CHECK(test.controller.view_of("repo-1").detail.has_value() == false);
}

TEST_CASE("a selected commit gets a CI mark only on a signed-in host", "[hosting][controller]")
{
    harness test {};
    const std::string sha(40, 'c');
    test.sign_in();
    test.controller.on_commit_selected("repo-1", sha);
    REQUIRE(std::holds_alternative<hosting_ci_job>(test.jobs.back().work));
    std::optional<ci_mark_view> mark { test.controller.ci_mark("repo-1", sha) };
    REQUIRE(mark.has_value());
    CHECK(mark->loading);
    CHECK(mark->text == u8"CI 확인 중…");
    // 이미 읽는 중이면 또 부르지 않는다.
    const std::size_t count { test.jobs.size() };
    test.controller.on_commit_selected("repo-1", sha);
    CHECK(test.jobs.size() == count);

    ci_status_info status {};
    status.sha = sha;
    status.state = ci_state::success;
    test.deliver(test.jobs.back(), hosting_result<ci_status_info> { .value = status });
    mark = test.controller.ci_mark("repo-1", sha);
    CHECK(mark->text == u8"CI 통과");
    CHECK(mark->source == u8"GitHub");
    CHECK(test.controller.ci_mark("repo-1", "WIP").has_value() == false);
    CHECK(test.controller.ci_mark("repo-1", "").has_value() == false);

    test.controller.sign_out("github.com");
    CHECK(test.controller.ci_mark("repo-1", sha).has_value() == false);
    REQUIRE(test.sign_outs.size() == 1);
    CHECK(test.sign_outs[0].host_key == "github.com");
}

TEST_CASE("a failed CI read says so and can be retried", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    const std::string sha(40, 'd');
    test.controller.on_commit_selected("repo-1", sha);
    hosting_result<ci_status_info> failed {};
    failed.error.kind = hosting_error_kind::network;
    failed.error.message = "cannot connect";
    test.deliver(test.jobs.back(), failed);
    const std::optional<ci_mark_view> mark { test.controller.ci_mark("repo-1", sha) };
    REQUIRE(mark.has_value());
    CHECK(mark->text == u8"CI 상태를 읽지 못했습니다");
    CHECK(mark->error.empty() == false);
    const std::size_t count { test.jobs.size() };
    test.controller.on_commit_selected("repo-1", sha);
    CHECK(test.jobs.size() == count + 1);
}

TEST_CASE("checking out a pull request fetches its ref into pr/N and then checks it out", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    test.deliver_list({ make_pull(7, "seven") });
    test.controller.activate_row("repo-1", "7");
    hosting_detail_action_intent intent { hosting_detail_action::checkout };
    REQUIRE(test.controller.handle(luil::app_message { intent }));
    REQUIRE(test.git_submissions.size() == 1);
    CHECK(test.git_submissions[0].first == "repo-1");
    const std::vector<git_operation>& operations { test.git_submissions[0].second };
    REQUIRE(operations.size() == 2);
    const auto* const fetch { std::get_if<fetch_request>(&operations[0]) };
    REQUIRE(fetch != nullptr);
    CHECK(fetch->remote == "origin");
    CHECK(fetch->refspec == "pull/7/head:pr/7");
    CHECK(fetch->prune == false);
    const auto* const checkout { std::get_if<checkout_branch_request>(&operations[1]) };
    REQUIRE(checkout != nullptr);
    CHECK(checkout->name == "pr/7");

    // 이미 그 브랜치에 있으면 받지 않고 알린다.
    test.target().current_branch = "pr/7";
    REQUIRE(test.controller.view_of("repo-1").detail.has_value());
    CHECK(test.controller.view_of("repo-1").detail->checked_out);
    REQUIRE(test.controller.handle(luil::app_message { intent }));
    CHECK(test.git_submissions.size() == 1);
}

TEST_CASE("a GitLab merge request is fetched from merge-requests/N into mr/N", "[hosting][controller]")
{
    harness test {};
    test.target().repository = resolve_hosting_repository("git@gitlab.com:group/sub/app.git", "origin", {});
    test.sign_in();
    pull_request_info merge_request { make_pull(5, "five") };
    merge_request.web_url = "https://gitlab.com/group/sub/app/-/merge_requests/5";
    test.deliver_list({ merge_request });
    CHECK(test.controller.view_of("repo-1").abbreviation == u8"MR");
    test.controller.activate_row("repo-1", "5");
    hosting_detail_action_intent intent { hosting_detail_action::checkout };
    REQUIRE(test.controller.handle(luil::app_message { intent }));
    REQUIRE(test.git_submissions.size() == 1);
    CHECK(std::get<fetch_request>(test.git_submissions[0].second[0]).refspec == "merge-requests/5/head:mr/5");
    CHECK(std::get<checkout_branch_request>(test.git_submissions[0].second[1]).name == "mr/5");
}

TEST_CASE("the browser opens only http and https addresses", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    pull_request_info evil { make_pull(1, "evil") };
    evil.web_url = "file:///C:/Windows/System32/calc.exe";
    pull_request_info spaced { make_pull(2, "spaced") };
    spaced.web_url = "https://github.com/x y";
    test.deliver_list({ evil, spaced, make_pull(3, "fine") });
    const hosting_detail_action_intent open { hosting_detail_action::open_browser };
    for (const char* number : { "1", "2" })
    {
        test.controller.activate_row("repo-1", number);
        REQUIRE(test.controller.handle(luil::app_message { open }));
        CHECK(test.opened_urls.empty());
        CHECK(test.notices.back().second);
    }
    test.controller.activate_row("repo-1", "3");
    REQUIRE(test.controller.handle(luil::app_message { open }));
    REQUIRE(test.opened_urls.size() == 1);
    CHECK(test.opened_urls[0] == "https://github.com/octo/hello/pull/3");
}

TEST_CASE("the create dialog defaults come from the current branch and the remote branches", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    std::optional<create_pull_request_defaults> defaults { test.controller.create_defaults("repo-1") };
    REQUIRE(defaults.has_value());
    CHECK(defaults->blocked_reason.empty());
    CHECK(defaults->source_branch == "feature/x");
    CHECK(defaults->target_branch == "main");
    CHECK(defaults->title == "Add the feature");
    CHECK(defaults->needs_push);
    CHECK(defaults->push_remote == "origin");

    // upstream이 있고 앞서지 않았으면 push가 필요 없다. main 위에서는 다른 대상을 고른다.
    test.target().has_upstream = true;
    test.target().ahead = 0;
    test.target().current_branch = "main";
    defaults = test.controller.create_defaults("repo-1");
    CHECK(defaults->needs_push == false);
    CHECK(defaults->target_branch == "develop");
    test.target().ahead = 2;
    CHECK(test.controller.create_defaults("repo-1")->needs_push);

    test.target().detached = true;
    CHECK(test.controller.create_defaults("repo-1")->blocked_reason.empty() == false);
    test.target().detached = false;
    test.controller.sign_out("github.com");
    CHECK(test.controller.create_defaults("repo-1")->blocked_reason == u8"먼저 로그인하세요.");
    CHECK(test.controller.view_of("repo-1").can_create == false);
    const sidebar_rows rows { rows_of(test.controller.view_of("repo-1")) };
    CHECK(find_row(rows, u8"pr:login") != nullptr);
}

TEST_CASE("creating after a push waits for the push and creates only when it succeeded", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    const create_pull_request_params params { .title = "T", .body = "B", .source_branch = "feature/x", .target_branch = "main", .draft = true };
    const std::size_t jobs_before { test.jobs.size() };
    test.controller.submit_create("repo-1", params, true);
    REQUIRE(test.git_submissions.size() == 1);
    const auto* const push { std::get_if<push_execute_request>(&test.git_submissions[0].second[0]) };
    REQUIRE(push != nullptr);
    CHECK(push->remote == "origin");
    CHECK(push->branch == "feature/x");
    CHECK(push->remote_branch == "feature/x");
    CHECK(push->set_upstream);
    // push가 도는 동안은 API를 부르지 않는다.
    test.controller.after_events();
    CHECK(test.jobs.size() == jobs_before);

    const operation_id id { test.operations.begin()->first };
    test.operations[id].state = operation_state::succeeded;
    test.controller.after_events();
    REQUIRE(test.jobs.size() == jobs_before + 1);
    REQUIRE(std::holds_alternative<hosting_create_job>(test.jobs.back().work));
    const create_pull_request_params& sent { std::get<hosting_create_job>(test.jobs.back().work).params };
    CHECK(sent.title == "T");
    CHECK(sent.draft);
    CHECK(sent.source_branch == "feature/x");
    // 같은 push를 두 번 이어 받지 않는다.
    test.controller.after_events();
    CHECK(test.jobs.size() == jobs_before + 1);

    // 푸시가 실패하면 만들지 않고 알린다.
    test.controller.submit_create("repo-1", params, true);
    const operation_id failed { std::prev(test.operations.end())->first };
    test.operations[failed].state = operation_state::failed;
    test.controller.after_events();
    CHECK(test.jobs.size() == jobs_before + 1);
    CHECK(test.notices.back().second);
}

TEST_CASE("creating without a push calls the API at once and shows the new pull request", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    test.controller.submit_create("repo-1", { .title = "T", .source_branch = "feature/x", .target_branch = "main" }, false);
    CHECK(test.git_submissions.empty());
    REQUIRE(std::holds_alternative<hosting_create_job>(test.jobs.back().work));
    created_pull_request created {};
    created.number = 42;
    created.title = "T";
    created.web_url = "https://github.com/octo/hello/pull/42";
    created.head_sha = std::string(40, 'e');
    test.deliver(test.jobs.back(), hosting_result<created_pull_request> { .value = created });
    // 만든 PR이 열리고 주소가 안내에 나온다. 목록도 다시 읽는다.
    const hosting_repository_view view { test.controller.view_of("repo-1") };
    REQUIRE(view.detail.has_value());
    CHECK(view.detail->number == 42);
    CHECK(view.detail->just_created);
    CHECK(test.notices.back().first.find(u8"https://github.com/octo/hello/pull/42") != std::u8string::npos);
    CHECK(std::ranges::any_of(test.jobs, [](const hosting_job& job) { return std::holds_alternative<hosting_list_job>(job.work); }));

    // 서버가 거절하면 이유를 알린다.
    test.controller.submit_create("repo-1", { .title = "T2", .source_branch = "feature/x", .target_branch = "main" }, false);
    hosting_result<created_pull_request> rejected {};
    rejected.error.kind = hosting_error_kind::rejected;
    rejected.error.message = "A pull request already exists";
    test.deliver(test.jobs.back(), rejected);
    CHECK(test.notices.back().second);
    CHECK(test.notices.back().first.find(u8"A pull request already exists") != std::u8string::npos);
}

TEST_CASE("closing a repository drops its state and pending results", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    test.controller.activate_row("repo-1", hosting_row_refresh);
    const hosting_job pending { test.jobs.back() };
    test.controller.on_repository_closed("repo-1");
    test.deliver(pending, hosting_result<pull_request_page> { .value = pull_request_page {} });
    CHECK(test.controller.view_of("repo-1").loaded == false);
}

TEST_CASE("the sidebar rows honor the filter and the collapsed bit", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    test.deliver_list({ make_pull(12, "Add feature"), make_pull(11, "Fix bug") });
    const hosting_repository_view view { test.controller.view_of("repo-1") };
    const sidebar_rows filtered { rows_of(view, u8"bug") };
    CHECK(find_row(filtered, u8"pr:n:11") != nullptr);
    CHECK(find_row(filtered, u8"pr:n:12") == nullptr);
    CHECK(filtered.counts[static_cast<std::size_t>(sidebar_section::pull_requests)] == 1);
    const std::uint8_t bit { static_cast<std::uint8_t>(1u << static_cast<unsigned>(sidebar_section::pull_requests)) };
    const sidebar_rows collapsed { rows_of(view, {}, bit) };
    REQUIRE(collapsed.rows.size() == 1);
    CHECK(collapsed.rows.front().collapsed);
    CHECK(collapsed.rows.front().count == 2);
}

TEST_CASE("the host rows list built-in and configured hosts with their in-memory sign-in state", "[hosting][controller]")
{
    harness test {};
    test.sign_in();
    const std::vector<hosting_host_row_view> rows { test.controller.host_rows({ { "git.corp.example", hosting_provider::gitlab, "" } }) };
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].host == "github.com");
    CHECK(rows[0].builtin);
    CHECK(rows[0].signed_in);
    CHECK(rows[0].user == u8"octocat");
    CHECK(rows[1].host == "gitlab.com");
    CHECK(rows[1].signed_in == false);
    CHECK(rows[2].host == "git.corp.example");
    CHECK(rows[2].builtin == false);
}

TEST_CASE("the create dialog form validates the target and builds the request values", "[hosting][controller]")
{
    dialog_state dialog {};
    dialog.kind = dialog_kind::create_pull_request;
    dialog.title = u8"PR 만들기";
    dialog.subject = "feature/x";
    dialog.choices = { "origin" };
    dialog.flag_c = true;
    dialog.flag_b = true;
    form_inputs inputs {};
    std::u8string reason {};
    CHECK(create_pull_request_ready(dialog, inputs, &reason) == false);
    CHECK(reason == u8"대상 브랜치를 입력하세요.");

    inputs.fields[0].text = u8"main";
    inputs.fields[1].text = u8"  새 기능  ";
    inputs.fields[2].text = u8"설명";
    CHECK(create_pull_request_ready(dialog, inputs));
    inputs.fields[0].text = u8"feature/x";
    CHECK(create_pull_request_ready(dialog, inputs, &reason) == false);
    CHECK(reason.find(u8"같습니다") != std::u8string::npos);
    inputs.fields[0].text = u8"--bad";
    CHECK(create_pull_request_ready(dialog, inputs) == false);
    inputs.fields[0].text = u8"main";
    inputs.fields[1].text = u8"   ";
    CHECK(create_pull_request_ready(dialog, inputs, &reason) == false);
    CHECK(reason == u8"제목을 입력하세요.");
    inputs.fields[1].text = u8"  새 기능  ";

    dialog.flag_a = true;
    const create_pull_request_params params { make_create_pull_request_params(dialog, inputs) };
    CHECK(params.target_branch == "main");
    CHECK(params.source_branch == "feature/x");
    CHECK(params.title == "새 기능");
    CHECK(params.body == "설명");
    CHECK(params.draft);

    form_build_context context {};
    context.inputs = &inputs;
    const form_view form { build_create_pull_request_form(dialog, context) };
    CHECK(form.id == u8"create-pull-request-dialog");
    REQUIRE(form.fields.size() == 3);
    REQUIRE(form.checks.size() == 2);
    CHECK(form.checks[0].checked);
    CHECK(form.checks[1].checked);
    REQUIRE(form.buttons.size() == 2);
    CHECK(form.buttons[1].enabled);
    // push가 필요 없으면 push 체크 칸이 없다.
    dialog.flag_c = false;
    CHECK(build_create_pull_request_form(dialog, context).checks.size() == 1);
    // push를 끄면 경고가 선다.
    dialog.flag_c = true;
    dialog.flag_b = false;
    CHECK(build_create_pull_request_form(dialog, context).warnings.size() == 1);
}

TEST_CASE("publishing to a host signs in first, creates the repository and then adds the remote and pushes", "[hosting][publish]")
{
    harness test {};
    // 원격이 없는 저장소다.
    test.target().repository.reset();
    publish_repository_request request {};
    request.host = *hosting_repository_for_host("github.com", {});
    request.params = { .name = "hello", .description = {}, .owner = {}, .is_private = true };
    request.push_branch = "main";
    test.controller.request_publish("repo-1", request);
    CHECK(test.controller.publishing("repo-1"));
    // 같은 저장소의 게시를 두 번 내지 않는다.
    test.controller.request_publish("repo-1", request);
    REQUIRE(test.jobs.size() == 1);
    REQUIRE_FALSE(test.notices.empty());
    CHECK(test.notices.back().second);
    REQUIRE(std::holds_alternative<hosting_sign_in_job>(test.jobs[0].work));
    CHECK(test.jobs[0].context.repository.host_key == "github.com");
    CHECK(test.jobs[0].context.working_directory == "C:\\work\\hello");

    test.deliver(test.jobs[0], hosting_result<std::string> { .value = std::string { "octocat" } });
    REQUIRE(test.jobs.size() == 2);
    REQUIRE(std::holds_alternative<hosting_create_repository_job>(test.jobs[1].work));
    CHECK(std::get<hosting_create_repository_job>(test.jobs[1].work).params.name == "hello");
    CHECK(std::get<hosting_create_repository_job>(test.jobs[1].work).params.is_private);
    CHECK(test.git_submissions.empty());

    created_repository created {
        .full_name = "octocat/hello", .web_url = "https://github.com/octocat/hello", .https_url = "https://github.com/octocat/hello.git", .ssh_url = "git@github.com:octocat/hello.git"
    };
    test.deliver(test.jobs[1], hosting_result<created_repository> { .value = created });
    CHECK_FALSE(test.controller.publishing("repo-1"));
    REQUIRE(test.git_submissions.size() == 1);
    const std::vector<git_operation>& operations { test.git_submissions[0].second };
    REQUIRE(operations.size() == 2);
    const auto* const add { std::get_if<add_remote_request>(&operations[0]) };
    REQUIRE(add != nullptr);
    CHECK(add->name == "origin");
    CHECK(add->url == "https://github.com/octocat/hello.git");
    const auto* const push { std::get_if<push_execute_request>(&operations[1]) };
    REQUIRE(push != nullptr);
    CHECK(push->remote == "origin");
    CHECK(push->branch == "main");
    CHECK(push->set_upstream);
    CHECK_FALSE(test.notices.back().second);
}

TEST_CASE("publishing skips sign-in on a signed-in host, uses the ssh URL when asked and stops on a rejection", "[hosting][publish]")
{
    harness test {};
    test.sign_in();
    const std::size_t before { test.jobs.size() };
    publish_repository_request request {};
    request.host = *hosting_repository_for_host("github.com", {});
    request.params = { .name = "hello", .owner = "octo", .is_private = false };
    request.remote_name = "upstream";
    request.use_ssh = true;
    test.controller.request_publish("repo-1", request);
    REQUIRE(test.jobs.size() == before + 1);
    REQUIRE(std::holds_alternative<hosting_create_repository_job>(test.jobs.back().work));
    test.deliver(test.jobs.back(),
        hosting_result<created_repository> { .value = created_repository { .full_name = "octo/hello", .https_url = "https://github.com/octo/hello.git", .ssh_url = "git@github.com:octo/hello.git" } });
    REQUIRE(test.git_submissions.size() == 1);
    // push할 브랜치를 주지 않았으면 원격만 더한다.
    REQUIRE(test.git_submissions[0].second.size() == 1);
    CHECK(std::get<add_remote_request>(test.git_submissions[0].second[0]).name == "upstream");
    CHECK(std::get<add_remote_request>(test.git_submissions[0].second[0]).url == "git@github.com:octo/hello.git");

    // 서버가 거절하면 알리고 git 작업을 내지 않는다.
    test.controller.request_publish("repo-1", request);
    hosting_result<created_repository> rejected {};
    rejected.error = hosting_error { .kind = hosting_error_kind::rejected, .status_code = 422, .message = "name already exists on this account" };
    test.deliver(test.jobs.back(), rejected);
    CHECK(test.git_submissions.size() == 1);
    REQUIRE_FALSE(test.notices.empty());
    CHECK(test.notices.back().second);
    CHECK_FALSE(test.controller.publishing("repo-1"));

    // 로그인이 실패해도 게시는 끝난다.
    harness fresh {};
    fresh.controller.request_publish("repo-1", request);
    hosting_result<std::string> refused {};
    refused.error.kind = hosting_error_kind::credential_unavailable;
    fresh.deliver(fresh.jobs.back(), refused, false);
    CHECK(fresh.jobs.size() == 1);
    CHECK_FALSE(fresh.controller.publishing("repo-1"));
}

TEST_CASE("an overlapping sign-in for the same host creates the repository only once", "[hosting][publish]")
{
    harness test {};
    // 사이드바의 로그인이 아직 끝나지 않았는데 게시를 골랐다.
    test.controller.request_sign_in("repo-1");
    publish_repository_request request {};
    request.host = *hosting_repository_for_host("github.com", {});
    request.params = { .name = "hello" };
    test.controller.request_publish("repo-1", request);
    REQUIRE(test.jobs.size() == 2);
    const hosting_job first { test.jobs[0] };
    const hosting_job second { test.jobs[1] };
    test.deliver(first, hosting_result<std::string> { .value = std::string { "octocat" } });
    test.deliver(second, hosting_result<std::string> { .value = std::string { "octocat" } });
    const auto creates { std::ranges::count_if(test.jobs, [](const hosting_job& job) { return std::holds_alternative<hosting_create_repository_job>(job.work); }) };
    CHECK(creates == 1);
    CHECK(test.controller.publishing("repo-1"));
}
