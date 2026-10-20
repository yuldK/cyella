// submodule·worktree·Git LFS를 실제 git으로 끝까지 돌린다. 실행기는 실제 Win32 process runner를 쓰고 job은
// 수동 dispatcher가 test 스레드에서 차례로 실행한다. 모두 `[integration]` 태그를 가지며 git이 없으면, LFS는 git-lfs가
// 없으면 건너뛴다.
#include "application/git_workspace.h"
#include "application/repository_tabs.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_workspace_files.h"

#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::run_git;
    using cyella::test::text_list;

    std::string trimmed(std::string text)
    {
        while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    }

    void git_ok(const std::filesystem::path& directory, const text_list& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        INFO("git " << [&arguments] {
            std::string joined {};
            for (const std::string& argument : arguments)
                joined.append(argument).push_back(' ');
            return joined;
        }());
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
    }

    std::string git_text(const std::filesystem::path& directory, const text_list& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
        return trimmed(result.standard_output);
    }

    void commit_file(const std::filesystem::path& repo, const std::string& path, const std::string_view content, const std::string& message)
    {
        test::write_file(repo / std::filesystem::path { std::u8string { path.begin(), path.end() } }, content);
        git_ok(repo, { "add", "--", path });
        git_ok(repo, { "commit", "-q", "-m", message });
    }

    std::filesystem::path make_repository(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path);
        git_ok(path, { "init", "-q", "-b", "main" });
        return path;
    }

    std::string slashes(const std::filesystem::path& path)
    {
        const std::u8string text { path.generic_u8string() };
        return std::string { text.begin(), text.end() };
    }

    bool lfs_is_available()
    {
        return test::git_is_available() && run_git(std::filesystem::temp_directory_path(), text_list { "lfs", "version" }).exit_code == 0;
    }

    class live_workspace
    {
    public:
        live_workspace()
            : process_ { win32::make_process_runner() }
            , files_ { win32::make_workspace_files() }
        {
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool_ = resolve_git_tool({}, win32::current_git_tool_environment(), *process_, *probe, {});
            REQUIRE(tool_.usable());
            executor_ = std::make_unique<git_operation_runner>(*process_, tool_.executable, *files_);
            git_workspace_options workspace_options {};
            workspace_options.refresh_on_open = false;
            workspace_ = std::make_unique<git_workspace>(*executor_, dispatcher_.dispatcher(), events_.sink(), nullptr, workspace_options);
        }

        std::string open(const std::filesystem::path& path)
        {
            const std::string id { workspace_->open_repository(path.generic_u8string()) };
            refresh(id);
            return id;
        }

        operation_id run(const std::string& repository, git_operation operation)
        {
            const operation_id id { workspace_->submit(repository, std::move(operation)) };
            REQUIRE(id != 0);
            settle();
            return id;
        }

        void refresh(const std::string& repository)
        {
            static_cast<void>(run(repository, refresh_request {}));
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

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace_->find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] const repository_session& session(const std::string& repository) const
        {
            const repository_session* const found { workspace_->find_session(repository) };
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] const repository_snapshot& snapshot(const std::string& repository) const
        {
            return session(repository).snapshot();
        }

        void require_succeeded(const operation_id id) const
        {
            const operation_status& found { status(id) };
            if (found.failure.has_value())
            {
                const std::string summary { found.failure->summary.begin(), found.failure->summary.end() };
                const std::string detail { found.failure->detail.begin(), found.failure->detail.end() };
                INFO("failure: " << summary);
                INFO("detail: " << detail);
                REQUIRE(found.state == operation_state::succeeded);
            }
            REQUIRE(found.state == operation_state::succeeded);
        }

    private:
        std::unique_ptr<process_runner> process_ {};
        std::unique_ptr<workspace_files> files_ {};
        git_tool_info tool_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        test::manual_dispatcher dispatcher_ {};
        test::event_queue events_ {};
        std::unique_ptr<git_workspace> workspace_ {};
    };

    const submodule_info* find_submodule(const repository_snapshot& snapshot, const std::string& path)
    {
        for (const submodule_info& info : snapshot.submodules)
            if (info.path == path)
                return &info;
        return nullptr;
    }

    const worktree_info* find_worktree(const repository_snapshot& snapshot, const std::filesystem::path& path)
    {
        const std::string wanted { slashes(path) };
        for (const worktree_info& info : snapshot.worktrees)
            if (info.path.size() == wanted.size()
                && std::equal(info.path.begin(), info.path.end(), wanted.begin(), [](const char left, const char right) { return std::tolower(left) == std::tolower(right); }))
                return &info;
        return nullptr;
    }

    const ref* find_local_ref(const repository_snapshot& snapshot, const std::string& name)
    {
        for (const ref& value : snapshot.refs)
            if (value.kind == ref_kind::local && value.name == name)
                return &value;
        return nullptr;
    }

    // 상위 저장소 하나와 submodule 둘(`libs/a`, `libs/b`)이다. 작업 복사본 `work`는 a만 초기화했다.
    struct submodule_fixture
    {
        submodule_fixture()
        {
            for (const char* const name : { "a", "b" })
            {
                const std::filesystem::path sub { make_repository(directory.path() / name) };
                commit_file(sub, std::string { name } + ".txt", name, std::string { name } + " one");
                commit_file(sub, std::string { name } + ".txt", std::string { name } + "2", std::string { name } + " two");
            }
            git_ok(directory.path(), { "init", "-q", "-b", "main", upstream.string() });
            commit_file(upstream, "s.txt", "super", "super one");
            git_ok(upstream, { "submodule", "add", "-q", "../a", "libs/a" });
            git_ok(upstream, { "submodule", "add", "-q", "../b", "libs/b" });
            git_ok(upstream, { "commit", "-q", "-m", "add submodules" });
            git_ok(directory.path(), { "clone", "-q", upstream.string(), work.string() });
            git_ok(work, { "submodule", "update", "-q", "--init", "libs/a" });
        }

        test::temp_directory directory {};
        std::filesystem::path upstream { directory.path() / "upstream" };
        std::filesystem::path work { directory.path() / "work" };
    };
} // namespace

