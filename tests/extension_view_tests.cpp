// submodule·worktree·Git LFS의 화면 쪽 규칙이다: 사이드바 구역, 메뉴 조건, 대화 상자, diff 카드, 두 번 누름과 메뉴가 낳는
// operation. git 없이 세션에 사건을 직접 넣고 workspace는 가짜 runner로 세운다.
#include "application/app_messages.h"
#include "application/diff_view_controller.h"
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/diff_special.h"
#include "presentation/form_extension.h"
#include "presentation/graph_geometry.h"
#include "presentation/menu_extension.h"
#include "presentation/menu_model.h"
#include "presentation/shell_controller.h"
#include "presentation/sidebar_extension.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    namespace snap = cyella::test::snapshot;

    const std::string sha_old { "1ecfd40f48933961e1d368341744a076db579db3" };
    const std::string sha_new { "9b8895860a444517b36dd491c64f404084088883" };

    submodule_info make_submodule(const std::string& path, const submodule_state state, const std::string& recorded, const std::string& checked_out, const std::uint32_t depth = 0)
    {
        submodule_info info {};
        info.path = path;
        info.name = path;
        info.url = "../" + path.substr(path.find('/') + 1);
        info.recorded_sha = recorded;
        info.checked_out_sha = checked_out;
        info.state = state;
        info.depth = depth;
        return info;
    }

    worktree_info make_worktree(const std::string& path, const std::string& branch, const bool main, const bool current)
    {
        worktree_info info {};
        info.path = path;
        info.head_sha = sha_old;
        info.branch = branch;
        info.branch_ref = branch.empty() ? std::string {} : "refs/heads/" + branch;
        info.detached = branch.empty();
        info.is_main = main;
        info.is_current = current;
        return info;
    }

    // 사이드바 test가 쓰는 저장소다: submodule 둘(초기화·미초기화), worktree 셋(현재·연결·잠김+정리 가능), LFS 파일 둘.
    repository_snapshot extension_snapshot()
    {
        repository_snapshot snapshot { snap::make_snapshot(snap::sha_of(4), 0) };
        snapshot.submodules = { make_submodule("libs/a", submodule_state::initialized, sha_old, sha_old), make_submodule("libs/b", submodule_state::uninitialized, sha_new, {}) };
        snapshot.worktrees
            = { make_worktree("C:/work/repo", "main", true, true), make_worktree("C:/work/repo-feat", "feature/graph", false, false), make_worktree("C:/work/repo-old", "", false, false) };
        snapshot.worktrees[2].locked = true;
        snapshot.worktrees[2].lock_reason = "usb";
        snapshot.worktrees[2].prunable = true;
        snapshot.refs[1].worktree_path = "C:/work/repo-feat";
        snapshot.lfs.in_use = true;
        snapshot.lfs.available = true;
        snapshot.lfs.installed = true;
        snapshot.lfs.version = "git-lfs/3.7.1";
        snapshot.lfs.files.push_back({ .path = "art/big.bin", .oid = std::string(64, 'a'), .size = 5000, .downloaded = true, .checked_out = true });
        snapshot.lfs.files.push_back({ .path = "art/pointer.bin", .oid = std::string(64, 'b'), .size = 3000000, .downloaded = false, .checked_out = false });
        return snapshot;
    }

    const sidebar_row_view* find_row(const sidebar_rows& rows, const std::u8string& key)
    {
        const auto found { std::ranges::find_if(rows.rows, [&key](const sidebar_row_view& row) { return row.key == key; }) };
        return found == rows.rows.end() ? nullptr : &*found;
    }

    bool has_entry(const menu_model& model, const std::u8string& key)
    {
        return std::ranges::any_of(model.entries, [&key](const menu_entry& entry) { return entry.key == key; });
    }

    bool entry_enabled(const menu_model& model, const std::u8string& key)
    {
        for (const menu_entry& entry : model.entries)
            if (entry.key == key)
                return entry.enabled;
        return false;
    }

    menu_context context_of(const repository_snapshot& snapshot)
    {
        menu_context context {};
        context.current_branch = snapshot.summary.current_branch;
        context.head_sha = snapshot.summary.head_sha;
        context.refs = snapshot.refs;
        context.submodules = snapshot.submodules;
        context.worktrees = snapshot.worktrees;
        context.lfs = snapshot.lfs;
        return context;
    }

    diff_file pointer_diff(const std::optional<lfs_pointer>& old_pointer, const std::optional<lfs_pointer>& new_pointer, const bool old_local, const bool new_local)
    {
        diff_file file {};
        file.old_path = "art/big.bin";
        file.new_path = "art/big.bin";
        file.old_mode = "100644";
        file.new_mode = "100644";
        file.lfs = lfs_diff_info { old_pointer, new_pointer, old_local, new_local };
        return file;
    }

    struct harness
    {
        harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(workspace) }
        {
            controller.start({});
            controller.open_repository(u8"C:\\work\\repo");
            repository = controller.selected_repository_id();
        }

        static shell_controller::dependencies make_dependencies(git_workspace& workspace)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } }; };
            return dependencies;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            controller.handle(luil::app_message { std::move(intent) });
            controller.after_events();
        }

        repository_session& load(repository_snapshot snapshot = extension_snapshot())
        {
            repository_session* const session { workspace.find_session(repository) };
            REQUIRE(session != nullptr);
            std::vector<commit_info> commits { snap::linear_history(5) };
            const std::uint64_t generation { session->begin_refresh() };
            REQUIRE(session->apply(snapshot_loaded { generation, std::move(snapshot) }));
            REQUIRE(session->apply(commits_loaded { .generation = generation, .first_index = 0, .commits = std::move(commits), .page_complete = true, .history_complete = true }));
            return *session;
        }

        [[nodiscard]] view_snapshot view()
        {
            return controller.make_view(1440.0f, 900.0f, 1.0f);
        }

        [[nodiscard]] operation_id last_operation() const
        {
            const std::deque<operation_id>& order { workspace.operation_order() };
            for (auto it { order.rbegin() }; it != order.rend(); ++it)
                if (const operation_status* const status { workspace.find_operation(*it) }; status != nullptr && status->automatic == false)
                    return *it;
            return 0;
        }

        template<typename request_type>
        [[nodiscard]] const request_type* last_request() const
        {
            const operation_id id { last_operation() };
            if (id == 0)
                return nullptr;
            const git_operation* const request { workspace.request_of(id) };
            return request == nullptr ? nullptr : std::get_if<request_type>(request);
        }

        [[nodiscard]] std::size_t operation_count() const
        {
            std::size_t count { 0 };
            for (const operation_id id : workspace.operation_order())
                if (const operation_status* const status { workspace.find_operation(id) }; status != nullptr && status->automatic == false)
                    ++count;
            return count;
        }

        void type(const std::size_t field, const std::u8string& text)
        {
            luil::text_edit_request request {};
            request.target = to_luil_target(static_cast<text_target>(static_cast<std::uint32_t>(text_target::form_field_0) + field));
            request.command = luil::text::text_edit_command::insert;
            request.text = text;
            send(edit_text_intent { request });
        }

        void menu(menu_target target, const std::u8string& key)
        {
            send(open_context_menu_intent { std::move(target), 0.0f, 0.0f });
            send(menu_select_intent { key });
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_operation_runner runner;
        git_workspace workspace;
        shell_controller controller;
        std::string repository {};
    };
} // namespace

