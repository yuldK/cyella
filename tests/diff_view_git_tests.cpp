// diff 뷰어의 git·파일·프로세스 입출력을 실제로 돌리는 통합 test다. 임시 저장소에서 blob을 꺼내고, 외부
// 프로세스를 띄우고, 컨트롤러가 낸 patch가 git_workspace를 거쳐 index에 닿는지 본다. 모두 `[integration]`
// 태그를 가지며 git이 없으면 건너뛴다.
#include "application/diff_view_controller.h"
#include "application/external_diff_opener.h"
#include "application/git_workspace.h"
#include "infrastructure/diff_temp_directory.h"
#include "infrastructure/git_diff_gateway.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_diff_tool_launcher.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_workspace_files.h"

#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {
    using namespace cyella;
    using json = nlohmann::json;
    using cyella::test::run_git;
    using cyella::test::text_list;

    void git_ok(const std::filesystem::path& directory, const text_list& arguments, const std::string_view input = {})
    {
        const test::git_run_result result { run_git(directory, arguments, input) };
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
    }

    std::string git_output(const std::filesystem::path& directory, const text_list& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        REQUIRE(result.exit_code == 0);
        return result.standard_output;
    }

    std::u8string u8_of(const std::filesystem::path& path)
    {
        return path.u8string();
    }

    // 임시 저장소와 실제 process runner·git 실행 파일을 가진 환경이다.
    struct environment
    {
        environment()
            : runner { win32::make_process_runner() }
        {
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool = resolve_git_tool({}, win32::current_git_tool_environment(), *runner, *probe, {});
            REQUIRE(tool.usable());
            repository = directory.path() / "repo";
            std::filesystem::create_directories(repository);
            git_ok(repository, { "init", "-q", "-b", "main" });
        }

        void write(const std::string& relative, const std::string_view content) const
        {
            test::write_file(repository / std::filesystem::path { std::u8string { relative.begin(), relative.end() } }, content);
        }

        void commit_all(const std::string& message) const
        {
            git_ok(repository, { "add", "-A" });
            git_ok(repository, { "commit", "-q", "-m", message });
        }

        [[nodiscard]] std::string read(const std::u8string& file) const
        {
            return test::read_file(std::filesystem::path { file });
        }

        test::temp_directory directory {};
        std::unique_ptr<process_runner> runner {};
        git_tool_info tool {};
        std::filesystem::path repository {};
    };

    diff_side_plan plan_of(const environment& env, const diff_view view, std::string old_path, std::string new_path, const char status = 'M', std::string sha = {})
    {
        diff_side_plan plan {};
        plan.repository_path = u8_of(env.repository);
        plan.view = view;
        plan.sha = std::move(sha);
        plan.old_path = std::move(old_path);
        plan.new_path = std::move(new_path);
        plan.status = status;
        return plan;
    }

    // 파일 끝에 NUL과 0xFF가 든 바이트다.
    std::string binary_bytes(const char seed)
    {
        std::string bytes { "BIN" };
        bytes.push_back(seed);
        bytes.push_back('\0');
        bytes.push_back('\xFF');
        bytes.append("\r\n\x01\x02", 4);
        return bytes;
    }
} // namespace