// ---------------------------------------------------------------- submodule

TEST_CASE("integration: submodules are listed with their state and update --init fills the uninitialized one", "[integration][submodule]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    submodule_fixture fixture {};
    live_workspace live {};
    const std::string repo { live.open(fixture.work) };

    REQUIRE(live.snapshot(repo).submodules.size() == 2);
    const submodule_info* const initialized { find_submodule(live.snapshot(repo), "libs/a") };
    const submodule_info* const uninitialized { find_submodule(live.snapshot(repo), "libs/b") };
    REQUIRE(initialized != nullptr);
    REQUIRE(uninitialized != nullptr);
    CHECK(initialized->state == submodule_state::initialized);
    CHECK(initialized->url == "../a");
    CHECK(initialized->recorded_sha == initialized->checked_out_sha);
    CHECK(initialized->checked_out_sha == git_text(fixture.work / "libs" / "a", { "rev-parse", "HEAD" }));
    CHECK(uninitialized->state == submodule_state::uninitialized);
    CHECK(uninitialized->checked_out_sha.empty());
    CHECK(uninitialized->recorded_sha == git_text(fixture.upstream / "libs" / "b", { "rev-parse", "HEAD" }));
    CHECK_FALSE(std::filesystem::exists(fixture.work / "libs" / "b" / ".git"));

    submodule_update_request update {};
    update.paths = { "libs/b" };
    live.require_succeeded(live.run(repo, update));
    const submodule_info* const updated { find_submodule(live.snapshot(repo), "libs/b") };
    REQUIRE(updated != nullptr);
    CHECK(updated->state == submodule_state::initialized);
    CHECK(updated->checked_out_sha == uninitialized->recorded_sha);
    CHECK(std::filesystem::exists(fixture.work / "libs" / "b" / ".git"));
}