// ---------------------------------------------------------------- 사이드바

TEST_CASE("the sidebar gains SUBMODULES, WORKTREES and GIT LFS sections with counts and badges", "[presentation][extension][sidebar]")
{
    const sidebar_rows rows { build_sidebar_rows(extension_snapshot(), {}, 0) };

    CHECK(rows.counts[static_cast<std::size_t>(sidebar_section::submodules)] == 2);
    CHECK(rows.counts[static_cast<std::size_t>(sidebar_section::worktrees)] == 3);
    CHECK(rows.counts[static_cast<std::size_t>(sidebar_section::lfs)] == 2);
    REQUIRE(find_row(rows, u8"h:SUBMODULES") != nullptr);
    CHECK(find_row(rows, u8"h:SUBMODULES")->count == 2);
    CHECK(find_row(rows, u8"h:WORKTREES")->header);

    const sidebar_row_view* const initialized { find_row(rows, u8"sm:libs/a") };
    REQUIRE(initialized != nullptr);
    CHECK(initialized->item == sidebar_item_kind::submodule);
    CHECK(initialized->path == "libs/a");
    CHECK(initialized->badge.empty());
    CHECK(initialized->detail == u8"1ecfd40");
    const sidebar_row_view* const uninitialized { find_row(rows, u8"sm:libs/b") };
    REQUIRE(uninitialized != nullptr);
    CHECK(uninitialized->badge == u8"미초기화");
    CHECK(uninitialized->tone == badge_tone::warning);
    CHECK(uninitialized->dimmed);
    // 초기화하기 전에는 기록된 커밋을 보인다.
    CHECK(uninitialized->detail == u8"9b88958");

    const sidebar_row_view* const main_tree { find_row(rows, u8"wt:C:/work/repo") };
    REQUIRE(main_tree != nullptr);
    CHECK(main_tree->item == sidebar_item_kind::worktree);
    CHECK(main_tree->label == u8"repo");
    CHECK(main_tree->detail == u8"main");
    CHECK(main_tree->badge == u8"주 · 현재");
    CHECK(main_tree->worktree_main);
    CHECK(main_tree->worktree_current);
    const sidebar_row_view* const stale { find_row(rows, u8"wt:C:/work/repo-old") };
    REQUIRE(stale != nullptr);
    CHECK(stale->badge == u8"잠김 · 정리 가능");
    CHECK(stale->tone == badge_tone::danger);
    CHECK(stale->detail == u8"detached 1ecfd40");

    const sidebar_row_view* const status { find_row(rows, u8"lfs:status") };
    REQUIRE(status != nullptr);
    CHECK(status->label == u8"git-lfs/3.7.1");
    CHECK(status->detail == u8"1개 포인터");
    CHECK(find_row(rows, u8"lfs:f:art/big.bin")->badge == u8"받음");
    CHECK(find_row(rows, u8"lfs:f:art/pointer.bin")->badge == u8"포인터");
    CHECK(find_row(rows, u8"lfs:f:art/pointer.bin")->detail == u8"3.0 MB");
}

TEST_CASE("the sidebar marks branches checked out in another worktree", "[presentation][extension][sidebar]")
{
    const sidebar_rows rows { build_sidebar_rows(extension_snapshot(), {}, 0) };
    const sidebar_row_view* const branch { find_row(rows, u8"l:feature/graph") };
    REQUIRE(branch != nullptr);
    CHECK(branch->worktree_path == "C:/work/repo-feat");
    CHECK(branch->tooltip.find(u8"다른 worktree") != std::u8string::npos);
    CHECK(find_row(rows, u8"l:main")->worktree_path.empty());
}

