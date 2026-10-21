// 커밋 상세 판의 규칙이다: 변경 파일 트리, 제목·본문 줄 나누기, 작성자 사진 받기, snapshot과 luil tree.
#include "infrastructure/avatar_fetcher.h"
#include "presentation/file_tree.h"
#include "presentation/text_wrap.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"

#include "git_test_support.h"
#include "view_snapshot_support.h"

#include "luil/ui/ui_tree.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace cyella;
using namespace cyella::test::snapshot;

namespace {
    // byte 하나가 폭 1인 가짜 글꼴이다. 줄 나누기 규칙만 본다.
    float byte_width(const std::u8string_view text)
    {
        return static_cast<float>(text.size());
    }

    std::vector<std::u8string> wrapped(const std::u8string_view text, const float width)
    {
        std::vector<std::u8string> lines {};
        for (const text_line& line : wrap_text_lines(text, width, byte_width))
            lines.emplace_back(text.substr(line.begin, line.end - line.begin));
        return lines;
    }

    std::vector<std::string> labels_of(const std::vector<file_tree_row>& rows)
    {
        std::vector<std::string> labels {};
        for (const file_tree_row& row : rows)
            labels.push_back(std::string(row.depth * 2, ' ') + (row.folder ? "[" : "") + std::string { row.label.begin(), row.label.end() } + (row.folder ? "]" : ""));
        return labels;
    }

    // 고른 커밋의 `show_commit` 결과를 세션에 넣는다.
    void load_detail(repository_session& session, const std::string& sha, std::vector<file_change> files)
    {
        commit_detail detail {};
        for (const commit_info& commit : session.graph_commits())
            if (commit.sha == sha)
                detail.commit = commit;
        detail.commit.author_email = "lusain@example.com";
        detail.commit.body = "- 첫 줄\n- 둘째 줄\n";
        detail.files = std::move(files);
        session.expect_commit_detail(operation_id { 7 });
        REQUIRE(session.apply(operation_id { 7 }, commit_detail_loaded { detail }));
    }

    std::vector<file_change> sample_files()
    {
        return {
            { .path = "README.md", .status = 'M', .side = change_side::unstaged, .lines_added = 1 },
            { .path = "src/presentation/ui/repository_ui.cpp", .status = 'M', .side = change_side::unstaged, .lines_added = 40, .lines_removed = 12 },
            { .path = "src/presentation/ui/list_rows.cpp", .status = 'M', .side = change_side::unstaged, .lines_added = 5 },
            { .path = "src/presentation/file_tree.cpp", .status = 'A', .side = change_side::unstaged, .lines_added = 60 },
            { .path = "tests/commit_detail_tests.cpp", .status = 'A', .side = change_side::unstaged, .lines_added = 9, .lines_removed = 2 },
        };
    }
} // namespace

TEST_CASE("file tree puts folders first and folds single-folder chains", "[presentation][file_tree]")
{
    const std::vector<std::string> paths {
        "README.md",
        "src/presentation/ui/repository_ui.cpp",
        "src/presentation/ui/list_rows.cpp",
        "src/presentation/file_tree.cpp",
        "tests/commit_detail_tests.cpp",
    };
    const std::vector<file_tree_row> rows { build_file_tree(paths, {}) };
    CHECK(labels_of(rows)
        == std::vector<std::string> {
            "[src/presentation]",
            "  [ui]",
            "    list_rows.cpp",
            "    repository_ui.cpp",
            "  file_tree.cpp",
            "[tests]",
            "  commit_detail_tests.cpp",
            "README.md",
        });
    // 합친 행의 경로는 가장 아래 폴더이고, 수는 그 아래 파일 모두다.
    CHECK(rows[0].path == "src/presentation");
    CHECK(rows[0].key == u8"d:src/presentation");
    CHECK(rows[0].file_count == 3);
    CHECK(rows[1].path == "src/presentation/ui");
    CHECK(rows[2].file_index == 2);
    CHECK(rows[7].file_index == 0);
}

TEST_CASE("collapsed folders hide what is under them but keep their count", "[presentation][file_tree]")
{
    const std::vector<std::string> paths { "a/b/one.txt", "a/b/two.txt", "a/three.txt", "four.txt" };
    const std::vector<file_tree_row> rows { build_file_tree(paths, { "a/b" }) };
    CHECK(labels_of(rows) == std::vector<std::string> { "[a]", "  [b]", "  three.txt", "four.txt" });
    CHECK_FALSE(rows[1].expanded);
    CHECK(rows[1].file_count == 2);

    CHECK(labels_of(build_file_tree(paths, { "a" })) == std::vector<std::string> { "[a]", "four.txt" });
    CHECK(build_file_tree({}, {}).empty());
}

