// 웹뷰 동작이 낳는 일(`diff_view_effect`)을 operation·worker 일·클립보드로 옮기는 조립부의 test다.
// workspace·scheduler·relay·클립보드는 낸 것을 세는 가짜로 바꾼다.
#include "application/diff_view_coordinator.h"
#include "infrastructure/git_diff_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
    using namespace cyella;
    using json = nlohmann::json;

    const std::string ready_message { R"({"type":"ready","protocol":1})" };

    std::string text_diff()
    {
        // 줄 위치: 0 a, 1 -b, 2 +B, 3 +B2, 4 c, 5 d, 6 -e, 7 +E, 8 f, 9 g
        return "diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n@@ -1,7 +1,8 @@\n a\n-b\n+B\n+B2\n c\n d\n-e\n+E\n f\n g\n";
    }

    file_diff text_file_diff(const diff_view view = diff_view::unstaged)
    {
        file_diff diff {};
        diff.request.path = "f.txt";
        diff.request.view = view;
        diff.files = parse_git_diff(text_diff());
        return diff;
    }

    file_diff binary_file_diff(const std::string& path)
    {
        file_diff diff {};
        diff.request.path = path;
        diff_file file {};
        file.old_path = file.new_path = path;
        file.is_binary = true;
        diff.files.push_back(std::move(file));
        return diff;
    }

    diff_target target_of(const file_diff& diff, std::string repository = "repo-1")
    {
        return diff_target { std::move(repository), u8"C:\\repo", diff.request };
    }

    class fake_gateway final : public diff_git_gateway
    {
    public:
        extracted_sides extract(const diff_side_plan& plan, const bool write_files) override
        {
            extractions.push_back(plan);
            last_write_files = write_files;
            return result;
        }

        extracted_sides extract_cancellable(const diff_side_plan& plan, const bool write_files, const process_cancellation_token& cancellation) override
        {
            saw_cancellation = cancellation.cancelled();
            return extract(plan, write_files);
        }

        std::optional<std::string> configured_diff_tool(const std::u8string_view) override
        {
            return std::nullopt;
        }

        std::vector<diff_side_plan> extractions {};
        bool last_write_files { false };
        bool saw_cancellation { false };
        extracted_sides result {};
    };

    class fake_launcher final : public diff_tool_launcher
    {
    public:
        tool_launch_result launch(const tool_launch_request& request) override
        {
            requests.push_back(request);
            return outcome;
        }

        std::vector<tool_launch_request> requests {};
        tool_launch_result outcome { true, {} };
    };

    struct harness
    {
        harness()
            : opener { gateway, launcher }
            , coordinator { make_ports() }
        {
            gateway.result.ok = true;
            gateway.result.left = { true, u8"C:\\tmp\\left\\logo.png", 10 };
            gateway.result.right = { true, u8"C:\\tmp\\right\\logo.png", 20 };
        }

        diff_view_ports make_ports()
        {
            diff_view_ports ports {};
            ports.submit = [this](const std::string& repository, git_operation operation) {
                submitted.emplace_back(repository, std::move(operation));
                return next_operation++;
            };
            ports.schedule = [this](operation_request request) {
                scheduled.push_back(std::move(request));
                return accept_jobs;
            };
            ports.copy_text = [this](std::string text) { copied.push_back(std::move(text)); };
            ports.sides_ready = [this](diff_sides_ready_intent message) { sides.push_back(std::move(message)); };
            ports.external_done = [this](external_open_done_intent message) { externals.push_back(std::move(message)); };
            ports.tools = [this] { return tools; };
            ports.gateway = &gateway;
            ports.opener = &opener;
            ports.git_executable = u8"C:\\Git\\cmd\\git.exe";
            return ports;
        }

        // 줄 선 일을 하나 꺼내 실행한다 (scheduler worker 역할).
        void run_next_job()
        {
            REQUIRE_FALSE(scheduled.empty());
            operation_request request { std::move(scheduled.front()) };
            scheduled.erase(scheduled.begin());
            request.job(operation_context {});
        }

        // 페이지가 준비된 상태로 첫 diff를 보인다.
        void show(const file_diff& diff, const std::string& repository = "repo-1")
        {
            REQUIRE(coordinator.handle_web_message(ready_message).accepted);
            coordinator.sync(target_of(diff, repository), &diff, ++serial);
            REQUIRE(coordinator.controller().poll());
        }

        std::string action(json message)
        {
            message["seq"] = coordinator.controller().content_seq();
            return message.dump();
        }

        template<typename request_type>
        const request_type& submitted_request(const std::size_t index) const
        {
            REQUIRE(index < submitted.size());
            REQUIRE(std::holds_alternative<request_type>(submitted[index].second));
            return std::get<request_type>(submitted[index].second);
        }

        fake_gateway gateway {};
        fake_launcher launcher {};
        external_diff_opener opener;
        diff_view_coordinator coordinator;
        std::vector<std::pair<std::string, git_operation>> submitted {};
        std::vector<operation_request> scheduled {};
        std::vector<std::string> copied {};
        std::vector<diff_sides_ready_intent> sides {};
        std::vector<external_open_done_intent> externals {};
        std::vector<external_diff_tool> tools {};
        operation_id next_operation { 100 };
        std::uint64_t serial { 0 };
        bool accept_jobs { true };
    };

    operation_finished finished_with(const operation_state state, std::u8string summary = {})
    {
        operation_finished finished {};
        finished.state = state;
        finished.summary = std::move(summary);
        return finished;
    }
} // namespace