TEST_CASE("the LFS section is absent for repositories that do not use it and reports a missing git-lfs", "[presentation][extension][sidebar]")
{
    repository_snapshot plain { extension_snapshot() };
    plain.lfs = {};
    const sidebar_rows without { build_sidebar_rows(plain, {}, 0) };
    CHECK(find_row(without, u8"h:GIT LFS") == nullptr);
    CHECK(find_row(without, u8"h:SUBMODULES") != nullptr);

    repository_snapshot missing { extension_snapshot() };
    missing.lfs.available = false;
    missing.lfs.files.clear();
    const sidebar_rows rows { build_sidebar_rows(missing, {}, 0) };
    const sidebar_row_view* const status { find_row(rows, u8"lfs:status") };
    REQUIRE(status != nullptr);
    CHECK(status->badge == u8"없음");
    CHECK(status->tone == badge_tone::danger);
}

TEST_CASE("collapsing and filtering apply to the new sections", "[presentation][extension][sidebar]")
{
    const std::uint8_t collapsed { static_cast<std::uint8_t>(1u << static_cast<unsigned>(sidebar_section::submodules)) };
    const sidebar_rows folded { build_sidebar_rows(extension_snapshot(), {}, collapsed) };
    CHECK(find_row(folded, u8"h:SUBMODULES")->collapsed);
    CHECK(find_row(folded, u8"h:SUBMODULES")->count == 2);
    CHECK(find_row(folded, u8"sm:libs/a") == nullptr);
    CHECK(find_row(folded, u8"wt:C:/work/repo") != nullptr);

    const sidebar_rows filtered { build_sidebar_rows(extension_snapshot(), u8"feat", 0) };
    CHECK(find_row(filtered, u8"sm:libs/a") == nullptr);
    CHECK(find_row(filtered, u8"h:SUBMODULES")->count == 0);
    CHECK(find_row(filtered, u8"wt:C:/work/repo-feat") != nullptr);
    CHECK(find_row(filtered, u8"wt:C:/work/repo") == nullptr);
}

TEST_CASE("a large LFS list is capped with a remainder row", "[presentation][extension][sidebar]")
{
    repository_snapshot snapshot { extension_snapshot() };
    snapshot.lfs.files.clear();
    for (std::size_t index { 0 }; index < lfs_sidebar_file_limit + 30; ++index)
        snapshot.lfs.files.push_back({ .path = "f" + std::to_string(index) + ".bin", .size = 1, .downloaded = true, .checked_out = true });
    const sidebar_rows rows { build_sidebar_rows(snapshot, {}, 0) };
    CHECK(find_row(rows, u8"h:GIT LFS")->count == lfs_sidebar_file_limit + 30);
    REQUIRE(find_row(rows, u8"lfs:more") != nullptr);
    CHECK(find_row(rows, u8"lfs:more")->label == u8"외 30개");
}

TEST_CASE("the view snapshot carries the new sections and the badges of file rows", "[presentation][extension][sidebar]")
{
    snap::snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo");
    repository_session& session { *harness.sessions["repo"] };
    repository_snapshot snapshot { extension_snapshot() };
    file_change lfs_file {};
    lfs_file.path = "art/big.bin";
    lfs_file.status = 'M';
    lfs_file.is_lfs = true;
    file_change gitlink {};
    gitlink.path = "libs/a";
    gitlink.status = 'M';
    gitlink.is_submodule = true;
    snapshot.unstaged = { lfs_file, gitlink };
    snapshot.summary.changed_file_count = 2;
    std::vector<commit_info> commits { snap::linear_history(3) };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = std::move(commits), .page_complete = true, .history_complete = true }));
    harness.ui.tabs[harness.tabs.selected().id].selected_sha = "WIP";

    const view_snapshot view { harness.build() };
    REQUIRE(view.repository.has_value());
    CHECK(view.repository->section_counts[static_cast<std::size_t>(sidebar_section::submodules)] == 2);
    REQUIRE(view.repository->working.has_value());
    REQUIRE(view.repository->working->unstaged.size() == 2);
    CHECK(view.repository->working->unstaged[0].lfs);
    CHECK_FALSE(view.repository->working->unstaged[0].submodule);
    CHECK(view.repository->working->unstaged[1].submodule);
}

TEST_CASE("a graph label of a branch checked out elsewhere carries the worktree marker", "[presentation][extension][graph]")
{
    const repository_snapshot snapshot { extension_snapshot() };
    const ref_label_map labels { build_ref_label_map(snapshot.refs, snapshot.summary) };
    const auto found { labels.find(snap::sha_of(0)) };
    REQUIRE(found != labels.end());
    const auto marked { std::ranges::find_if(found->second, [](const ref_label& label) { return label.ref_name == "feature/graph"; }) };
    REQUIRE(marked != found->second.end());
    CHECK(marked->worktree_path == "C:/work/repo-feat");
    const auto main_label { labels.find(snap::sha_of(4)) };
    REQUIRE(main_label != labels.end());
    CHECK(main_label->second.front().worktree_path.empty());
}

// ---------------------------------------------------------------- 메뉴