TEST_CASE("integration: a submodule that moved ahead shows as a pointer change in the working area", "[integration][submodule][diff]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    submodule_fixture fixture {};
    live_workspace live {};
    const std::string repo { live.open(fixture.work) };
    const std::string recorded { find_submodule(live.snapshot(repo), "libs/a")->recorded_sha };

    commit_file(fixture.work / "libs" / "a", "new.txt", "new", "inside the submodule");
    live.refresh(repo);

    const submodule_info* const ahead { find_submodule(live.snapshot(repo), "libs/a") };
    REQUIRE(ahead != nullptr);
    CHECK(ahead->state == submodule_state::modified);
    CHECK(ahead->recorded_sha == recorded);
    CHECK(ahead->checked_out_sha != recorded);

    const auto change { std::find_if(live.snapshot(repo).unstaged.begin(), live.snapshot(repo).unstaged.end(), [](const file_change& value) { return value.path == "libs/a"; }) };
    REQUIRE(change != live.snapshot(repo).unstaged.end());
    CHECK(change->is_submodule);
    CHECK(change->status == 'M');
    CHECK(change->submodule_flags.substr(0, 2) == "SC");

    // 포인터 변경의 diff는 `Subproject commit` 두 줄이고 모드가 160000이다.
    const operation_id diff { live.run(repo, diff_file_request { .path = "libs/a" }) };
    live.require_succeeded(diff);
    REQUIRE(live.session(repo).selected_diff().has_value());
    REQUIRE(live.session(repo).selected_diff()->files.size() == 1);
    const diff_file& file { live.session(repo).selected_diff()->files.front() };
    CHECK(file.new_mode == "160000");
    REQUIRE(file.hunks.size() == 1);
    CHECK(file.hunks.front().lines.back().content == "Subproject commit " + ahead->checked_out_sha);
    CHECK_FALSE(file.lfs.has_value());
}

TEST_CASE("integration: a submodule behind the recorded commit is out of date and sync follows .gitmodules", "[integration][submodule]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    submodule_fixture fixture {};
    // 상위 저장소가 a의 새 커밋을 기록한다.
    commit_file(fixture.directory.path() / "a", "a.txt", "a3", "a three");
    git_ok(fixture.upstream / "libs" / "a", { "pull", "-q" });
    git_ok(fixture.upstream, { "add", "libs/a" });
    git_ok(fixture.upstream, { "commit", "-q", "-m", "bump a" });
    git_ok(fixture.work, { "pull", "-q" });

    live_workspace live {};
    const std::string repo { live.open(fixture.work) };
    const submodule_info* const behind { find_submodule(live.snapshot(repo), "libs/a") };
    REQUIRE(behind != nullptr);
    CHECK(behind->state == submodule_state::out_of_date);
    CHECK(behind->recorded_sha == git_text(fixture.upstream / "libs" / "a", { "rev-parse", "HEAD" }));
    CHECK(behind->checked_out_sha != behind->recorded_sha);

    submodule_update_request update {};
    update.paths = { "libs/a" };
    live.require_succeeded(live.run(repo, update));
    CHECK(find_submodule(live.snapshot(repo), "libs/a")->state == submodule_state::initialized);

    // .gitmodules의 url을 바꾼 뒤 sync하면 submodule 설정이 따라온다.
    git_ok(fixture.work, { "config", "-f", ".gitmodules", "submodule.libs/a.url", "../moved" });
    const std::string before { git_text(fixture.work, { "config", "--get", "submodule.libs/a.url" }) };
    live.require_succeeded(live.run(repo, submodule_sync_request {}));
    // 상대 url은 origin 기준의 절대 경로로 풀려 설정에 들어간다.
    const std::string after { git_text(fixture.work, { "config", "--get", "submodule.libs/a.url" }) };
    CHECK(after != before);
    CHECK(after.size() >= 6);
    CHECK(after.substr(after.size() - 6) == "/moved");
}

// ---------------------------------------------------------------- worktree