TEST_CASE("stage hunk submits apply_patch to the shown repository and re-reads the file afterwards", "[diff][coordinator]")
{
    harness test {};
    const file_diff diff { text_file_diff() };
    test.show(diff, "repo-7");

    REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "stage_hunk" }, { "hunk", 0 } })).accepted);
    REQUIRE(test.submitted.size() == 1);
    CHECK(test.submitted[0].first == "repo-7");
    const apply_patch_request& apply { test.submitted_request<apply_patch_request>(0) };
    CHECK(apply.cached);
    CHECK_FALSE(apply.reverse);
    CHECK(apply.recount);
    CHECK(apply.patch.starts_with("diff --git a/f.txt b/f.txt\n"));
    CHECK(apply.patch.find("@@ -1,7 +1,8 @@") != std::string::npos);
    CHECK(test.coordinator.busy());

    // 적용이 끝나면 같은 요청의 diff_file을 같은 저장소에 다시 낸다.
    test.coordinator.on_operation_finished(100, finished_with(operation_state::succeeded));
    REQUIRE(test.submitted.size() == 2);
    CHECK(test.submitted[1].first == "repo-7");
    CHECK(test.submitted_request<diff_file_request>(1) == diff.request);

    // 다시 읽은 diff가 오면(serial이 오르면) 새 내용으로 웹뷰가 갱신된다.
    test.coordinator.on_operation_finished(101, finished_with(operation_state::succeeded));
    CHECK_FALSE(test.coordinator.busy());
    CHECK_FALSE(test.coordinator.controller().poll());
    test.coordinator.sync(target_of(diff, "repo-7"), &diff, test.serial + 1);
    CHECK(test.coordinator.controller().poll());
    CHECK(test.coordinator.take_notice() == std::nullopt);
}

TEST_CASE("unstage and discard pick the matching git apply flags", "[diff][coordinator]")
{
    SECTION("unstage reverses on the index")
    {
        harness test {};
        test.show(text_file_diff(diff_view::staged));
        REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "unstage_hunk" }, { "hunk", 0 } })).accepted);
        const apply_patch_request& apply { test.submitted_request<apply_patch_request>(0) };
        CHECK(apply.cached);
        CHECK(apply.reverse);
    }

    SECTION("discard reverses on the working tree")
    {
        harness test {};
        test.show(text_file_diff(diff_view::unstaged));
        REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "discard_hunk" }, { "hunk", 0 } })).accepted);
        const apply_patch_request& apply { test.submitted_request<apply_patch_request>(0) };
        CHECK_FALSE(apply.cached);
        CHECK(apply.reverse);
    }
}

TEST_CASE("a line selection becomes a patch of exactly those lines", "[diff][coordinator]")
{
    harness test {};
    test.show(text_file_diff());
    // 줄 2, 3(+B, +B2)만 고른다.
    REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "stage_lines" }, { "hunk", 0 }, { "ranges", json::array({ json::array({ 2, 3 }) }) } })).accepted);
    const apply_patch_request& apply { test.submitted_request<apply_patch_request>(0) };
    CHECK(apply.cached);
    CHECK(apply.patch.find("+B\n") != std::string::npos);
    CHECK(apply.patch.find("+B2\n") != std::string::npos);
    // 고르지 않은 변경은 문맥으로 물러난다.
    CHECK(apply.patch.find("+E\n") == std::string::npos);
    CHECK(apply.patch.find("-e\n") == std::string::npos);
}

TEST_CASE("a failed apply unlocks the page and says why", "[diff][coordinator]")
{
    harness test {};
    const file_diff diff { text_file_diff() };
    test.show(diff);
    REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "stage_hunk" }, { "hunk", 0 } })).accepted);
    CHECK_FALSE(test.coordinator.controller().can_stage());

    operation_finished failed { finished_with(operation_state::failed) };
    failed.failure = operation_failure { failure_kind::other, u8"patch does not apply", {} };
    test.coordinator.on_operation_finished(100, failed);

    CHECK_FALSE(test.coordinator.busy());
    // 같은 diff를 다시 보내 웹뷰의 잠금을 푼다.
    CHECK(test.coordinator.controller().can_stage());
    CHECK(test.coordinator.controller().poll());
    const std::optional<diff_notice> notice { test.coordinator.take_notice() };
    REQUIRE(notice.has_value());
    CHECK(notice->warning);
    CHECK(notice->text.find(u8"patch does not apply") != std::u8string::npos);
    // re-read는 없다.
    CHECK(test.submitted.size() == 1);
}