TEST_CASE("submodule menus offer update, open and copy path", "[presentation][extension][menu]")
{
    const repository_snapshot snapshot { extension_snapshot() };
    const menu_context context { context_of(snapshot) };

    const menu_model initialized { build_context_menu(menu_target { .kind = menu_target_kind::submodule, .name = "libs/a" }, context) };
    CHECK(initialized.title == u8"libs/a");
    CHECK(entry_enabled(initialized, u8"open"));
    CHECK(entry_enabled(initialized, u8"update"));
    CHECK(entry_enabled(initialized, u8"copy_path"));

    const menu_model uninitialized { build_context_menu(menu_target { .kind = menu_target_kind::submodule, .name = "libs/b" }, context) };
    CHECK_FALSE(entry_enabled(uninitialized, u8"open"));
    CHECK(entry_enabled(uninitialized, u8"update"));

    const menu_model section { build_context_menu(menu_target { .kind = menu_target_kind::submodules_section }, context) };
    CHECK(entry_enabled(section, u8"update_all"));
    CHECK(entry_enabled(section, u8"sync_all"));
    CHECK_FALSE(entry_enabled(build_context_menu(menu_target { .kind = menu_target_kind::submodules_section }, menu_context {}), u8"update_all"));
}

TEST_CASE("worktree menus guard the main and current worktrees", "[presentation][extension][menu]")
{
    const menu_context context { context_of(extension_snapshot()) };

    const menu_model main_tree { build_context_menu(menu_target { .kind = menu_target_kind::worktree, .name = "C:/work/repo" }, context) };
    CHECK_FALSE(entry_enabled(main_tree, u8"open"));
    CHECK_FALSE(entry_enabled(main_tree, u8"remove"));
    CHECK(entry_enabled(main_tree, u8"copy_path"));

    const menu_model linked { build_context_menu(menu_target { .kind = menu_target_kind::worktree, .name = "C:/work/repo-feat" }, context) };
    CHECK(entry_enabled(linked, u8"open"));
    CHECK(entry_enabled(linked, u8"remove"));
    CHECK(linked.entries[1].danger);

    // 폴더가 사라진 worktree는 열 수 없지만 지울 수는 있다.
    const menu_model stale { build_context_menu(menu_target { .kind = menu_target_kind::worktree, .name = "C:/work/repo-old" }, context) };
    CHECK_FALSE(entry_enabled(stale, u8"open"));
    CHECK(entry_enabled(stale, u8"remove"));

    const menu_model section { build_context_menu(menu_target { .kind = menu_target_kind::worktrees_section }, context) };
    CHECK(entry_enabled(section, u8"add"));
    CHECK(entry_enabled(section, u8"prune"));
    menu_context clean { context };
    clean.worktrees.pop_back();
    CHECK_FALSE(entry_enabled(build_context_menu(menu_target { .kind = menu_target_kind::worktrees_section }, clean), u8"prune"));
}

