#include "application/repository_change_classifier.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace cyella;
using namespace std::chrono_literals;

namespace {
    constexpr auto working_tree { watched_root_kind::working_tree };
    constexpr auto git_directory { watched_root_kind::git_directory };
    constexpr auto common_directory { watched_root_kind::common_directory };

    constexpr auto ignored { change_classification::ignored };
    constexpr auto worktree_change { change_classification::working_tree };
    constexpr auto state_change { change_classification::repository_state };
} // namespace

TEST_CASE("ordinary working tree files are working tree changes", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8"src\\main.cpp") == worktree_change);
    CHECK(classify_repository_change(working_tree, u8"README.md") == worktree_change);
    CHECK(classify_repository_change(working_tree, u8"한글 폴더\\파일.txt") == worktree_change);
    // `.git`으로 시작할 뿐인 이름은 git 디렉터리가 아니다.
    CHECK(classify_repository_change(working_tree, u8".gitignore") == worktree_change);
    CHECK(classify_repository_change(working_tree, u8".gitattributes") == worktree_change);
    CHECK(classify_repository_change(working_tree, u8".github\\workflows\\ci.yml") == worktree_change);
    CHECK(classify_repository_change(working_tree, u8"index") == worktree_change);
}

TEST_CASE("empty paths and the git directory itself are ignored", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8"") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\") == ignored);
    CHECK(classify_repository_change(git_directory, u8"") == ignored);
}

TEST_CASE("working tree changes under .git are not working tree changes", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8".git\\hooks\\pre-commit") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\config.lock") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\COMMIT_EDITMSG") == ignored);
    CHECK(classify_repository_change(working_tree, u8".GIT\\objects\\ab\\cdef") == ignored);
}

TEST_CASE("state files under .git are repository state changes", "[repository_watch]")
{
    for (const std::u8string name : { u8"config", u8"HEAD", u8"index", u8"packed-refs", u8"FETCH_HEAD", u8"ORIG_HEAD", u8"MERGE_HEAD", u8"CHERRY_PICK_HEAD", u8"REVERT_HEAD" })
    {
        INFO(std::string(name.begin(), name.end()));
        CHECK(classify_repository_change(working_tree, u8".git\\" + name) == state_change);
        CHECK(classify_repository_change(git_directory, name) == state_change);
    }
    // NTFS는 대소문자를 구분하지 않는다.
    CHECK(classify_repository_change(working_tree, u8".git\\head") == state_change);
    CHECK(classify_repository_change(working_tree, u8".Git\\Index") == state_change);
}

TEST_CASE("refs and in progress operation directories are repository state changes", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8".git\\refs\\heads\\main") == state_change);
    CHECK(classify_repository_change(working_tree, u8".git\\refs\\heads\\feature\\한글") == state_change);
    CHECK(classify_repository_change(working_tree, u8".git\\refs\\tags\\v1.0") == state_change);
    CHECK(classify_repository_change(working_tree, u8".git\\refs\\remotes\\origin\\main") == state_change);
    CHECK(classify_repository_change(working_tree, u8".git\\refs") == state_change);
    CHECK(classify_repository_change(working_tree, u8".git\\rebase-merge") == state_change);
    CHECK(classify_repository_change(working_tree, u8".git\\rebase-merge\\git-rebase-todo") == state_change);
    CHECK(classify_repository_change(working_tree, u8".git\\rebase-apply\\next") == state_change);
    CHECK(classify_repository_change(git_directory, u8"refs/heads/main") == state_change);
    CHECK(classify_repository_change(git_directory, u8"rebase-merge/done") == state_change);
}

TEST_CASE("lock files are ignored before any other rule", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8".git\\index.lock") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\HEAD.lock") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\refs\\heads\\main.lock") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\packed-refs.lock") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\rebase-merge\\x.LOCK") == ignored);
    CHECK(classify_repository_change(git_directory, u8"index.lock") == ignored);
    // 작업 트리의 `.lock` 파일(예: 패키지 매니저 잠금)은 평범한 작업 트리 파일이다.
    CHECK(classify_repository_change(working_tree, u8"yarn.lock") == worktree_change);
}