TEST_CASE("wrapping breaks at spaces, keeps newlines and splits only overlong words", "[presentation][text_wrap]")
{
    CHECK(wrapped(u8"aaa bbb ccc", 7.0f) == std::vector<std::u8string> { u8"aaa bbb", u8"ccc" });
    CHECK(wrapped(u8"aaa bbb ccc", 100.0f) == std::vector<std::u8string> { u8"aaa bbb ccc" });
    CHECK(wrapped(u8"first\n\nthird\r\n", 100.0f) == std::vector<std::u8string> { u8"first", u8"", u8"third", u8"" });
    CHECK(wrapped(u8"abcdefghij kl", 4.0f) == std::vector<std::u8string> { u8"abcd", u8"efgh", u8"ij", u8"kl" });
    // 본문의 들여쓰기는 남는다.
    CHECK(wrapped(u8"  - item", 100.0f) == std::vector<std::u8string> { u8"  - item" });
    // 한글 어절은 깨지지 않는다 (글자가 3 byte라 폭 6은 두 글자다).
    CHECK(wrapped(u8"가나 다라", 6.0f) == std::vector<std::u8string> { u8"가나", u8"다라" });
    CHECK(wrapped(u8"abc", 0.0f) == std::vector<std::u8string> { u8"abc" });
}

TEST_CASE("the width estimate is generous for Korean and Latin text", "[presentation][text_wrap]")
{
    CHECK(estimate_text_width(u8"가나다", 12.0f, false) == 36.0f);
    CHECK(estimate_text_width(u8"abc", 10.0f, false) > 15.0f);
    CHECK(estimate_text_width(u8"abc", 10.0f, true) > estimate_text_width(u8"abc", 10.0f, false));
    CHECK(estimate_wrapped_line_count(u8"", 100.0f, 12.0f, false) == 0);
    CHECK(estimate_wrapped_line_count(u8"가나다 라마바 사아자", 80.0f, 12.0f, false) == 2);
}

TEST_CASE("commit detail snapshot splits author fields and builds the file tree on request", "[presentation][snapshot][file_tree]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(3), 0);
    harness.ui.tabs[tab].selected_sha = sha_of(1);
    load_detail(*harness.sessions["repo-1"], sha_of(1), sample_files());

    view_snapshot view { harness.build() };
    REQUIRE(view.repository->commit.has_value());
    const commit_detail_view* commit { &*view.repository->commit };
    CHECK_FALSE(commit->loading);
    CHECK(commit->author_name == u8"Lusain");
    CHECK(commit->author_email == u8"lusain@example.com");
    CHECK(commit->date_absolute.size() >= 16);
    CHECK(commit->parent_shas == std::vector<std::string> { sha_of(0) });
    CHECK(commit->added_total == u8"+115");
    CHECK(commit->removed_total == u8"-14");
    CHECK_FALSE(commit->files_tree);
    CHECK(commit->tree.empty());

    harness.settings.ui.commit_files_tree = true;
    harness.ui.tabs[tab].collapsed_folders = { "tests" };
    view = harness.build();
    commit = &*view.repository->commit;
    CHECK(commit->files_tree);
    CHECK(labels_of(commit->tree) == std::vector<std::string> { "[src/presentation]", "  [ui]", "    list_rows.cpp", "    repository_ui.cpp", "  file_tree.cpp", "[tests]", "README.md" });
}

TEST_CASE("commit diff navigation preserves its commit and never enters working changes", "[presentation][diff]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(3), 2);
    const std::string sha { sha_of(1) };
    harness.ui.tabs[tab].selected_sha = sha;
    load_detail(*harness.sessions["repo-1"], sha, sample_files());
    harness.ui.tabs[tab].diff = diff_file_request { .path = "README.md", .view = diff_view::commit, .sha = sha, .context_lines = 10 };
    const repository_view repository { *harness.build().repository };
    const diff_panel_view& diff { *repository.diff };
    CHECK(diff.file_position == 1);
    CHECK(diff.file_count == sample_files().size());
    CHECK_FALSE(diff.previous_file.has_value());
    REQUIRE(diff.next_file.has_value());
    CHECK(diff.next_file->path == repository.commit->files[1].path);
    CHECK(diff.next_file->view == diff_view::commit);
    CHECK(diff.next_file->sha == sha);
    CHECK(diff.next_file->context_lines == 10);
}