TEST_CASE("a branch used by another worktree cannot be checked out from its menu", "[presentation][extension][menu]")
{
    const menu_context context { context_of(extension_snapshot()) };
    const menu_model blocked { build_context_menu(menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph" }, context) };
    REQUIRE(has_entry(blocked, u8"checkout"));
    CHECK_FALSE(entry_enabled(blocked, u8"checkout"));
    CHECK(entry_enabled(blocked, u8"open_worktree"));
    CHECK_FALSE(has_entry(blocked, u8"add_worktree"));

    menu_context free_context { context };
    free_context.refs[1].worktree_path.clear();
    const menu_model open { build_context_menu(menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph" }, free_context) };
    CHECK(entry_enabled(open, u8"checkout"));
    CHECK(entry_enabled(open, u8"add_worktree"));
    CHECK_FALSE(has_entry(open, u8"open_worktree"));

    // 현재 브랜치에서는 새 worktree로 체크아웃할 수 없다.
    CHECK_FALSE(entry_enabled(build_context_menu(menu_target { .kind = menu_target_kind::local_branch, .name = "main" }, free_context), u8"add_worktree"));
    CHECK(has_entry(build_context_menu(menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(1) }, context), u8"add_worktree"));
}

TEST_CASE("LFS menus follow the availability of git-lfs and the file's tracking", "[presentation][extension][menu]")
{
    menu_context context { context_of(extension_snapshot()) };
    const menu_model section { build_context_menu(menu_target { .kind = menu_target_kind::lfs_section }, context) };
    CHECK(entry_enabled(section, u8"pull"));
    CHECK(entry_enabled(section, u8"fetch"));
    CHECK(entry_enabled(section, u8"fetch_all"));
    CHECK(entry_enabled(section, u8"track"));
    context.lfs.available = false;
    CHECK_FALSE(entry_enabled(build_context_menu(menu_target { .kind = menu_target_kind::lfs_section }, context), u8"pull"));

    const menu_model tracked { build_context_menu(menu_target { .kind = menu_target_kind::file, .name = "art/big.bin", .lfs = true }, context) };
    CHECK(has_entry(tracked, u8"lfs_pull"));
    CHECK_FALSE(has_entry(tracked, u8"lfs_track"));
    const menu_model plain { build_context_menu(menu_target { .kind = menu_target_kind::file, .name = "art/logo.psd" }, context) };
    CHECK(has_entry(plain, u8"lfs_track"));
    CHECK(plain.entries.back().label == u8"*.psd을(를) LFS로 추적…");
    // 확장자가 없는 파일에는 패턴을 제안하지 않는다.
    CHECK_FALSE(has_entry(build_context_menu(menu_target { .kind = menu_target_kind::file, .name = "Makefile" }, context), u8"lfs_track"));

    CHECK(lfs_pattern_for_path("art/logo.PSD") == "*.PSD");
    CHECK(lfs_pattern_for_path("a/b.tar.gz") == "*.gz");
    CHECK(lfs_pattern_for_path(".gitignore").empty());
    CHECK(lfs_pattern_for_path("dir.d/readme").empty());
    CHECK(lfs_pattern_for_path("trailing.").empty());
}

// ---------------------------------------------------------------- 대화 상자 규칙

TEST_CASE("the worktree dialog rules check folder, mode and branch", "[presentation][extension][dialog]")
{
    const repository_snapshot snapshot { extension_snapshot() };
    dialog_state dialog {};
    dialog.kind = dialog_kind::add_worktree;
    form_inputs inputs {};
    const auto set = [&inputs](const std::size_t field, const std::u8string& text) {
        inputs.fields[field].text = text;
        inputs.fields[field].caret = text.size();
        inputs.fields[field].anchor = text.size();
    };
    std::u8string reason {};

    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot, &reason));
    CHECK(reason.find(u8"폴더") != std::u8string::npos);
    set(0, u8"relative/dir");
    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot, &reason));
    CHECK(reason.find(u8"절대 경로") != std::u8string::npos);
    set(0, u8"C:/work/repo-feat");
    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot, &reason));
    CHECK(reason.find(u8"이미 worktree가 있는 폴더") != std::u8string::npos);
    set(0, u8"C:/work/new-tree");

    // 새 브랜치: 이름이 필요하고 이미 있는 이름은 안 된다.
    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot));
    set(1, u8"main");
    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot, &reason));
    set(1, u8"topic/x");
    CHECK(add_worktree_ready(dialog, inputs, snapshot));
    dialog.subject = snap::sha_of(2);
    const worktree_add_request created { make_worktree_add_request(dialog, inputs) };
    CHECK(created.new_branch == "topic/x");
    CHECK(created.start_point == snap::sha_of(2));
    CHECK(created.path == u8"C:/work/new-tree");

    // 기존 브랜치: 현재 브랜치나 다른 worktree가 쓰는 브랜치는 안 된다.
    dialog.choice = static_cast<std::size_t>(worktree_dialog_mode::existing_branch);
    set(1, u8"main");
    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot, &reason));
    set(1, u8"feature/graph");
    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot, &reason));
    CHECK(reason.find(u8"다른 worktree") != std::u8string::npos);
    set(1, u8"nope");
    CHECK_FALSE(add_worktree_ready(dialog, inputs, snapshot));
    repository_snapshot free_snapshot { snapshot };
    free_snapshot.refs[1].worktree_path.clear();
    set(1, u8"feature/graph");
    CHECK(add_worktree_ready(dialog, inputs, free_snapshot));
    const worktree_add_request existing { make_worktree_add_request(dialog, inputs) };
    CHECK(existing.branch == "feature/graph");
    CHECK(existing.new_branch.empty());

    // detached에는 브랜치 이름이 필요 없다.
    dialog.choice = static_cast<std::size_t>(worktree_dialog_mode::detached);
    set(1, u8"");
    CHECK(add_worktree_ready(dialog, inputs, snapshot));
    CHECK(make_worktree_add_request(dialog, inputs).detach);
}

TEST_CASE("the default worktree folder sits next to the repository", "[presentation][extension][dialog]")
{
    CHECK(default_worktree_folder(u8"E:\\work\\repo", "feat/x") == u8"E:\\work\\repo-feat-x");
    CHECK(default_worktree_folder(u8"E:/work/repo/", "") == u8"E:/work/repo-worktree");
    CHECK(default_worktree_folder(u8"repo", "x").empty());
}

TEST_CASE("the LFS pattern dialog requires a plain pattern", "[presentation][extension][dialog]")
{
    dialog_state dialog {};
    form_inputs inputs {};
    std::u8string reason {};
    CHECK_FALSE(lfs_track_ready(dialog, inputs, &reason));
    inputs.fields[0].text = u8"-bad";
    CHECK_FALSE(lfs_track_ready(dialog, inputs, &reason));
    inputs.fields[0].text = u8" *.psd ";
    CHECK(lfs_track_ready(dialog, inputs));
    CHECK(make_lfs_track_request(inputs).pattern == "*.psd");
}

// ---------------------------------------------------------------- diff 카드

TEST_CASE("an LFS pointer diff becomes a card with sizes unless both sides are local", "[presentation][extension][diff]")
{
    const lfs_pointer before { std::string(64, 'a'), 5000 };
    const lfs_pointer after { std::string(64, 'b'), 3000000 };

    const diff_file missing { pointer_diff(before, after, true, false) };
    CHECK(missing.is_special());
    const std::optional<diff_special_view> card { build_diff_special(missing) };
    REQUIRE(card.has_value());
    CHECK(card->kind == diff_special_kind::lfs_pointer);
    CHECK(card->title == u8"LFS 포인터");
    REQUIRE(card->rows.size() == 5);
    CHECK(card->rows[0].value == u8"art/big.bin");
    CHECK(card->rows[1].value == u8"5.0 KB · oid aaaaaaaaaa");
    CHECK(card->rows[2].value == u8"3.0 MB · oid bbbbbbbbbb");
    CHECK(card->rows[3].value == u8"로컬에 있음");
    CHECK(card->rows[4].value == u8"로컬에 없음");
    REQUIRE(card->action.has_value());
    CHECK(std::holds_alternative<lfs_pull_request>(*card->action));
    CHECK(card->action_label == u8"LFS pull");

    // 두 쪽의 내용이 모두 로컬에 있으면 카드가 아니라 이미지·바이너리 비교다.
    const diff_file local { pointer_diff(before, after, true, true) };
    CHECK_FALSE(local.is_special());
    CHECK_FALSE(build_diff_special(local).has_value());

    // 추가된 파일은 옛 쪽이 없다.
    const diff_file added { pointer_diff(std::nullopt, after, true, false) };
    const std::optional<diff_special_view> added_card { build_diff_special(added) };
    REQUIRE(added_card.has_value());
    CHECK(added_card->rows[1].value == u8"없음");
    CHECK(added_card->rows[3].value == u8"해당 없음");
}