TEST_CASE("integration: the gateway extracts the two sides of an unstaged change byte for byte", "[diff][integration][gateway]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    const environment env {};
    env.write("img/logo.png", binary_bytes('A'));
    env.commit_all("base");
    // index와 작업 트리가 모두 다르도록 index를 한 번 더 바꾼다.
    env.write("img/logo.png", binary_bytes('B'));
    git_ok(env.repository, { "add", "img/logo.png" });
    env.write("img/logo.png", binary_bytes('C'));

    diff_temp_directory temp {};
    git_diff_gateway gateway { *env.runner, env.tool.executable, temp };

    SECTION("unstaged: the index against the worktree")
    {
        const extracted_sides sides { gateway.extract(plan_of(env, diff_view::unstaged, "img/logo.png", "img/logo.png"), true) };
        REQUIRE(sides.ok);
        CHECK(sides.left.present);
        CHECK(sides.right.present);
        CHECK(env.read(sides.left.file) == binary_bytes('B'));
        CHECK(env.read(sides.right.file) == binary_bytes('C'));
        CHECK(sides.left.size == binary_bytes('B').size());
        CHECK(sides.right.size == binary_bytes('C').size());
        // 원래 파일 이름이 남고 left·right 폴더로 갈린다.
        CHECK(std::filesystem::path { sides.left.file }.filename() == "logo.png");
        CHECK(std::filesystem::path { sides.left.file }.parent_path().filename() == "left");
        CHECK(std::filesystem::path { sides.right.file }.parent_path().filename() == "right");
    }

    SECTION("staged: HEAD against the index")
    {
        const extracted_sides sides { gateway.extract(plan_of(env, diff_view::staged, "img/logo.png", "img/logo.png"), true) };
        REQUIRE(sides.ok);
        CHECK(env.read(sides.left.file) == binary_bytes('A'));
        CHECK(env.read(sides.right.file) == binary_bytes('B'));
    }

    SECTION("sizes only writes no files")
    {
        const extracted_sides sides { gateway.extract(plan_of(env, diff_view::unstaged, "img/logo.png", "img/logo.png"), false) };
        REQUIRE(sides.ok);
        CHECK(sides.left.size == binary_bytes('B').size());
        CHECK(sides.right.size == binary_bytes('C').size());
        CHECK(sides.left.file.empty());
        CHECK(sides.right.file.empty());
    }

    SECTION("a missing path fails with a reason")
    {
        const extracted_sides sides { gateway.extract(plan_of(env, diff_view::staged, "img/none.png", "img/none.png"), true) };
        CHECK_FALSE(sides.ok);
        CHECK_FALSE(sides.error.empty());
    }
}

TEST_CASE("integration: the gateway reads commits, additions, deletions and Hangul names", "[diff][integration][gateway]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    const environment env {};
    const std::string name { "dir with space/\xED\x95\x9C\xEA\xB8\x80.bin" };
    env.write(name, binary_bytes('1'));
    env.write("gone.bin", binary_bytes('g'));
    env.commit_all("first");
    env.write(name, binary_bytes('2'));
    env.write("added.bin", binary_bytes('a'));
    std::filesystem::remove(env.repository / "gone.bin");
    env.commit_all("second");
    const std::string sha { git_output(env.repository, { "rev-parse", "HEAD" }).substr(0, 40) };

    diff_temp_directory temp {};
    git_diff_gateway gateway { *env.runner, env.tool.executable, temp };

    SECTION("a commit compares its first parent with itself")
    {
        const extracted_sides sides { gateway.extract(plan_of(env, diff_view::commit, name, name, 'M', sha), true) };
        REQUIRE(sides.ok);
        CHECK(env.read(sides.left.file) == binary_bytes('1'));
        CHECK(env.read(sides.right.file) == binary_bytes('2'));
        CHECK(std::filesystem::path { sides.left.file }.filename().u8string() == u8"한글.bin");
    }

    SECTION("an added file has an absent left side with an empty placeholder")
    {
        const extracted_sides sides { gateway.extract(plan_of(env, diff_view::commit, {}, "added.bin", 'A', sha), true) };
        REQUIRE(sides.ok);
        CHECK_FALSE(sides.left.present);
        CHECK(sides.right.present);
        CHECK(std::filesystem::exists(std::filesystem::path { sides.left.file }));
        CHECK(env.read(sides.left.file).empty());
        CHECK(env.read(sides.right.file) == binary_bytes('a'));
    }

    SECTION("a deleted file has an absent right side")
    {
        const extracted_sides sides { gateway.extract(plan_of(env, diff_view::commit, "gone.bin", {}, 'D', sha), true) };
        REQUIRE(sides.ok);
        CHECK(sides.left.present);
        CHECK_FALSE(sides.right.present);
        CHECK(env.read(sides.left.file) == binary_bytes('g'));
        CHECK(env.read(sides.right.file).empty());
    }
}

TEST_CASE("integration: diff.tool is read from git config", "[diff][integration][gateway]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    const environment env {};
    diff_temp_directory temp {};
    git_diff_gateway gateway { *env.runner, env.tool.executable, temp };

    CHECK_FALSE(gateway.configured_diff_tool(u8_of(env.repository)).has_value());
    git_ok(env.repository, { "config", "diff.tool", "meld" });
    CHECK(gateway.configured_diff_tool(u8_of(env.repository)) == "meld");
}

