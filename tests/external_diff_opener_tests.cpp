#include "application/diff_sides.h"
#include "application/external_diff_opener.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {
    using namespace cyella;

    // 실행하지 않고 부른 내용만 기록하는 가짜 git이다.
    class fake_gateway final : public diff_git_gateway
    {
    public:
        extracted_sides extract(const diff_side_plan& plan, const bool write_files) override
        {
            extractions.push_back(plan);
            last_write_files = write_files;
            return result;
        }

        std::optional<std::string> configured_diff_tool(const std::u8string_view repository_path) override
        {
            asked_repositories.emplace_back(repository_path);
            return difftool;
        }

        std::vector<diff_side_plan> extractions {};
        std::vector<std::u8string> asked_repositories {};
        bool last_write_files { false };
        extracted_sides result {};
        std::optional<std::string> difftool {};
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

    extracted_sides two_files()
    {
        extracted_sides sides {};
        sides.ok = true;
        sides.left = { true, u8"C:\\Temp\\op\\left\\a b.psd", 10 };
        sides.right = { true, u8"C:\\Temp\\op\\right\\a b.psd", 20 };
        return sides;
    }

    diff_side_plan plan_for(const std::string& path, const diff_view view = diff_view::unstaged)
    {
        diff_side_plan plan {};
        plan.repository_path = u8"C:\\repo";
        plan.view = view;
        plan.sha = "abc123";
        plan.old_path = path;
        plan.new_path = path;
        return plan;
    }

    external_diff_tool tool(std::u8string extension, std::u8string executable, std::u8string arguments)
    {
        external_diff_tool entry {};
        entry.extensions.push_back(std::move(extension));
        entry.executable = std::move(executable);
        entry.arguments = std::move(arguments);
        return entry;
    }
} // namespace

TEST_CASE("file apps open the selected version without running a shell", "[diff][external][file_app]")
{
    fake_gateway gateway {};
    gateway.result = two_files();
    fake_launcher launcher {};
    external_diff_opener opener { gateway, launcher };
    external_open_request request {};
    request.plan = plan_for("art/a b.psd", diff_view::staged);
    request.file_app = external_file_app { u8"Editor", u8"editor.exe", u8"--file={file} --root {repository} {path}" };

    SECTION("staged and historical versions use the extracted right side")
    {
        for (const diff_view view : { diff_view::staged, diff_view::commit })
        {
            request.plan.view = view;
            REQUIRE(opener.open(request).launched());
            REQUIRE(launcher.requests.back().arguments.size() == 4);
            CHECK(launcher.requests.back().arguments[0] == u8"--file=C:\\Temp\\op\\right\\a b.psd");
            CHECK(launcher.requests.back().arguments[2] == u8"C:\\repo");
            CHECK(gateway.extractions.back().view == view);
        }
    }
    SECTION("a deleted file opens the previous version")
    {
        request.plan.status = 'D';
        request.plan.new_path.clear();
        gateway.result.right = {};
        REQUIRE(opener.open(request).launched());
        CHECK(launcher.requests[0].arguments[0] == u8"--file=C:\\Temp\\op\\left\\a b.psd");
    }
    SECTION("worktree and untracked files open the actual file")
    {
        test::temporary_directory directory { "file-app" };
        const auto file { directory.path() / L"src" / L"한 글.cpp" };
        test::write_file(file, "working content");
        request.plan.repository_path = directory.u8path();
        request.plan.view = diff_view::unstaged;
        request.plan.new_path = "src/한 글.cpp";
        request.plan.untracked = true;
        REQUIRE(opener.open(request).launched());
        CHECK(std::filesystem::path { launcher.requests[0].arguments[0].substr(7) } == file);
        CHECK(gateway.extractions.empty());
        CHECK(test::read_file(file) == "working content");
    }
    SECTION("failed extraction, invalid templates and cancellation do not launch")
    {
        gateway.result.ok = false;
        CHECK_FALSE(opener.open(request).launched());
        gateway.result = two_files();
        request.file_app->arguments = u8"\"{file}";
        CHECK(opener.open(request).status == external_open_status::invalid_template);
        process_cancellation_source cancellation {};
        cancellation.request_cancellation();
        request.cancellation = cancellation.token();
        CHECK_FALSE(opener.open(request).launched());
        CHECK(launcher.requests.empty());
    }
    SECTION("a repository-only app does not need a file extraction")
    {
        request.file_app->arguments = u8"{repository}";
        gateway.result.ok = false;
        REQUIRE(opener.open(request).launched());
        CHECK(gateway.extractions.empty());
        CHECK(launcher.requests[0].arguments == std::vector<std::u8string> { u8"C:\\repo" });
    }
    SECTION("a worktree directory such as a submodule can be passed to an editor")
    {
        test::temporary_directory directory { "file-app-folder" };
        const auto folder { directory.path() / "nested-repository" };
        std::filesystem::create_directories(folder);
        request.plan.repository_path = directory.u8path();
        request.plan.view = diff_view::unstaged;
        request.plan.new_path = "nested-repository";
        REQUIRE(opener.open(request).launched());
        CHECK(std::filesystem::path { launcher.requests[0].arguments[0].substr(7) } == folder);
        CHECK(gateway.extractions.empty());
    }
}