TEST_CASE("a gitlink diff becomes a submodule pointer card", "[presentation][extension][diff]")
{
    diff_file file {};
    file.old_path = file.new_path = "libs/a";
    file.old_mode = file.new_mode = "160000";
    diff_hunk hunk {};
    hunk.lines = { { diff_line_kind::del, 1, 0, "Subproject commit " + sha_old }, { diff_line_kind::add, 0, 1, "Subproject commit " + sha_new + "-dirty" } };
    file.hunks = { hunk };

    CHECK(file.is_gitlink());
    CHECK(file.is_special());
    const std::optional<diff_special_view> card { build_diff_special(file) };
    REQUIRE(card.has_value());
    CHECK(card->kind == diff_special_kind::submodule_pointer);
    CHECK(card->title == u8"서브모듈 포인터");
    CHECK(card->rows[1].value == u8"1ecfd40f4893");
    CHECK(card->rows[2].value.starts_with(u8"9b8895860a44"));
    CHECK(card->rows[2].value.find(u8"커밋하지 않은 변경") != std::u8string::npos);
    CHECK(card->open_path == "libs/a");
    CHECK_FALSE(card->action.has_value());

    // 제거된 서브모듈은 열 수 없다.
    file.status = 'D';
    file.hunks[0].lines = { { diff_line_kind::del, 1, 0, "Subproject commit " + sha_old } };
    const std::optional<diff_special_view> removed { build_diff_special(file) };
    REQUIRE(removed.has_value());
    CHECK(removed->open_path.empty());
    CHECK(removed->rows[2].value.find(u8"제거") != std::u8string::npos);
}

TEST_CASE("the snapshot builder shows the card for a loaded LFS pointer diff", "[presentation][extension][diff]")
{
    snap::snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo");
    repository_session& session { *harness.sessions["repo"] };
    snap::load(session, snap::linear_history(2), 1);
    const diff_file_request request { .path = "art/big.bin" };
    file_diff diff {};
    diff.request = request;
    diff.files = { pointer_diff(lfs_pointer { std::string(64, 'a'), 5000 }, lfs_pointer { std::string(64, 'b'), 6000 }, true, false) };
    session.expect_file_diff(1);
    REQUIRE(session.apply(1, file_diff_loaded { diff }));
    harness.ui.tabs[harness.tabs.selected().id].diff = request;

    const view_snapshot view { harness.build() };
    REQUIRE(view.repository.has_value());
    REQUIRE(view.repository->diff.has_value());
    REQUIRE(view.repository->diff->special.has_value());
    CHECK(view.repository->diff->special->title == u8"LFS 포인터");
    CHECK(view.repository->diff->lines.empty());
}

TEST_CASE("the diff controller hands LFS pointers to the card or to the binary and image views", "[application][extension][diff]")
{
    const lfs_pointer before { std::string(64, 'a'), 5000 };
    const lfs_pointer after { std::string(64, 'b'), 6000 };
    const auto show = [](diff_view_controller& controller, diff_file file) {
        file_diff diff {};
        diff.request.path = file.display_path();
        diff.files = { std::move(file) };
        return controller.show(diff, u8"C:/work/repo");
    };

    diff_view_controller card_controller {};
    CHECK(show(card_controller, pointer_diff(before, after, true, false)).empty());
    CHECK(card_controller.content() == diff_content::special);
    CHECK_FALSE(card_controller.can_stage());

    // 실제 내용이 로컬에 있고 이미지면 LFS 객체를 꺼내 이미지 비교를 한다.
    diff_view_controller image_controller {};
    diff_file image { pointer_diff(before, after, true, true) };
    image.old_path = image.new_path = "art/logo.png";
    const std::vector<diff_view_effect> image_effects { show(image_controller, image) };
    REQUIRE(image_effects.size() == 1);
    const auto* const prepare { std::get_if<prepare_sides_effect>(&image_effects.front()) };
    REQUIRE(prepare != nullptr);
    CHECK(prepare->plan.lfs);
    CHECK(prepare->write_files);
    CHECK(image_controller.content() == diff_content::loading);

    // 이미지가 아니면 바이너리 카드(크기만)다.
    diff_view_controller binary_controller {};
    const std::vector<diff_view_effect> binary_effects { show(binary_controller, pointer_diff(before, after, true, true)) };
    REQUIRE(binary_effects.size() == 1);
    CHECK_FALSE(std::get<prepare_sides_effect>(binary_effects.front()).write_files);

    // gitlink도 카드다.
    diff_view_controller gitlink_controller {};
    diff_file gitlink {};
    gitlink.old_path = gitlink.new_path = "libs/a";
    gitlink.old_mode = gitlink.new_mode = "160000";
    CHECK(show(gitlink_controller, gitlink).empty());
    CHECK(gitlink_controller.content() == diff_content::special);
}