TEST_CASE("a cancelled apply unlocks the page without a notice", "[diff][coordinator]")
{
    harness test {};
    test.show(text_file_diff());
    REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "stage_hunk" }, { "hunk", 0 } })).accepted);
    test.coordinator.on_operation_finished(100, finished_with(operation_state::cancelled));
    CHECK(test.coordinator.controller().can_stage());
    CHECK_FALSE(test.coordinator.take_notice().has_value());
}

TEST_CASE("a rejected submit reports and unlocks", "[diff][coordinator]")
{
    harness test {};
    test.show(text_file_diff());
    test.next_operation = 0;
    REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "stage_hunk" }, { "hunk", 0 } })).accepted);
    CHECK(test.coordinator.controller().can_stage());
    CHECK(test.coordinator.take_notice().has_value());
}

TEST_CASE("an image diff prepares its two sides on the scheduler and shows them when they arrive", "[diff][coordinator]")
{
    harness test {};
    const file_diff diff { binary_file_diff("art/logo.png") };
    REQUIRE(test.coordinator.handle_web_message(ready_message).accepted);
    test.coordinator.sync(target_of(diff), &diff, 1);
    CHECK(test.coordinator.controller().content() == diff_content::loading);

    // 두 쪽은 scheduler worker에서 꺼낸다. logic thread에서 git을 부르지 않는다.
    REQUIRE(test.scheduled.size() == 1);
    CHECK(test.scheduled[0].repository_key == "diff-view");
    CHECK(test.gateway.extractions.empty());

    test.run_next_job();
    REQUIRE(test.gateway.extractions.size() == 1);
    CHECK(test.gateway.last_write_files);
    CHECK(test.gateway.extractions[0].new_path == "art/logo.png");
    REQUIRE(test.sides.size() == 1);
    CHECK(test.sides[0].sides.ok);

    test.coordinator.sides_prepared(test.sides[0]);
    CHECK(test.coordinator.controller().content() == diff_content::image);
    REQUIRE(test.coordinator.controller().poll());
    const json message { json::parse(test.coordinator.controller().latest_message()) };
    CHECK(message.at("type") == "show_image");
}

TEST_CASE("a plain binary file shows the binary card when the sides could not be written", "[diff][coordinator]")
{
    harness test {};
    const file_diff diff { binary_file_diff("data.bin") };
    REQUIRE(test.coordinator.handle_web_message(ready_message).accepted);
    test.coordinator.sync(target_of(diff), &diff, 1);
    REQUIRE(test.scheduled.size() == 1);
    test.run_next_job();
    // 이미지가 아니면 크기만 구한다.
    CHECK_FALSE(test.gateway.last_write_files);
    test.coordinator.sides_prepared(test.sides.at(0));
    CHECK(test.coordinator.controller().content() == diff_content::binary);
}

TEST_CASE("moving to another file cancels the earlier sides job and drops its late result", "[diff][coordinator]")
{
    harness test {};
    const file_diff first { binary_file_diff("a.png") };
    REQUIRE(test.coordinator.handle_web_message(ready_message).accepted);
    test.coordinator.sync(target_of(first), &first, 1);
    REQUIRE(test.scheduled.size() == 1);

    const file_diff second { text_file_diff() };
    test.coordinator.sync(target_of(second), &second, 2);
    CHECK(test.coordinator.controller().content() == diff_content::text);

    // 앞 일은 취소된 token으로 돌고, 그 결과는 ticket이 달라 버려진다.
    test.run_next_job();
    CHECK(test.gateway.extractions.empty());
    REQUIRE(test.sides.size() == 1);
    CHECK_FALSE(test.sides[0].sides.ok);
    test.coordinator.sides_prepared(test.sides[0]);
    CHECK(test.coordinator.controller().content() == diff_content::text);
}