TEST_CASE("resolve_diff_sides picks the two sides of each view", "[diff][external]")
{
    diff_side_plan plan { plan_for("src/a.cpp") };

    SECTION("unstaged compares the index with the worktree")
    {
        const diff_side_pair pair { resolve_diff_sides(plan) };
        CHECK(pair.left == diff_side_spec { diff_side_kind::index, {}, "src/a.cpp" });
        CHECK(pair.right == diff_side_spec { diff_side_kind::worktree, {}, "src/a.cpp" });
    }

    SECTION("staged compares HEAD with the index")
    {
        plan.view = diff_view::staged;
        const diff_side_pair pair { resolve_diff_sides(plan) };
        CHECK(pair.left == diff_side_spec { diff_side_kind::revision, "HEAD", "src/a.cpp" });
        CHECK(pair.right == diff_side_spec { diff_side_kind::index, {}, "src/a.cpp" });
    }

    SECTION("a commit compares its first parent with itself")
    {
        plan.view = diff_view::commit;
        const diff_side_pair pair { resolve_diff_sides(plan) };
        CHECK(pair.left == diff_side_spec { diff_side_kind::revision, "abc123^", "src/a.cpp" });
        CHECK(pair.right == diff_side_spec { diff_side_kind::revision, "abc123", "src/a.cpp" });
    }

    SECTION("an added file has no left side and a deleted file has no right side")
    {
        plan.status = 'A';
        plan.old_path.clear();
        plan.view = diff_view::staged;
        CHECK(resolve_diff_sides(plan).left.kind == diff_side_kind::none);
        CHECK(resolve_diff_sides(plan).right.kind == diff_side_kind::index);

        plan.status = 'D';
        plan.old_path = "src/a.cpp";
        plan.new_path.clear();
        CHECK(resolve_diff_sides(plan).left.kind == diff_side_kind::revision);
        CHECK(resolve_diff_sides(plan).right.kind == diff_side_kind::none);
    }

    SECTION("an untracked file has no left side")
    {
        plan.status = 'A';
        plan.old_path.clear();
        plan.untracked = true;
        CHECK(resolve_diff_sides(plan).left.kind == diff_side_kind::none);
        CHECK(resolve_diff_sides(plan).right.kind == diff_side_kind::worktree);
    }

    SECTION("a renamed file reads its left side under the old name")
    {
        plan.status = 'R';
        plan.old_path = "old.txt";
        plan.new_path = "new.txt";
        plan.view = diff_view::commit;
        CHECK(resolve_diff_sides(plan).left.path == "old.txt");
        CHECK(resolve_diff_sides(plan).right.path == "new.txt");
    }
}

TEST_CASE("is_image_path goes by the extension", "[diff][external]")
{
    for (const char* path : { "a.png", "dir/B.JPG", "x.jpeg", "x.gif", "x.webp", "x.bmp", "icon.svg", "favicon.ICO" })
        CHECK(is_image_path(path));
    for (const char* path : { "a.txt", "png", "a.png.bak", "dir.png/readme", "a.psd", "" })
        CHECK_FALSE(is_image_path(path));
}