// ---------------------------------------------------------------- controller

TEST_CASE("double-clicking a submodule opens it as its own tab, and an uninitialized one is updated first", "[presentation][extension][controller]")
{
    harness test {};
    test.load();
    const std::size_t tabs_before { test.controller.tabs().tabs().size() };

    test.send(activate_sidebar_item_intent { sidebar_item_kind::submodule, "libs/b" });
    const auto* const update { test.last_request<submodule_update_request>() };
    REQUIRE(update != nullptr);
    CHECK(update->init);
    CHECK(update->recursive);
    CHECK(update->paths == std::vector<std::string> { "libs/b" });
    CHECK(test.controller.tabs().tabs().size() == tabs_before);

    test.send(activate_sidebar_item_intent { sidebar_item_kind::submodule, "libs/a" });
    REQUIRE(test.controller.tabs().tabs().size() == tabs_before + 1);
    CHECK(test.controller.tabs().find_repository(u8"C:/work/repo/libs/a") == test.controller.tabs().selected_index());
    // 같은 submodule을 다시 열면 새 탭이 아니라 그 탭이다.
    test.send(activate_sidebar_item_intent { sidebar_item_kind::submodule, "libs/a" });
    CHECK(test.controller.tabs().tabs().size() == tabs_before + 1);
}

TEST_CASE("the submodule menu opens, updates and copies the path", "[presentation][extension][controller]")
{
    harness test {};
    test.load();
    const std::size_t before { test.operation_count() };

    test.menu(menu_target { .kind = menu_target_kind::submodule, .name = "libs/a" }, u8"update");
    REQUIRE(test.operation_count() == before + 1);
    CHECK(test.last_request<submodule_update_request>()->paths == std::vector<std::string> { "libs/a" });

    test.menu(menu_target { .kind = menu_target_kind::submodule, .name = "libs/a" }, u8"copy_path");
    CHECK(test.controller.take_clipboard_text() == std::optional<std::u8string> { u8"libs/a" });

    // 초기화하지 않은 submodule은 열 수 없다 (항목이 꺼져 있어 아무 일도 일어나지 않는다).
    const std::size_t tabs { test.controller.tabs().tabs().size() };
    test.menu(menu_target { .kind = menu_target_kind::submodule, .name = "libs/b" }, u8"open");
    CHECK(test.controller.tabs().tabs().size() == tabs);

    test.menu(menu_target { .kind = menu_target_kind::submodules_section }, u8"sync_all");
    CHECK(test.last_request<submodule_sync_request>() != nullptr);

    test.menu(menu_target { .kind = menu_target_kind::submodule, .name = "libs/a" }, u8"open");
    CHECK(test.controller.tabs().tabs().size() == tabs + 1);
}

TEST_CASE("opening a worktree makes a tab of its own, and the main worktree is the tab already open", "[presentation][extension][controller]")
{
    harness test {};
    test.load();
    const std::size_t tabs { test.controller.tabs().tabs().size() };

    test.send(activate_sidebar_item_intent { sidebar_item_kind::worktree, "C:/work/repo" });
    CHECK(test.controller.tabs().tabs().size() == tabs);
    test.send(activate_sidebar_item_intent { sidebar_item_kind::worktree, "C:/work/repo-old" });
    CHECK(test.controller.tabs().tabs().size() == tabs);
    REQUIRE(test.controller.state().notice.has_value());

    test.send(activate_sidebar_item_intent { sidebar_item_kind::worktree, "C:/work/repo-feat" });
    REQUIRE(test.controller.tabs().tabs().size() == tabs + 1);
    CHECK(test.controller.tabs().find_repository(u8"C:\\work\\repo-feat") == test.controller.tabs().selected_index());
}

TEST_CASE("removing a worktree asks first and can be forced", "[presentation][extension][controller]")
{
    harness test {};
    test.load();
    const std::size_t before { test.operation_count() };

    test.menu(menu_target { .kind = menu_target_kind::worktree, .name = "C:/work/repo-old" }, u8"remove");
    REQUIRE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().dialog->kind == dialog_kind::confirm);
    CHECK(test.controller.state().dialog->danger);
    // 잠긴 worktree라는 안내가 들어 있다.
    const bool mentions_lock { std::ranges::any_of(test.controller.state().dialog->lines, [](const std::u8string& line) { return line.find(u8"잠긴") != std::u8string::npos; }) };
    CHECK(mentions_lock);
    CHECK(test.operation_count() == before);

    test.send(form_button_intent { u8"submit" });
    const auto* const plain { test.last_request<worktree_remove_request>() };
    REQUIRE(plain != nullptr);
    CHECK(plain->path == u8"C:/work/repo-old");
    CHECK_FALSE(plain->force);

    // 체크를 켜면 강제 제거다.
    test.menu(menu_target { .kind = menu_target_kind::worktree, .name = "C:/work/repo-old" }, u8"remove");
    test.send(form_check_intent { 0 });
    test.send(form_button_intent { u8"submit" });
    CHECK(test.last_request<worktree_remove_request>()->force);

    // 주 작업 트리의 항목은 꺼져 있어 확인 상자도 뜨지 않는다.
    test.menu(menu_target { .kind = menu_target_kind::worktree, .name = "C:/work/repo" }, u8"remove");
    CHECK_FALSE(test.controller.state().dialog.has_value());
}