TEST_CASE("commit detail tree shows wrapped text, label columns, folder rows and the view toggle", "[presentation][ui][file_tree]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(3), 0);
    harness.ui.tabs[tab].selected_sha = sha_of(1);
    load_detail(*harness.sessions["repo-1"], sha_of(1), sample_files());
    harness.settings.ui.commit_files_tree = true;

    const ui::built_tree built { ui::build_ui_tree(harness.build()) };
    const auto find = [&built](const luil::ui_element_kind kind, const std::u8string& owner) { return built.tree->find(luil::ui_element_id { kind, owner }); };
    // 제목·본문·작성 정보가 한 카드다. 작성자는 이름만 보이고 메일은 툴팁·보조 기술 이름에 있다.
    CHECK(find(ui::kind_panel, u8"commit-message-card") != nullptr);
    CHECK(find(ui::kind_label, u8"detail-subject") != nullptr);
    CHECK(find(ui::kind_label, u8"detail-body") != nullptr);
    const luil::ui_element* const author { find(ui::kind_label, u8"detail-author") };
    REQUIRE(author != nullptr);
    CHECK(author->accessibility().name == u8"Lusain <lusain@example.com>");
    CHECK(find(ui::kind_label, u8"detail-date") != nullptr);
    CHECK(find(ui::kind_label, u8"detail-sha-label") != nullptr);
    CHECK(find(ui::kind_label, u8"detail-sha") != nullptr);
    CHECK(find(ui::kind_text_button, u8"detail-parent-0") != nullptr);
    // 고른 커밋(1) 위에 HEAD 커밋(2) 하나가 있다. 단추가 그 수를 말한다.
    const luil::ui_element* const rebase { find(ui::kind_text_button, u8"detail-interactive-rebase") };
    REQUIRE(rebase != nullptr);
    CHECK(rebase->accessibility().name == u8"이후 커밋 1개 편집…");
    CHECK(rebase->enabled());
    CHECK(find(ui::kind_file_row, u8"folder-d:src/presentation") != nullptr);
    // 파일 행을 마우스로 누르면 diff가 열린다. 행 가운데의 hit는 경로 툴팁을 단 파일 행 element가 받으므로, 그 element가
    // 고르기(diff 열기)를 직접 내야 한다. 내지 않으면 누름이 삼켜져 키보드로만 열렸다.
    const luil::ui_element* const file_row { find(ui::kind_file_row, u8"file-f:src/presentation/file_tree.cpp") };
    REQUIRE(file_row != nullptr);
    const luil::rect_f box { file_row->bounds() };
    const luil::ui_element* const hit { built.tree->hit_test(box.x + box.width / 3.0f, box.y + box.height / 2.0f) };
    REQUIRE(hit != nullptr);
    CHECK(hit->id() == file_row->id());
    const auto* const click { hit->action(luil::ui_trigger::left_click) };
    REQUIRE(click != nullptr);
    const std::vector<luil::input_action> actions { (*click)(luil::ui_action_context {}) };
    REQUIRE(actions.size() == 1);
    // 우클릭은 커밋 파일 메뉴를 연다.
    CHECK(hit->action(luil::ui_trigger::right_click) != nullptr);
    const luil::ui_element* const tree_button { built.tree->find(luil::ui_element_id { ui::kind_segment, u8"detail-files-tree" }) };
    CHECK(tree_button != nullptr);
}

namespace {
    // 보낸 주소를 적고 준비한 답을 차례로 돌려준다.
    class avatar_transport final : public hosting_transport
    {
    public:
        hosting_http_response execute(const hosting_http_request& request, const secret_text&, const process_cancellation_token&) override
        {
            urls.push_back(request.url);
            hosting_http_response response {};
            if (answers.empty())
            {
                response.transport_error = "offline";
                return response;
            }
            response = answers.front();
            answers.erase(answers.begin());
            return response;
        }

        std::vector<std::string> urls {};
        std::vector<hosting_http_response> answers {};
    };

    hosting_http_response answer(const int status, std::string body = {})
    {
        hosting_http_response response {};
        response.status_code = status;
        response.body = std::move(body);
        return response;
    }