TEST_CASE("integration: worktrees are added, listed, guard their branch and are removed", "[integration][worktree]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo_path { make_repository(directory.path() / "repo") };
    commit_file(repo_path, "a.txt", "one", "one");
    git_ok(repo_path, { "branch", "feat" });
    live_workspace live {};
    const std::string repo { live.open(repo_path) };

    REQUIRE(live.snapshot(repo).worktrees.size() == 1);
    CHECK(live.snapshot(repo).worktrees.front().is_main);
    CHECK(live.snapshot(repo).worktrees.front().is_current);

    // 기존 브랜치를 새 폴더에 체크아웃한다.
    const std::filesystem::path feature_path { directory.path() / "feat-tree" };
    worktree_add_request add {};
    add.path = feature_path.u8string();
    add.branch = "feat";
    live.require_succeeded(live.run(repo, add));
    REQUIRE(live.snapshot(repo).worktrees.size() == 2);
    const worktree_info* const linked { find_worktree(live.snapshot(repo), feature_path) };
    REQUIRE(linked != nullptr);
    CHECK(linked->branch == "feat");
    CHECK_FALSE(linked->is_main);
    CHECK_FALSE(linked->is_current);
    CHECK(std::filesystem::exists(feature_path / "a.txt"));

    // 그 브랜치에는 다른 worktree 표시가 달리고, 체크아웃은 git이 거절한 이유로 실패한다.
    const ref* const marked { find_local_ref(live.snapshot(repo), "feat") };
    REQUIRE(marked != nullptr);
    CHECK_FALSE(marked->worktree_path.empty());
    CHECK(find_local_ref(live.snapshot(repo), "main")->worktree_path.empty());
    const operation_id refused { live.run(repo, checkout_branch_request { .name = "feat" }) };
    REQUIRE(live.status(refused).state == operation_state::failed);
    REQUIRE(live.status(refused).failure.has_value());
    CHECK(live.status(refused).failure->kind == failure_kind::branch_checked_out_elsewhere);
    const std::string summary { live.status(refused).failure->summary.begin(), live.status(refused).failure->summary.end() };
    CHECK(summary.find("feat-tree") != std::string::npos);

    // 새 브랜치를 커밋에서 만들며 추가하고, detached로도 추가한다.
    const std::filesystem::path topic_path { directory.path() / "topic-tree" };
    worktree_add_request topic {};
    topic.path = topic_path.u8string();
    topic.new_branch = "topic";
    topic.start_point = "main";
    live.require_succeeded(live.run(repo, topic));
    CHECK(git_text(topic_path, { "branch", "--show-current" }) == "topic");

    const std::filesystem::path detached_path { directory.path() / "detached-tree" };
    worktree_add_request detached {};
    detached.path = detached_path.u8string();
    detached.detach = true;
    live.require_succeeded(live.run(repo, detached));
    const worktree_info* const detached_info { find_worktree(live.snapshot(repo), detached_path) };
    REQUIRE(detached_info != nullptr);
    CHECK(detached_info->detached);

    // 이미 쓰는 브랜치로 만들려 해도 같은 이유로 실패한다.
    worktree_add_request again {};
    again.path = (directory.path() / "again-tree").u8string();
    again.branch = "feat";
    const operation_id duplicate { live.run(repo, again) };
    REQUIRE(live.status(duplicate).failure.has_value());
    CHECK(live.status(duplicate).failure->kind == failure_kind::branch_checked_out_elsewhere);

    // 잠긴 작업 트리는 강제해야 지워지고, 주 작업 트리는 어떤 경우에도 거절한다.
    git_ok(repo_path, { "worktree", "lock", "--reason", "usb", detached_path.string() });
    live.refresh(repo);
    CHECK(find_worktree(live.snapshot(repo), detached_path)->locked);
    const operation_id locked { live.run(repo, worktree_remove_request { detached_path.u8string(), false }) };
    REQUIRE(live.status(locked).failure.has_value());
    CHECK(live.status(locked).failure->kind == failure_kind::refused);
    CHECK(std::filesystem::exists(detached_path));
    live.require_succeeded(live.run(repo, worktree_remove_request { detached_path.u8string(), true }));
    CHECK_FALSE(std::filesystem::exists(detached_path));

    const operation_id main_removal { live.run(repo, worktree_remove_request { repo_path.u8string(), true }) };
    REQUIRE(live.status(main_removal).failure.has_value());
    CHECK(live.status(main_removal).failure->kind == failure_kind::refused);

    live.require_succeeded(live.run(repo, worktree_remove_request { feature_path.u8string(), false }));
    CHECK_FALSE(std::filesystem::exists(feature_path));
    CHECK(find_local_ref(live.snapshot(repo), "feat")->worktree_path.empty());
    // 지운 뒤에는 그 브랜치로 체크아웃할 수 있다.
    live.require_succeeded(live.run(repo, checkout_branch_request { .name = "feat" }));
}