TEST_CASE("integration: temporary directories live until the app ends and stale ones are swept", "[diff][integration][temp]")
{
    const test::temp_directory scratch {};
    const std::filesystem::path root { scratch.path() / "cyella-diff" };

    // 오래전에 남은 다른 프로세스의 폴더와 방금 만든 폴더다.
    std::filesystem::create_directories(root / "pold" / "1");
    std::filesystem::create_directories(root / "pfresh" / "1");
    std::filesystem::create_directories(root / "keep-me");
    std::filesystem::last_write_time(root / "pold", std::filesystem::file_time_type::clock::now() - std::chrono::hours { 48 });
    std::filesystem::last_write_time(root / "keep-me", std::filesystem::file_time_type::clock::now() - std::chrono::hours { 48 });

    std::filesystem::path first {};
    std::filesystem::path second {};
    std::filesystem::path process_directory {};
    {
        diff_temp_directory temp { root };
        process_directory = temp.process_directory();
        first = temp.create_operation_directory();
        second = temp.create_operation_directory();
        CHECK(first != second);
        CHECK(std::filesystem::is_directory(first));
        CHECK(std::filesystem::is_directory(second));
        CHECK(first.parent_path() == process_directory);

        // 시작할 때 오래된 폴더만 지운다. 우리 접두어가 아닌 폴더는 건드리지 않는다.
        CHECK_FALSE(std::filesystem::exists(root / "pold"));
        CHECK(std::filesystem::exists(root / "pfresh"));
        CHECK(std::filesystem::exists(root / "keep-me"));
    }
    // 앱이 끝나면(소멸) 이 프로세스의 폴더가 모두 사라진다.
    CHECK_FALSE(std::filesystem::exists(process_directory));
    CHECK_FALSE(std::filesystem::exists(first));
    CHECK(std::filesystem::exists(root / "pfresh"));
}

TEST_CASE("integration: remove_all deletes everything the directory made", "[diff][integration][temp]")
{
    const test::temp_directory scratch {};
    diff_temp_directory temp { scratch.path() / "cyella-diff" };
    const std::filesystem::path operation { temp.create_operation_directory() };
    test::write_file(operation / "left" / "a.bin", "x");
    temp.remove_all();
    CHECK_FALSE(std::filesystem::exists(operation));
}

TEST_CASE("integration: temporary revision copies can be cleaned up while read only", "[diff][integration][temp]")
{
    const test::temp_directory scratch {};
    diff_temp_directory temp { scratch.path() / "cyella-diff" };
    const std::filesystem::path operation { temp.create_operation_directory() };
    const std::filesystem::path file { operation / "right" / "old.cpp" };
    test::write_file(file, "old revision");
    std::filesystem::permissions(file, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace);
    temp.remove_all();
    const bool removed { std::filesystem::exists(operation) == false };
    // 실패해도 검증용 폴더를 읽기 전용으로 남기지 않는다.
    std::error_code ignored {};
    if (removed == false)
        std::filesystem::permissions(file, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ignored);
    CHECK(removed);
}