TEST_CASE("the worktree dialog creates a new branch tree and prune runs directly", "[presentation][extension][controller]")
{
    harness test {};
    test.load();

    test.menu(menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(2) }, u8"add_worktree");
    REQUIRE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().dialog->kind == dialog_kind::add_worktree);
    CHECK(test.controller.state().dialog->subject == snap::sha_of(2));
    // 폴더 칸은 저장소 옆의 이름이 미리 채워져 있다.
    const view_snapshot opened { test.view() };
    REQUIRE(opened.form.has_value());
    CHECK(opened.form->id == u8"worktree-dialog");
    REQUIRE(opened.form->fields.size() == 2);
    CHECK(opened.form->fields[0].view.displayed_text().starts_with(u8"C:\\work\\repo-"));
    // 브랜치 이름이 없으면 추가할 수 없다.
    CHECK_FALSE(opened.form->buttons.back().enabled);

    test.type(1, u8"topic");
    test.send(form_button_intent { u8"submit" });
    const auto* const request { test.last_request<worktree_add_request>() };
    REQUIRE(request != nullptr);
    CHECK(request->new_branch == "topic");
    CHECK(request->start_point == snap::sha_of(2));
    CHECK(request->path.starts_with(u8"C:\\work\\repo-"));
    CHECK_FALSE(test.controller.state().dialog.has_value());

    test.menu(menu_target { .kind = menu_target_kind::worktrees_section }, u8"prune");
    CHECK(test.last_request<worktree_prune_request>() != nullptr);
}

TEST_CASE("a branch used by another worktree is refused with a clear message instead of a checkout", "[presentation][extension][controller]")
{
    harness test {};
    test.load();
    const std::size_t before { test.operation_count() };

    test.send(activate_ref_intent { ref_kind::local, "feature/graph" });
    CHECK(test.operation_count() == before);
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->warning);
    CHECK(test.controller.state().notice->text.find(u8"다른 worktree") != std::u8string::npos);
    CHECK(test.controller.state().notice->text.find(u8"C:/work/repo-feat") != std::u8string::npos);

    // 메뉴의 "그 worktree 열기"는 그 폴더를 새 탭으로 연다.
    const std::size_t tabs { test.controller.tabs().tabs().size() };
    test.menu(menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, u8"open_worktree");
    CHECK(test.controller.tabs().tabs().size() == tabs + 1);
    CHECK(test.controller.tabs().find_repository(u8"C:\\work\\repo-feat") == test.controller.tabs().selected_index());
}

TEST_CASE("LFS menu items run the operations and the track dialog suggests the extension", "[presentation][extension][controller]")
{
    harness test {};
    test.load();

    test.menu(menu_target { .kind = menu_target_kind::lfs_section }, u8"pull");
    CHECK(test.last_request<lfs_pull_request>() != nullptr);
    test.menu(menu_target { .kind = menu_target_kind::lfs_section }, u8"fetch_all");
    REQUIRE(test.last_request<lfs_fetch_request>() != nullptr);
    CHECK(test.last_request<lfs_fetch_request>()->all);
    test.menu(menu_target { .kind = menu_target_kind::lfs_file, .name = "art/pointer.bin" }, u8"pull");
    CHECK(test.last_request<lfs_pull_request>() != nullptr);

    test.menu(menu_target { .kind = menu_target_kind::file, .name = "art/logo.psd", .side = change_side::unstaged }, u8"lfs_track");
    REQUIRE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().dialog->kind == dialog_kind::lfs_track);
    const view_snapshot opened { test.view() };
    REQUIRE(opened.form.has_value());
    CHECK(opened.form->fields[0].view.displayed_text() == u8"*.psd");
    test.send(form_button_intent { u8"submit" });
    const auto* const track { test.last_request<lfs_track_request>() };
    REQUIRE(track != nullptr);
    CHECK(track->pattern == "*.psd");
    CHECK_FALSE(test.controller.state().dialog.has_value());
}

TEST_CASE("the new operations have names, lanes and a display name", "[application][extension]")
{
    CHECK(operation_kind_of(worktree_add_request {}) == operation_kind::worktree_add);
    CHECK(operation_kind_of(lfs_pull_request {}) == operation_kind::lfs_pull);
    CHECK(operation_kind_of(submodule_sync_request {}) == operation_kind::submodule_sync);
    CHECK(operation_kind_name(operation_kind::lfs_track) == "lfs_track");
    CHECK(operation_kind_name(operation_kind::worktree_prune) == "worktree_prune");
    CHECK(failure_kind_name(failure_kind::branch_checked_out_elsewhere) == "branch_checked_out_elsewhere");
    const std::vector<operation_kind> kinds { operation_kind::submodule_update, operation_kind::submodule_sync, operation_kind::worktree_add, operation_kind::worktree_remove };
    std::vector<operation_kind> all_kinds { kinds };
    all_kinds.insert(all_kinds.end(), { operation_kind::worktree_prune, operation_kind::lfs_pull, operation_kind::lfs_fetch, operation_kind::lfs_track });
    for (const operation_kind kind : all_kinds)
    {
        CHECK(operation_lane_of(kind) == operation_lane::write);
        CHECK_FALSE(is_setup_operation(kind));
        CHECK(operation_display_name(kind).empty() == false);
    }
}