TEST_CASE("integration: prune forgets a worktree whose folder is gone, and a linked worktree is its own repository", "[integration][worktree]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo_path { make_repository(directory.path() / "repo") };
    commit_file(repo_path, "a.txt", "one", "one");
    live_workspace live {};
    const std::string repo { live.open(repo_path) };

    const std::filesystem::path linked_path { directory.path() / "Linked Tree" };
    worktree_add_request add {};
    add.path = linked_path.u8string();
    add.new_branch = "linked";
    live.require_succeeded(live.run(repo, add));

    // 연결된 worktree를 열면 자기 경로·자기 브랜치를 가진 별개의 저장소다.
    const std::string linked { live.open(linked_path) };
    CHECK(linked != repo);
    CHECK(live.snapshot(linked).summary.current_branch == "linked");
    CHECK(live.snapshot(repo).summary.current_branch == "main");
    REQUIRE(live.snapshot(linked).worktrees.size() == 2);
    CHECK(find_worktree(live.snapshot(linked), linked_path)->is_current);
    CHECK(find_worktree(live.snapshot(linked), repo_path)->is_main);
    // 이쪽에서는 main이 다른 worktree의 브랜치다.
    CHECK_FALSE(find_local_ref(live.snapshot(linked), "main")->worktree_path.empty());
    CHECK(find_local_ref(live.snapshot(linked), "linked")->worktree_path.empty());

    // 탭과 최근 목록은 경로로 같은 저장소를 가르므로 두 worktree는 두 탭이다.
    repository_tabs tabs {};
    const open_repository_result first { tabs.open_repository(repo_path.u8string()) };
    const open_repository_result second { tabs.open_repository(linked_path.u8string()) };
    CHECK(first.id != second.id);
    CHECK(second.created);
    CHECK(tabs.tabs().size() == 2);
    // 대소문자와 구분자가 달라도 같은 worktree는 기존 탭이다.
    std::u8string shouting { linked_path.generic_u8string() };
    std::transform(shouting.begin(), shouting.end(), shouting.begin(), [](const char8_t value) { return static_cast<char8_t>(std::toupper(static_cast<unsigned char>(value))); });
    CHECK(tabs.open_repository(shouting).created == false);

    // 폴더가 사라진 worktree는 prunable이고 prune이 기록을 지운다.
    std::filesystem::remove_all(linked_path);
    live.refresh(repo);
    const worktree_info* const gone { find_worktree(live.snapshot(repo), linked_path) };
    REQUIRE(gone != nullptr);
    CHECK(gone->prunable);
    live.require_succeeded(live.run(repo, worktree_prune_request {}));
    CHECK(live.snapshot(repo).worktrees.size() == 1);
}

// ---------------------------------------------------------------- Git LFS