TEST_CASE("integration: the Win32 launcher starts a detached process with arguments and a working directory", "[diff][integration][launcher]")
{
    const test::temp_directory scratch {};
    const std::unique_ptr<diff_tool_launcher> launcher { win32::make_diff_tool_launcher() };

    // PATH에서 cmd를 찾아 인자가 전달되는지, 작업 폴더가 맞는지 본다.
    const std::filesystem::path output { scratch.path() / "launched.txt" };
    tool_launch_request request {};
    request.executable = u8"cmd";
    request.working_directory = u8_of(scratch.path());
    request.arguments = { u8"/c", u8"echo %CD%> " + u8_of(output) };
    const tool_launch_result result { launcher->launch(request) };
    REQUIRE(result.launched);

    for (int attempt { 0 }; attempt < 100 && std::filesystem::exists(output) == false; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    REQUIRE(std::filesystem::exists(output));
    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
    const std::string written { test::read_file(output) };
    CHECK(written.find(scratch.path().filename().string()) != std::string::npos);

    SECTION("a missing executable is reported")
    {
        request.executable = u8"definitely-not-a-cyella-tool-12345";
        request.arguments.clear();
        const tool_launch_result failed { launcher->launch(request) };
        CHECK_FALSE(failed.launched);
        CHECK_FALSE(failed.error.empty());
    }

    SECTION("an empty executable is reported")
    {
        request.executable.clear();
        CHECK_FALSE(launcher->launch(request).launched);
    }
}

TEST_CASE("integration: opening an external tool extracts files and launches it", "[diff][integration][opener]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    const environment env {};
    env.write("art/pic.psd", binary_bytes('x'));
    env.commit_all("base");
    env.write("art/pic.psd", binary_bytes('y'));

    diff_temp_directory temp {};
    git_diff_gateway gateway { *env.runner, env.tool.executable, temp };
    const std::unique_ptr<diff_tool_launcher> launcher { win32::make_diff_tool_launcher() };
    external_diff_opener opener { gateway, *launcher };

    // cmd로 두 인자를 파일에 적어 템플릿이 실제 파일로 풀렸는지 확인한다.
    const std::filesystem::path report { env.directory.path() / "report.txt" };
    external_open_request request {};
    request.plan = plan_of(env, diff_view::unstaged, "art/pic.psd", "art/pic.psd");
    request.git_executable = env.tool.executable;
    external_diff_tool tool {};
    tool.extensions.push_back(u8".psd");
    tool.executable = u8"cmd";
    tool.arguments = u8"/c copy /b {left} + {right} " + u8_of(report);
    request.tools.push_back(tool);

    const external_open_result result { opener.open(request) };
    REQUIRE(result.status == external_open_status::launched_tool);

    for (int attempt { 0 }; attempt < 100 && std::filesystem::exists(report) == false; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    REQUIRE(std::filesystem::exists(report));
    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
    // left(index)와 right(작업 트리)를 이어 붙인 결과다.
    CHECK(test::read_file(report) == binary_bytes('x') + binary_bytes('y'));
}

namespace {
    // 실제 git과 git_workspace를 수동 dispatcher로 돌리는 최소 환경이다.
    class live_workspace
    {
    public:
        explicit live_workspace(const environment& env)
            : files_ { win32::make_workspace_files() }
            , executor_ { *env.runner, env.tool.executable, *files_, {} }
        {
            git_workspace_options options {};
            options.refresh_on_open = false;
            workspace_ = std::make_unique<git_workspace>(executor_, dispatcher_.dispatcher(), events_.sink(), nullptr, options);
        }

        std::string open(const std::filesystem::path& path)
        {
            const std::string id { workspace_->open_repository(path.generic_u8string()) };
            settle();
            return id;
        }

        operation_id run(const std::string& repository, git_operation operation)
        {
            const operation_id id { workspace_->submit(repository, std::move(operation)) };
            REQUIRE(id != 0);
            settle();
            return id;
        }

        void settle()
        {
            for (int round { 0 }; round < 64; ++round)
            {
                while (dispatcher_.run())
                {
                }
                const std::vector<operation_event> taken { events_.take() };
                if (taken.empty() && dispatcher_.pending() == 0)
                    return;
                for (const operation_event& event : taken)
                    workspace_->apply(event);
            }
            FAIL("operations did not settle");
        }

        [[nodiscard]] const repository_session& session(const std::string& repository) const
        {
            const repository_session* const found { workspace_->find_session(repository) };
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace_->find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

    private:
        std::unique_ptr<workspace_files> files_ {};
        git_operation_runner executor_;
        test::manual_dispatcher dispatcher_ {};
        test::event_queue events_ {};
        std::unique_ptr<git_workspace> workspace_ {};
    };

    // 페이지가 보낼 동작 메시지다.
    std::string page_action(const diff_view_controller& controller, json message)
    {
        message["seq"] = controller.content_seq();
        return message.dump();
    }
} // namespace

TEST_CASE("integration: clicking stage on selected lines reaches the index through the workspace", "[diff][integration][end-to-end]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    const environment env {};
    env.write("f.txt", "a1\na2\na3\na4\na5\n");
    env.commit_all("base");
    env.write("f.txt", "a1\nA2\na3\na4\nA5\n");

    live_workspace live { env };
    const std::string repository { live.open(env.repository) };

    diff_view_controller controller {};
    REQUIRE(controller.handle_web_message(R"({"type":"ready","protocol":1})").accepted);

    // 작업 트리 diff를 읽어 보인다.
    const operation_id read_unstaged { live.run(repository, diff_file_request { "f.txt", diff_view::unstaged }) };
    REQUIRE(live.status(read_unstaged).state == operation_state::succeeded);
    REQUIRE(live.session(repository).selected_diff().has_value());
    static_cast<void>(controller.show(*live.session(repository).selected_diff(), u8_of(env.repository)));
    REQUIRE(controller.poll());
    const json shown { json::parse(controller.latest_message()) };
    REQUIRE(shown.at("type") == "show_diff");
    REQUIRE(shown.at("can_stage") == true);

    // hunk 안의 줄: 0 a1, 1 -a2, 2 +A2, 3 a3, 4 a4, 5 -a5, 6 +A5. 첫 교체(-a2 +A2)만 stage한다.
    const web_message_result stage { controller.handle_web_message(page_action(controller, { { "type", "stage_lines" }, { "hunk", 0 }, { "ranges", json::array({ json::array({ 1, 2 }) }) } })) };
    REQUIRE(stage.accepted);
    REQUIRE(stage.effects.size() == 1);
    const auto& apply { std::get<apply_patch_effect>(stage.effects.front()) };
    const operation_id applied { live.run(repository, apply.request) };
    REQUIRE(live.status(applied).state == operation_state::succeeded);
    CHECK(git_output(env.repository, { "show", ":f.txt" }) == "a1\nA2\na3\na4\na5\n");
    CHECK(test::read_file(env.repository / "f.txt") == "a1\nA2\na3\na4\nA5\n");

    // staged diff를 읽어 unstage 한다. 같은 줄을 다시 내리면 index가 원래대로 돌아온다.
    const operation_id read_staged { live.run(repository, diff_file_request { "f.txt", diff_view::staged }) };
    REQUIRE(live.status(read_staged).state == operation_state::succeeded);
    static_cast<void>(controller.show(*live.session(repository).selected_diff(), u8_of(env.repository)));
    REQUIRE(controller.poll());
    const web_message_result unstage { controller.handle_web_message(page_action(controller, { { "type", "unstage_hunk" }, { "hunk", 0 } })) };
    REQUIRE(unstage.accepted);
    const operation_id reverted { live.run(repository, std::get<apply_patch_effect>(unstage.effects.front()).request) };
    REQUIRE(live.status(reverted).state == operation_state::succeeded);
    CHECK(git_output(env.repository, { "show", ":f.txt" }) == "a1\na2\na3\na4\na5\n");
}

TEST_CASE("integration: discarding one hunk restores just that part of the worktree", "[diff][integration][end-to-end]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    std::string before {};
    for (int line { 1 }; line <= 30; ++line)
        before += "l" + std::to_string(line) + "\n";
    std::string after { before };
    after.replace(after.find("l2\n"), 3, "L2\n");
    after.replace(after.find("l28\n"), 4, "L28\n");

    const environment env {};
    env.write("g.txt", before);
    env.commit_all("base");
    env.write("g.txt", after);

    live_workspace live { env };
    const std::string repository { live.open(env.repository) };
    diff_view_controller controller {};
    REQUIRE(controller.handle_web_message(R"({"type":"ready","protocol":1})").accepted);
    live.run(repository, diff_file_request { "g.txt", diff_view::unstaged });
    static_cast<void>(controller.show(*live.session(repository).selected_diff(), u8_of(env.repository)));
    REQUIRE(controller.poll());
    REQUIRE(json::parse(controller.latest_message()).at("file").at("hunks").size() == 2);

    const web_message_result discard { controller.handle_web_message(page_action(controller, { { "type", "discard_hunk" }, { "hunk", 1 } })) };
    REQUIRE(discard.accepted);
    const operation_id id { live.run(repository, std::get<apply_patch_effect>(discard.effects.front()).request) };
    REQUIRE(live.status(id).state == operation_state::succeeded);

    std::string expected { before };
    expected.replace(expected.find("l2\n"), 3, "L2\n");
    CHECK(test::read_file(env.repository / "g.txt") == expected);
}