    avatar_fetch_options avatar_options(const std::filesystem::path& cache)
    {
        avatar_fetch_options options {};
        options.cache_directory = cache;
        // 해시 자체는 플랫폼(CNG)이 한다. 여기서는 다듬은 메일이 그대로 가는지만 본다.
        options.sha256_hex = [](const std::string_view text) { return "hash-" + std::string { text }; };
        return options;
    }
} // namespace

TEST_CASE("gravatar addresses use the trimmed lowercase email and ask for 404 when there is no picture", "[presentation][avatar]")
{
    CHECK(normalize_avatar_email("  Lusain@Example.COM\t") == "lusain@example.com");
    CHECK(gravatar_url("abc", 80) == "https://gravatar.com/avatar/abc?s=80&d=404");
}

TEST_CASE("avatar fetcher keeps pictures and missing answers on disk and asks once", "[presentation][avatar]")
{
    const cyella::test::temp_directory directory {};
    avatar_transport transport {};
    const avatar_fetcher fetcher { transport, avatar_options(directory.path()) };
    const process_cancellation_source cancellation {};

    transport.answers = { answer(200, "PNGDATA") };
    const avatar_fetch_result first { fetcher.fetch(" Ann@Example.com ", cancellation.token()) };
    CHECK(first.bytes == "PNGDATA");
    CHECK_FALSE(first.missing);
    REQUIRE(transport.urls.size() == 1);
    CHECK(transport.urls.front() == "https://gravatar.com/avatar/hash-ann@example.com?s=80&d=404");
    // 두 번째는 디스크에서 읽고 묻지 않는다.
    CHECK(fetcher.fetch("ann@example.com", cancellation.token()).bytes == "PNGDATA");
    CHECK(transport.urls.size() == 1);

    transport.answers = { answer(404) };
    CHECK(fetcher.fetch("nobody@example.com", cancellation.token()).missing);
    CHECK(fetcher.fetch("nobody@example.com", cancellation.token()).missing);
    CHECK(transport.urls.size() == 2);

    // 받지 못했고 보관한 것도 없으면 실패다 (다음에 다시 묻는다). 메일이 아니면 묻지 않는다.
    const avatar_fetch_result offline { fetcher.fetch("new@example.com", cancellation.token()) };
    CHECK(offline.failed);
    CHECK(offline.bytes.empty());
    CHECK(fetcher.fetch("not an email", cancellation.token()).missing);
    CHECK(transport.urls.size() == 3);
}

TEST_CASE("the rebase button counts the current branch commits and words a commit off the branch differently", "[presentation][ui][rebase]")
{
    // c1 ← c2(다른 갈래 끝), c1 ← c3(HEAD) 꼴이다. topo 순서로 HEAD가 맨 앞이다.
    std::vector<commit_info> commits(3);
    commits[0] = { .sha = sha_of(2), .parents = { sha_of(0) }, .author_name = "Lusain", .author_time = 1700000000, .subject = "head" };
    commits[1] = { .sha = sha_of(1), .parents = { sha_of(0) }, .author_name = "Lusain", .author_time = 1700000000, .subject = "other" };
    commits[2] = { .sha = sha_of(0), .author_name = "Lusain", .author_time = 1700000000, .subject = "root" };

    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], commits, 0);
    const auto button_name = [&harness, tab](const std::string& sha) {
        harness.ui.tabs[tab].selected_sha = sha;
        const ui::built_tree built { ui::build_ui_tree(harness.build()) };
        const luil::ui_element* const button { built.tree->find(luil::ui_element_id { ui::kind_text_button, u8"detail-interactive-rebase" }) };
        REQUIRE(button != nullptr);
        return std::make_pair(button->accessibility().name, button->enabled());
    };

    CHECK(button_name(sha_of(0)) == std::make_pair(std::u8string { u8"이후 커밋 1개 편집…" }, true));
    // 다른 갈래 끝을 고르면 현재 브랜치(c3)를 그 위로 옮겨 쌓는다 (인터랙티브 rebase의 "main 끝에서 여기서부터"와 같은 쓰임).
    CHECK(button_name(sha_of(1)) == std::make_pair(std::u8string { u8"현재 브랜치 커밋 1개를 이 위로 편집…" }, true));
    CHECK(button_name(sha_of(2)) == std::make_pair(std::u8string { u8"이후 커밋 편집…" }, false));
}