TEST_CASE("integration: lfs_track edits .gitattributes and tracked files are marked and diffed as pointers", "[integration][lfs]")
{
    if (lfs_is_available() == false)
        SKIP("git-lfs is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo_path { make_repository(directory.path() / "repo") };
    git_ok(repo_path, { "lfs", "install", "--local" });
    commit_file(repo_path, "readme.txt", "hello", "readme");
    live_workspace live {};
    const std::string repo { live.open(repo_path) };
    CHECK_FALSE(live.snapshot(repo).lfs.in_use);

    live.require_succeeded(live.run(repo, lfs_track_request { "*.bin" }));
    CHECK(test::read_file(repo_path / ".gitattributes").find("*.bin filter=lfs") != std::string::npos);
    CHECK(live.snapshot(repo).lfs.in_use);
    CHECK(live.snapshot(repo).lfs.available);
    CHECK(live.snapshot(repo).lfs.version.rfind("git-lfs/", 0) == 0);
    CHECK(live.snapshot(repo).lfs.installed);

    const std::string original(5000, 'x');
    test::write_file(repo_path / "big.bin", original);
    git_ok(repo_path, { "add", "." });
    git_ok(repo_path, { "commit", "-q", "-m", "lfs file" });
    live.refresh(repo);
    REQUIRE(live.snapshot(repo).lfs.files.size() == 1);
    CHECK(live.snapshot(repo).lfs.files.front().path == "big.bin");
    CHECK(live.snapshot(repo).lfs.files.front().size == 5000);
    CHECK(live.snapshot(repo).lfs.files.front().checked_out);
    CHECK(live.snapshot(repo).lfs.files.front().downloaded);

    // 고친 파일은 LFS 표시가 달리고, diff는 포인터 두 쪽을 담는다(텍스트 diff로 보이지 않는다).
    const std::string changed(6000, 'y');
    test::write_file(repo_path / "big.bin", changed);
    test::write_file(repo_path / "plain.txt", "plain");
    live.refresh(repo);
    const auto big { std::find_if(live.snapshot(repo).unstaged.begin(), live.snapshot(repo).unstaged.end(), [](const file_change& value) { return value.path == "big.bin"; }) };
    const auto plain { std::find_if(live.snapshot(repo).unstaged.begin(), live.snapshot(repo).unstaged.end(), [](const file_change& value) { return value.path == "plain.txt"; }) };
    REQUIRE(big != live.snapshot(repo).unstaged.end());
    REQUIRE(plain != live.snapshot(repo).unstaged.end());
    CHECK(big->is_lfs);
    CHECK_FALSE(plain->is_lfs);

    // 파일 감시가 내는 가벼운 새로 고침(status만)도 LFS 표시를 지켜야 한다.
    live.require_succeeded(live.run(repo, refresh_status_request {}));
    const auto status_big { std::find_if(live.snapshot(repo).unstaged.begin(), live.snapshot(repo).unstaged.end(), [](const file_change& value) { return value.path == "big.bin"; }) };
    REQUIRE(status_big != live.snapshot(repo).unstaged.end());
    CHECK(status_big->is_lfs);

    live.require_succeeded(live.run(repo, diff_file_request { .path = "big.bin" }));
    REQUIRE(live.session(repo).selected_diff().has_value());
    REQUIRE(live.session(repo).selected_diff()->files.size() == 1);
    const diff_file& diff { live.session(repo).selected_diff()->files.front() };
    REQUIRE(diff.lfs.has_value());
    REQUIRE(diff.lfs->old_pointer.has_value());
    REQUIRE(diff.lfs->new_pointer.has_value());
    CHECK(diff.lfs->old_pointer->size == 5000);
    CHECK(diff.lfs->new_pointer->size == 6000);
    // 작업 트리에 실제 내용이 있고, 옛 객체도 로컬 저장소에 있다.
    CHECK(diff.lfs->old_local);
    CHECK(diff.lfs->new_local);

    // 커밋한 뒤의 파일 목록과 커밋 diff에도 같은 표시가 붙는다.
    git_ok(repo_path, { "add", "." });
    git_ok(repo_path, { "commit", "-q", "-m", "change" });
    const std::string head { git_text(repo_path, { "rev-parse", "HEAD" }) };
    live.require_succeeded(live.run(repo, show_commit_request { head }));
    REQUIRE(live.session(repo).selected_commit().has_value());
    for (const file_change& file : live.session(repo).selected_commit()->files)
        CHECK(file.is_lfs == (file.path == "big.bin"));
    live.require_succeeded(live.run(repo, diff_file_request { .path = "big.bin", .view = diff_view::commit, .sha = head }));
    REQUIRE(live.session(repo).selected_diff()->files.front().lfs.has_value());
}

TEST_CASE("integration: lfs_fetch downloads objects and lfs_pull fills the working tree of a pointer-only clone", "[integration][lfs]")
{
    if (lfs_is_available() == false)
        SKIP("git-lfs is not installed");
    test::temp_directory directory {};
    const std::filesystem::path source { make_repository(directory.path() / "source") };
    git_ok(source, { "lfs", "install", "--local" });
    git_ok(source, { "lfs", "track", "*.bin" });
    const std::string content { "binary-content-" + std::string(4000, 'z') };
    test::write_file(source / "asset.bin", content);
    git_ok(source, { "add", "." });
    git_ok(source, { "commit", "-q", "-m", "asset" });

    // 시험 환경에는 전역 `filter.lfs`가 없어 복제본에는 포인터 파일만 남는다 (`GIT_LFS_SKIP_SMUDGE`와 같은 상태).
    const std::filesystem::path pointer_clone { directory.path() / "clone-fetch" };
    git_ok(directory.path(), { "clone", "-q", source.string(), pointer_clone.string() });
    CHECK(test::read_file(pointer_clone / "asset.bin").rfind("version https://git-lfs.github.com/spec/v1", 0) == 0);

    live_workspace live {};
    const std::string fetch_repo { live.open(pointer_clone) };
    REQUIRE(live.snapshot(fetch_repo).lfs.in_use);
    REQUIRE(live.snapshot(fetch_repo).lfs.files.size() == 1);
    CHECK_FALSE(live.snapshot(fetch_repo).lfs.files.front().checked_out);
    CHECK_FALSE(live.snapshot(fetch_repo).lfs.installed);

    // fetch는 객체만 받는다. 작업 트리는 포인터 그대로다.
    live.require_succeeded(live.run(fetch_repo, lfs_fetch_request {}));
    CHECK(live.snapshot(fetch_repo).lfs.files.front().downloaded);
    CHECK_FALSE(live.snapshot(fetch_repo).lfs.files.front().checked_out);
    CHECK(test::read_file(pointer_clone / "asset.bin").rfind("version https://git-lfs.github.com/spec/v1", 0) == 0);

    // pull은 설정이 없으면 이 저장소에만 `lfs install --local`을 한 뒤 실제 내용으로 채운다.
    const operation_id pull { live.run(fetch_repo, lfs_pull_request {}) };
    live.require_succeeded(pull);
    CHECK(test::read_file(pointer_clone / "asset.bin") == content);
    CHECK(live.snapshot(fetch_repo).lfs.files.front().checked_out);
    CHECK(live.snapshot(fetch_repo).lfs.installed);
    const std::vector<operation_command>& commands { live.status(pull).commands };
    CHECK(std::any_of(
        commands.begin(), commands.end(), [](const operation_command& command) { return std::u8string_view { command.command_line }.find(u8"lfs install --local") != std::u8string_view::npos; }));

    // 같은 일을 객체를 받지 않은 새 복제본에서 곧바로 pull로 한다.
    const std::filesystem::path pull_clone { directory.path() / "clone-pull" };
    git_ok(directory.path(), { "clone", "-q", source.string(), pull_clone.string() });
    const std::string pull_repo { live.open(pull_clone) };
    live.require_succeeded(live.run(pull_repo, lfs_pull_request {}));
    CHECK(test::read_file(pull_clone / "asset.bin") == content);
    // 파일이 그대로면 작업 트리는 깨끗하다 (포인터와 내용이 일치한다).
    CHECK(live.snapshot(pull_repo).unstaged.empty());
}