TEST_CASE("a tool from the extension table gets the two extracted files", "[diff][external]")
{
    fake_gateway gateway {};
    gateway.result = two_files();
    fake_launcher launcher {};
    external_diff_opener opener { gateway, launcher };

    external_open_request request {};
    request.plan = plan_for("art/a b.psd");
    request.git_executable = u8"C:\\git\\git.exe";
    request.tools.push_back(tool(u8".txt", u8"notepad", u8"{left}"));
    request.tools.push_back(tool(u8".psd", u8"C:\\Tools\\diff.exe", u8"--base {base} \"{left}\" \"{right}\" --title {path}"));

    const external_open_result result { opener.open(request) };
    CHECK(result.status == external_open_status::launched_tool);
    CHECK(result.launched());
    CHECK(result.tool == u8"C:\\Tools\\diff.exe");
    REQUIRE(gateway.extractions.size() == 1);
    CHECK(gateway.last_write_files);
    REQUIRE(launcher.requests.size() == 1);
    const tool_launch_request& launched { launcher.requests.front() };
    CHECK(launched.executable == u8"C:\\Tools\\diff.exe");
    CHECK(launched.working_directory == u8"C:\\repo");
    // {base}는 값이 없어 인자째 빠지고, 공백이 든 경로는 한 인자로 남는다.
    const std::vector<std::u8string> expected { u8"--base", u8"C:\\Temp\\op\\left\\a b.psd", u8"C:\\Temp\\op\\right\\a b.psd", u8"--title", u8"art/a b.psd" };
    CHECK(launched.arguments == expected);
}

TEST_CASE("the table wins over diff.tool", "[diff][external]")
{
    fake_gateway gateway {};
    gateway.result = two_files();
    gateway.difftool = "meld";
    fake_launcher launcher {};
    external_diff_opener opener { gateway, launcher };

    external_open_request request {};
    request.plan = plan_for("a.psd");
    request.git_executable = u8"C:\\git\\git.exe";
    request.tools.push_back(tool(u8".psd", u8"viewer", u8"{left} {right}"));
    CHECK(opener.open(request).status == external_open_status::launched_tool);
    CHECK(gateway.asked_repositories.empty());
}

TEST_CASE("without a table entry diff.tool is handed to git difftool", "[diff][external]")
{
    fake_gateway gateway {};
    gateway.difftool = "meld";
    fake_launcher launcher {};
    external_diff_opener opener { gateway, launcher };

    external_open_request request {};
    request.plan = plan_for("bin/data.bin");
    request.git_executable = u8"C:\\git\\git.exe";
    request.tools.push_back(tool(u8".psd", u8"viewer", u8"{left} {right}"));

    SECTION("unstaged")
    {
        const external_open_result result { opener.open(request) };
        CHECK(result.status == external_open_status::launched_difftool);
        REQUIRE(launcher.requests.size() == 1);
        CHECK(launcher.requests.front().executable == u8"C:\\git\\git.exe");
        CHECK(launcher.requests.front().working_directory == u8"C:\\repo");
        const std::vector<std::u8string> expected { u8"--literal-pathspecs", u8"difftool", u8"--no-prompt", u8"--", u8"bin/data.bin" };
        CHECK(launcher.requests.front().arguments == expected);
        // 두 쪽을 따로 꺼내지 않는다. git이 한다.
        CHECK(gateway.extractions.empty());
    }

    SECTION("staged")
    {
        request.plan.view = diff_view::staged;
        CHECK(opener.open(request).status == external_open_status::launched_difftool);
        const std::vector<std::u8string> expected { u8"--literal-pathspecs", u8"difftool", u8"--no-prompt", u8"--cached", u8"--", u8"bin/data.bin" };
        CHECK(launcher.requests.front().arguments == expected);
    }

    SECTION("commit")
    {
        request.plan.view = diff_view::commit;
        CHECK(opener.open(request).status == external_open_status::launched_difftool);
        const std::vector<std::u8string> expected { u8"--literal-pathspecs", u8"difftool", u8"--no-prompt", u8"abc123^", u8"abc123", u8"--", u8"bin/data.bin" };
        CHECK(launcher.requests.front().arguments == expected);
    }
}