TEST_CASE("objects logs and unlisted entries are ignored", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8".git\\objects\\pack\\pack-1.pack") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\objects") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\logs\\HEAD") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\logs\\refs\\heads\\main") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\info\\exclude") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\MERGE_MSG") == ignored);
    // 목록의 이름이 하위 경로 안에 있다고 상태 변경은 아니다.
    CHECK(classify_repository_change(working_tree, u8".git\\hooks\\HEAD") == ignored);
    CHECK(classify_repository_change(working_tree, u8".git\\modules\\sub\\HEAD") == ignored);
    CHECK(classify_repository_change(git_directory, u8"objects/ab/cd") == ignored);
}

TEST_CASE("nested repositories do not count as this repository", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8"vendor\\lib\\.git\\index") == ignored);
    CHECK(classify_repository_change(working_tree, u8"vendor\\lib\\.git") == ignored);
}

TEST_CASE("forward slashes are accepted", "[repository_watch]")
{
    CHECK(classify_repository_change(working_tree, u8".git/refs/heads/main") == state_change);
    CHECK(classify_repository_change(working_tree, u8"src/main.cpp") == worktree_change);
}

TEST_CASE("the common directory only reports refs", "[repository_watch]")
{
    CHECK(classify_repository_change(common_directory, u8"refs/heads/main") == state_change);
    CHECK(classify_repository_change(common_directory, u8"packed-refs") == state_change);
    // HEAD와 index는 다른 worktree의 것이다.
    CHECK(classify_repository_change(common_directory, u8"HEAD") == ignored);
    CHECK(classify_repository_change(common_directory, u8"index") == ignored);
    CHECK(classify_repository_change(common_directory, u8"objects/ab/cd") == ignored);
    CHECK(classify_repository_change(common_directory, u8"refs/heads/main.lock") == ignored);
    CHECK(classify_repository_change(common_directory, u8"worktrees/x/HEAD") == ignored);
}

TEST_CASE("gitdir pointers are resolved against the .git file's directory", "[repository_watch]")
{
    CHECK(resolve_gitdir_pointer(u8"gitdir: C:/main/.git/worktrees/wt\n", u8"C:\\wt") == u8"C:\\main\\.git\\worktrees\\wt");
    CHECK(resolve_gitdir_pointer(u8"gitdir: ../.git/modules/sub\r\n", u8"C:\\super\\sub") == u8"C:\\super\\sub\\..\\.git\\modules\\sub");
    CHECK(resolve_gitdir_pointer(u8"gitdir: .git-store", u8"C:\\work\\repo\\") == u8"C:\\work\\repo\\.git-store");
    CHECK(resolve_gitdir_pointer(u8"gitdir:   D:\\한글 폴더\\.git  \n", u8"C:\\x") == u8"D:\\한글 폴더\\.git");
    CHECK(resolve_gitdir_pointer(u8"gitdir: \\\\server\\share\\r.git", u8"C:\\x") == u8"\\\\server\\share\\r.git");
}

TEST_CASE("malformed gitdir pointers are rejected", "[repository_watch]")
{
    CHECK_FALSE(resolve_gitdir_pointer(u8"", u8"C:\\x").has_value());
    CHECK_FALSE(resolve_gitdir_pointer(u8"gitdir:", u8"C:\\x").has_value());
    CHECK_FALSE(resolve_gitdir_pointer(u8"gitdir:   \n", u8"C:\\x").has_value());
    CHECK_FALSE(resolve_gitdir_pointer(u8"ref: refs/heads/main", u8"C:\\x").has_value());
    CHECK_FALSE(resolve_gitdir_pointer(u8"GITDIR: C:\\x", u8"C:\\x").has_value());
    // 어느 드라이브인지 알 수 없는 경로는 쓰지 않는다.
    CHECK_FALSE(resolve_gitdir_pointer(u8"gitdir: /c/Users/x/.git", u8"C:\\x").has_value());
    CHECK_FALSE(resolve_gitdir_pointer(u8"gitdir: \\rooted\\.git", u8"C:\\x").has_value());
}

TEST_CASE("commondir pointers are resolved against the gitdir", "[repository_watch]")
{
    CHECK(resolve_commondir_pointer(u8"../..\n", u8"C:\\main\\.git\\worktrees\\wt") == u8"C:\\main\\.git\\worktrees\\wt\\..\\..");
    CHECK(resolve_commondir_pointer(u8"C:/main/.git", u8"C:\\other") == u8"C:\\main\\.git");
    CHECK_FALSE(resolve_commondir_pointer(u8"\n", u8"C:\\x").has_value());
}