TEST_CASE("open external runs on the scheduler with the configured tools and reports the result", "[diff][coordinator]")
{
    harness test {};
    external_diff_tool tool {};
    tool.extensions = { u8".png" };
    tool.executable = u8"C:\\tools\\imgdiff.exe";
    tool.arguments = u8"{left} {right}";
    test.tools.push_back(tool);
    const file_diff diff { binary_file_diff("art/logo.png") };
    test.show(diff);
    test.run_next_job();
    test.coordinator.sides_prepared(test.sides.at(0));
    REQUIRE(test.coordinator.controller().poll());

    REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "open_external" } })).accepted);
    REQUIRE(test.scheduled.size() == 1);
    CHECK(test.launcher.requests.empty());
    test.run_next_job();

    REQUIRE(test.launcher.requests.size() == 1);
    CHECK(test.launcher.requests[0].executable == u8"C:\\tools\\imgdiff.exe");
    REQUIRE(test.launcher.requests[0].arguments.size() == 2);
    CHECK(test.launcher.requests[0].arguments[0] == u8"C:\\tmp\\left\\logo.png");
    REQUIRE(test.externals.size() == 1);
    CHECK(test.externals[0].result.status == external_open_status::launched_tool);

    test.coordinator.external_opened(test.externals[0]);
    const std::optional<diff_notice> notice { test.coordinator.take_notice() };
    REQUIRE(notice.has_value());
    CHECK_FALSE(notice->warning);
}

TEST_CASE("external open results become notices", "[diff][coordinator]")
{
    SECTION("no tool")
    {
        const diff_notice notice { describe_external_open({ external_open_status::no_tool, {}, "none" }) };
        CHECK(notice.warning);
        CHECK(notice.text.find(u8"diff 도구") != std::u8string::npos);
    }

    SECTION("launch failed names the tool and the reason")
    {
        const diff_notice notice { describe_external_open({ external_open_status::launch_failed, u8"C:\\tools\\x.exe", "access denied" }) };
        CHECK(notice.warning);
        CHECK(notice.text.find(u8"C:\\tools\\x.exe") != std::u8string::npos);
        CHECK(notice.text.find(u8"access denied") != std::u8string::npos);
    }

    SECTION("extraction failure and a bad template are warnings")
    {
        CHECK(describe_external_open({ external_open_status::extraction_failed, {}, "x" }).warning);
        CHECK(describe_external_open({ external_open_status::invalid_template, {}, "x" }).warning);
    }

    SECTION("launching is a plain confirmation")
    {
        CHECK_FALSE(describe_external_open({ external_open_status::launched_tool, u8"tool", {} }).warning);
        CHECK_FALSE(describe_external_open({ external_open_status::launched_difftool, u8"git", {} }).warning);
    }
}

TEST_CASE("a job that cannot be queued degrades instead of hanging", "[diff][coordinator]")
{
    harness test {};
    test.accept_jobs = false;
    const file_diff diff { binary_file_diff("art/logo.png") };
    REQUIRE(test.coordinator.handle_web_message(ready_message).accepted);
    test.coordinator.sync(target_of(diff), &diff, 1);
    // 두 쪽을 꺼내지 못했으니 바이너리 카드로 물러선다.
    CHECK(test.coordinator.controller().content() == diff_content::binary);
}

TEST_CASE("copy goes to the clipboard port", "[diff][coordinator]")
{
    harness test {};
    test.show(text_file_diff());
    REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "copy" }, { "text", "a\nb" } })).accepted);
    REQUIRE(test.copied.size() == 1);
    CHECK(test.copied[0] == "a\nb");
}

TEST_CASE("sync follows the wanted diff and clears stale content", "[diff][coordinator]")
{
    harness test {};
    const file_diff diff { text_file_diff() };
    test.show(diff);
    CHECK(test.coordinator.shown().has_value());

    SECTION("a diff that is still loading clears the old one")
    {
        file_diff other {};
        other.request.path = "other.txt";
        test.coordinator.sync(target_of(other), &diff, 2);
        CHECK_FALSE(test.coordinator.shown().has_value());
        CHECK(test.coordinator.controller().content() == diff_content::empty);
    }

    SECTION("closing the diff clears it")
    {
        test.coordinator.sync(std::nullopt, nullptr, 0);
        CHECK_FALSE(test.coordinator.shown().has_value());
        CHECK(test.coordinator.controller().content() == diff_content::empty);
    }

    SECTION("the same loaded diff is not shown twice")
    {
        CHECK_FALSE(test.coordinator.controller().poll());
        test.coordinator.sync(target_of(diff), &diff, test.serial);
        CHECK_FALSE(test.coordinator.controller().poll());
    }

    SECTION("switching to another tab's diff shows it and keeps the tab's repository")
    {
        const file_diff other { text_file_diff(diff_view::staged) };
        test.coordinator.sync(target_of(other, "repo-2"), &other, 1);
        REQUIRE(test.coordinator.shown().has_value());
        CHECK(test.coordinator.shown()->repository_id == "repo-2");
        CHECK(test.coordinator.controller().poll());
        REQUIRE(test.coordinator.handle_web_message(test.action({ { "type", "unstage_hunk" }, { "hunk", 0 } })).accepted);
        CHECK(test.submitted.at(0).first == "repo-2");
    }
}