TEST_CASE("no tool at all is reported, not guessed", "[diff][external]")
{
    fake_gateway gateway {};
    fake_launcher launcher {};
    external_diff_opener opener { gateway, launcher };

    external_open_request request {};
    request.plan = plan_for("bin/data.bin");
    request.git_executable = u8"C:\\git\\git.exe";

    SECTION("neither table nor diff.tool")
    {
        const external_open_result result { opener.open(request) };
        CHECK(result.status == external_open_status::no_tool);
        CHECK_FALSE(result.launched());
        CHECK(launcher.requests.empty());
    }

    SECTION("git difftool cannot compare an untracked file")
    {
        gateway.difftool = "meld";
        request.plan.untracked = true;
        CHECK(opener.open(request).status == external_open_status::no_tool);
        CHECK(launcher.requests.empty());
    }

    SECTION("no git executable to run difftool with")
    {
        gateway.difftool = "meld";
        request.git_executable.clear();
        CHECK(opener.open(request).status == external_open_status::no_tool);
    }
}

TEST_CASE("failures are reported without launching", "[diff][external]")
{
    fake_gateway gateway {};
    gateway.result = two_files();
    fake_launcher launcher {};
    external_diff_opener opener { gateway, launcher };

    external_open_request request {};
    request.plan = plan_for("a.psd");
    request.git_executable = u8"C:\\git\\git.exe";
    request.tools.push_back(tool(u8".psd", u8"viewer", u8"{left} {right}"));

    SECTION("extraction failed")
    {
        gateway.result.ok = false;
        gateway.result.error = "git cat-file failed";
        const external_open_result result { opener.open(request) };
        CHECK(result.status == external_open_status::extraction_failed);
        CHECK(result.detail == "git cat-file failed");
        CHECK(launcher.requests.empty());
    }

    SECTION("template with an unclosed quote")
    {
        request.tools.clear();
        request.tools.push_back(tool(u8".psd", u8"viewer", u8"\"{left}"));
        CHECK(opener.open(request).status == external_open_status::invalid_template);
        CHECK(launcher.requests.empty());
    }

    SECTION("launch failed")
    {
        launcher.outcome = { false, "not found" };
        const external_open_result result { opener.open(request) };
        CHECK(result.status == external_open_status::launch_failed);
        CHECK(result.detail == "not found");
        CHECK_FALSE(result.launched());
    }

    SECTION("difftool launch failed")
    {
        request.tools.clear();
        gateway.difftool = "meld";
        launcher.outcome = { false, "git missing" };
        CHECK(opener.open(request).status == external_open_status::launch_failed);
    }
}

TEST_CASE("a folder opens in an app without extracting any file", "[diff][external][file_app]")
{
    fake_gateway gateway {};
    fake_launcher launcher {};
    external_diff_opener opener { gateway, launcher };
    external_open_request request {};
    request.folder = u8"C:\\work\\한글 저장소";

    SECTION("{repository} and {file} are both the folder and it is the working directory")
    {
        request.file_app = external_file_app { u8"Editor", u8"editor.exe", u8"--root {repository} {file}" };
        const external_open_result result { opener.open(request) };
        REQUIRE(result.launched());
        CHECK(result.tool == u8"Editor");
        REQUIRE(launcher.requests.size() == 1);
        CHECK(launcher.requests[0].executable == u8"editor.exe");
        CHECK(launcher.requests[0].arguments == std::vector<std::u8string> { u8"--root", u8"C:\\work\\한글 저장소", u8"C:\\work\\한글 저장소" });
        CHECK(launcher.requests[0].working_directory == u8"C:\\work\\한글 저장소");
        CHECK(gateway.extractions.empty());
    }
    SECTION("invalid templates and launch failures are reported")
    {
        request.file_app = external_file_app { u8"Broken", u8"broken.exe", u8"\"{repository}" };
        CHECK(opener.open(request).status == external_open_status::invalid_template);
        CHECK(launcher.requests.empty());
        request.file_app->arguments = u8"{repository}";
        launcher.outcome = { false, "not found" };
        const external_open_result failed { opener.open(request) };
        CHECK(failed.status == external_open_status::launch_failed);
        CHECK(failed.detail == "not found");
    }
}