namespace {
    using steady = change_debouncer::clock;
    const steady::time_point origin { steady::time_point {} + 1h };

    pending_changes only(const bool working_tree_changed, const bool repository_state_changed)
    {
        return { working_tree_changed, repository_state_changed };
    }
} // namespace

TEST_CASE("the debouncer is idle until a change arrives", "[repository_watch]")
{
    change_debouncer debouncer {};
    CHECK_FALSE(debouncer.due_time().has_value());
    CHECK_FALSE(debouncer.take_ready(origin + 10s).any());

    debouncer.note(ignored, origin);
    CHECK_FALSE(debouncer.due_time().has_value());
}

TEST_CASE("the default debounce window is 300 ms and a change is held until then", "[repository_watch]")
{
    CHECK(debounce_options {}.window == 300ms);

    change_debouncer debouncer {};
    debouncer.note(worktree_change, origin);

    REQUIRE(debouncer.due_time().has_value());
    CHECK(*debouncer.due_time() == origin + 300ms);
    CHECK_FALSE(debouncer.take_ready(origin + 299ms).any());
    CHECK(debouncer.take_ready(origin + 300ms) == only(true, false));
    // 한 번 꺼내면 비워진다.
    CHECK_FALSE(debouncer.take_ready(origin + 10s).any());
    CHECK_FALSE(debouncer.due_time().has_value());
}

TEST_CASE("further changes push the due time back", "[repository_watch]")
{
    change_debouncer debouncer {};
    debouncer.note(worktree_change, origin);
    debouncer.note(worktree_change, origin + 200ms);
    debouncer.note(worktree_change, origin + 450ms);

    CHECK(*debouncer.due_time() == origin + 750ms);
    CHECK_FALSE(debouncer.take_ready(origin + 700ms).any());
    CHECK(debouncer.take_ready(origin + 750ms) == only(true, false));
}

TEST_CASE("both kinds are reported once and share the window", "[repository_watch]")
{
    change_debouncer debouncer {};
    debouncer.note(worktree_change, origin);
    debouncer.note(state_change, origin + 100ms);
    debouncer.note(worktree_change, origin + 150ms);
    debouncer.note(state_change, origin + 160ms);

    CHECK_FALSE(debouncer.take_ready(origin + 459ms).any());
    CHECK(debouncer.take_ready(origin + 460ms) == only(true, true));
}

TEST_CASE("a constant stream of changes is still reported after the maximum wait", "[repository_watch]")
{
    debounce_options options {};
    options.window = 300ms;
    options.maximum_wait = 2s;
    change_debouncer debouncer { options };

    for (auto offset = 0ms; offset <= 5s; offset += 100ms)
    {
        debouncer.note(worktree_change, origin + offset);
        if (offset < 2s)
            CHECK_FALSE(debouncer.take_ready(origin + offset).any());
        else
        {
            CHECK(debouncer.take_ready(origin + offset) == only(true, false));
            break;
        }
    }
}

TEST_CASE("a custom window and an invalid maximum wait", "[repository_watch]")
{
    debounce_options options {};
    options.window = 50ms;
    options.maximum_wait = 10ms;
    change_debouncer debouncer { options };

    debouncer.note(state_change, origin);
    // 최대 대기가 창보다 짧으면 창으로 본다.
    CHECK(*debouncer.due_time() == origin + 50ms);
    CHECK(debouncer.take_ready(origin + 50ms) == only(false, true));
}

TEST_CASE("overflow reports both kinds", "[repository_watch]")
{
    change_debouncer debouncer {};
    debouncer.note_overflow(origin);
    CHECK(debouncer.take_ready(origin + 300ms) == only(true, true));
}

TEST_CASE("changes after a flush start a new window", "[repository_watch]")
{
    change_debouncer debouncer {};
    debouncer.note(worktree_change, origin);
    REQUIRE(debouncer.take_ready(origin + 300ms).any());

    debouncer.note(state_change, origin + 1s);
    CHECK(*debouncer.due_time() == origin + 1300ms);
    CHECK(debouncer.take_ready(origin + 1300ms) == only(false, true));
}
