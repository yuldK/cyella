#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 인터랙티브 rebase의 할 일 목록(todo)을 다루는 순수 규칙이다. 앱은 git의 편집기를 띄우지 않고, 사용자가
    // 화면에서 고친 목록을 이 규칙으로 검사해 git의 `git-rebase-todo` 형식으로 쓴다. 결정과 이유는
    // `docs/decisions/ADR-005-interactive-rebase.md`에 있다.
    //
    // 이 파일은 편집기 도우미(`cyella-rebase-editor.exe`)도 직접 컴파일한다. 그래서 표준 라이브러리 밖에
    // 기대지 않는다.

    // 할 일 한 줄의 동작이다. git의 명령어 이름과 같은 뜻이다.
    enum class rebase_action
    {
        pick,
        // 메시지를 바꾼다. 새 메시지는 `rebase_todo_entry::message`다.
        reword,
        // 앞 커밋에 합치고 두 메시지를 합친다.
        squash,
        // 앞 커밋에 합치고 이 커밋의 메시지는 버린다.
        fixup,
        // `fixup -C`: 앞 커밋에 합치고 메시지는 이 커밋의 것을 쓴다 (`amend!` 커밋의 뜻).
        fixup_keep_message,
        drop,
    };

    // git의 todo 명령어다 (`pick`, `fixup -C` …).
    [[nodiscard]] std::string_view rebase_action_word(rebase_action action) noexcept;
    // 화면의 이름이다 (`pick`, `fixup -C`). git 명령어와 같지만 화면 문구를 한곳에 둔다.
    [[nodiscard]] std::u8string_view rebase_action_label(rebase_action action) noexcept;
    // 편집기의 한 글자 단축키(p r s f d)다. `fixup -C`는 단축키가 없다.
    [[nodiscard]] std::optional<rebase_action> rebase_action_for_key(char32_t key) noexcept;
    // 앞 커밋에 합치는 동작이다 (squash, fixup, fixup -C).
    [[nodiscard]] bool is_meld_action(rebase_action action) noexcept;

    struct rebase_todo_entry
    {
        rebase_action action { rebase_action::pick };
        // 커밋 SHA 전체다. git이 쓴 `done` 파일과 짝을 맞추려면 줄인 SHA를 쓰면 안 된다.
        std::string sha {};
        // reword면 새 메시지다. squash가 든 묶음의 **마지막 줄**이면 합친 메시지다. 그 밖에는 비운다.
        // 값이 없으면 git이 정하는 메시지를 그대로 쓴다.
        std::optional<std::string> message {};

        [[nodiscard]] bool operator==(const rebase_todo_entry&) const = default;
    };

    // `fixup!`·`squash!`·`amend!`로 시작하는 커밋의 뜻이다. 편집기가 자동 정렬과 표시에 쓴다.
    enum class autosquash_marker
    {
        none,
        fixup,
        squash,
        amend,
    };

    // 편집기를 처음 채울 커밋 하나다. 순서는 git이 적용할 순서(오래된 것부터)다.
    struct rebase_commit
    {
        std::string sha {};
        std::string subject {};
        // 제목 뒤의 본문이다 (빈 줄 다음부터). 없으면 빈 문자열이다.
        std::string body {};
        std::string author_name {};
        std::int64_t author_time { 0 };
        // 이미 어느 원격 브랜치에 있는 커밋이다. 고쳐 쓰면 강제 push가 필요하다.
        bool pushed { false };
    };

    // 편집기를 여는 데 필요한 범위다. `load_rebase_plan`의 결과다.
    struct rebase_plan
    {
        // 요청한 값 그대로다 (브랜치 이름이나 SHA).
        std::string onto {};
        std::string branch {};
        std::string onto_sha {};
        std::string head_sha {};
        // git이 todo에 넣을 커밋과 같다 (`log --right-only --cherry-pick --max-parents=1 onto...branch`).
        std::vector<rebase_commit> commits {};
        // 범위 안의 병합 커밋 수다. 0이 아니면 rebase 뒤 이력이 한 줄로 펴진다.
        std::uint32_t merge_count { 0 };
        // 이미 upstream에 올라간 원격 브랜치가 있다 (`pushed` 판정의 근거).
        bool has_remote_branches { false };
    };

    enum class rebase_todo_problem
    {
        empty,
        // 첫 줄(drop을 뺀)이 squash·fixup이다. 합칠 앞 커밋이 없다.
        starts_with_meld,
        unknown_commit,
        duplicate_commit,
        // 범위의 커밋이 목록에서 빠졌다. 빠진 커밋은 git이 지우므로 drop으로 적어야 한다.
        missing_commit,
        reword_without_message,
        // 메시지를 받을 수 없는 줄에 메시지가 있다 (pick, drop, squash 묶음의 가운데 줄).
        message_not_allowed,
        // 메시지가 비었거나 내용이 공백·주석뿐이다.
        empty_message,
    };

    struct rebase_todo_issue
    {
        rebase_todo_problem problem { rebase_todo_problem::empty };
        // 문제가 난 줄이다. `empty`·`missing_commit`이면 의미가 없다.
        std::size_t index { 0 };
        std::string sha {};
    };

    // 목록을 범위(`range_shas`, 전체 SHA)와 맞춰 검사한다. 문제가 없으면 값이 없다.
    //  - 비어 있지 않다.
    //  - 모든 SHA가 범위에 있고 한 번씩만 나온다. 범위의 커밋이 모두 나온다 (지우는 것은 drop으로 적는다).
    //  - drop을 뺀 첫 줄이 squash·fixup이 아니다.
    //  - reword는 메시지가 있다. 메시지는 reword 줄과 squash가 든 묶음의 마지막 줄에만 둔다.
    [[nodiscard]] std::optional<rebase_todo_issue> find_rebase_todo_problem(std::span<const rebase_todo_entry> todo, std::span<const std::string> range_shas);
    [[nodiscard]] std::u8string rebase_todo_problem_text(const rebase_todo_issue& issue);

    // git의 `git-rebase-todo` 내용이다. 한 줄에 `<명령> <SHA 전체>`이고 줄 끝은 LF, 마지막 줄도 LF로 끝난다.
    // 주석과 제목은 쓰지 않는다 (제목이 `#`나 이상한 글자를 가져도 해석이 흔들리지 않는다).
    [[nodiscard]] std::string serialize_rebase_todo(std::span<const rebase_todo_entry> todo);

    // 묶음 하나다. pick·reword 하나와 그 뒤에 이어지는 squash·fixup 줄들이다. drop은 묶음에 들지 않는다.
    struct rebase_group
    {
        // 묶음의 첫 줄과 마지막 줄의 자리다 (todo 기준). 사이의 drop 줄도 범위 안에 있을 수 있다.
        std::size_t head { 0 };
        std::size_t last { 0 };
        // 묶음에 든 줄의 자리다 (drop 제외).
        std::vector<std::size_t> members {};
        // squash(또는 fixup -C 뒤 squash)가 하나라도 있다. git이 묶음 끝에서 편집기를 열어 합친 메시지를 묻는다.
        bool has_squash { false };
    };

    // 목록을 묶음으로 나눈다. 앞에 묶일 커밋이 없는 squash·fixup은 그 줄만으로 묶음이 된다 (검사에서 걸러진다).
    [[nodiscard]] std::vector<rebase_group> rebase_groups(std::span<const rebase_todo_entry> todo);

    // 편집기 도우미가 쓸 메시지 파일 하나다. git이 `sha`의 줄을 처리하며 편집기를 열면 이 글로 바꾼다.
    struct rebase_message_file
    {
        std::string sha {};
        std::string message {};

        [[nodiscard]] bool operator==(const rebase_message_file&) const = default;
    };

    // 메시지가 있는 줄을 도우미 파일로 옮긴다. reword는 그 줄, squash 묶음의 합친 메시지는 묶음의 마지막 줄이다.
    // 줄 끝은 LF로 바꾸고 끝에 줄바꿈 하나를 둔다.
    [[nodiscard]] std::vector<rebase_message_file> rebase_message_files(std::span<const rebase_todo_entry> todo);

    // 줄 끝을 LF로 맞추고 끝의 빈 줄을 하나로 줄인다. 메시지 파일과 화면의 초안이 같은 모양이 된다.
    [[nodiscard]] std::string normalize_commit_message(std::string_view message);

    // ------------------------------------------------------------------ 편집기 초기값

    // 커밋 제목의 `fixup! `·`squash! `·`amend! ` 표지다 (첫 표지가 뜻을 정한다).
    [[nodiscard]] autosquash_marker autosquash_marker_of(std::string_view subject) noexcept;

    // 모두 pick이다.
    [[nodiscard]] std::vector<rebase_todo_entry> default_rebase_todo(std::span<const rebase_commit> commits);

    // git `--autosquash`와 같은 규칙으로 줄을 옮긴다. `fixup! X`는 앞쪽에서 제목이 X인 커밋(없으면 SHA가 X로
    // 시작하는 커밋, 그것도 없으면 제목이 X로 시작하는 커밋) 바로 뒤로 가서 fixup이 된다. `squash!`는 squash,
    // `amend!`는 fixup -C다. 같은 대상의 표지 커밋은 원래 순서대로 이어 붙는다. 대상을 못 찾으면 pick으로 남는다.
    [[nodiscard]] std::vector<rebase_todo_entry> autosquash_rebase_todo(std::span<const rebase_commit> commits);

    // 커밋 하나의 메시지 전체다 (제목, 빈 줄, 본문).
    [[nodiscard]] std::string full_commit_message(const rebase_commit& commit);

    // squash 묶음의 합친 메시지 초안이다. git과 같이 pick과 squash 줄의 메시지를 빈 줄로 잇고, fixup은 빼고,
    // fixup -C는 그때까지의 메시지를 그 커밋의 것으로 바꾼다. `messages`는 todo 줄마다의 현재 메시지다
    // (reword로 고친 메시지가 반영된 값).
    [[nodiscard]] std::string combined_squash_message(std::span<const rebase_todo_entry> todo, const rebase_group& group, std::span<const std::string> messages);

    // ------------------------------------------------------------------ 결과 미리보기

    struct rebase_result_commit
    {
        // 결과 커밋이 될 묶음의 첫 줄 자리다.
        std::size_t head { 0 };
        std::string subject {};
        // 합쳐진 커밋 수다 (묶음 줄 수).
        std::size_t combined { 1 };
        // 메시지가 바뀐다 (reword, squash, fixup -C).
        bool message_changed { false };
        // 원래 커밋과 다른 커밋이 된다 (합쳐짐, 메시지 바뀜, 앞 커밋이 바뀜).
        bool rewritten { false };
    };

    struct rebase_preview
    {
        std::vector<rebase_result_commit> commits {};
        std::size_t original_count { 0 };
        std::size_t dropped_count { 0 };
        // 순서가 바뀌었다.
        bool reordered { false };
    };

    // 목록대로 rebase한 뒤의 커밋이다 (오래된 것부터). `commits`는 todo의 SHA로 찾는 원래 커밋이다. 범위
    // 밖의 SHA는 제목이 비어 있다.
    [[nodiscard]] rebase_preview preview_rebase(std::span<const rebase_todo_entry> todo, std::span<const rebase_commit> commits);

    // ------------------------------------------------------------------ git이 쓴 파일 읽기 (편집기 도우미)

    // git todo 형식의 한 줄이다 (`pick abc123 # 제목`, `fixup -C abc123`, `p abc123`).
    struct rebase_todo_line
    {
        // 정규화한 명령어다 (`pick`, `reword`, `edit`, `squash`, `fixup`, `drop`). 줄인 명령어(`p`)도 풀어 둔다.
        std::string command {};
        // `fixup -C`·`fixup -c`의 깃발이다.
        std::string flag {};
        std::string sha {};
    };

    // 커밋을 다루는 줄이면 그 값이다. 빈 줄, 주석, `exec`·`label` 같은 커밋 없는 줄은 값이 없다.
    [[nodiscard]] std::optional<rebase_todo_line> parse_rebase_todo_line(std::string_view line);

    // todo 글 전체에서 커밋 줄의 SHA를 순서대로 꺼낸다.
    [[nodiscard]] std::vector<std::string> rebase_todo_commits(std::string_view todo_text);

    // git이 만든 todo(`git-rebase-todo`, SHA가 줄어 있다)가 편집기에 보인 범위와 같은가. 줄인 SHA마다 앞부분이
    // 같은 전체 SHA가 정확히 하나 있고, 개수가 같아야 한다. 편집기를 연 뒤 브랜치가 움직였으면 거짓이다.
    [[nodiscard]] bool rebase_todo_matches(std::string_view git_todo, std::span<const std::string> expected_shas);

    // `rebase-merge/done`의 마지막 커밋 줄이다. git이 지금 편집기를 연 커밋이다.
    [[nodiscard]] std::optional<rebase_todo_line> last_done_line(std::string_view done_text);

    // `rebase-merge/done` 마지막 줄로 지금 편집기가 열린 메시지가 어느 메시지 파일의 것인지 고른다. reword·squash·
    // fixup 줄이면 그 SHA이고, pick처럼 메시지를 바꾸지 않는 줄이면 값이 없다 (충돌 뒤 continue가 연 편집기).
    [[nodiscard]] std::optional<std::string> message_key_for_editor(std::string_view done_text);

    // `^[0-9a-f]{4,64}$`. 메시지 파일 이름으로 쓰기 전에 확인한다 (경로 조각이 섞이지 않게).
    [[nodiscard]] bool is_hex_object_id(std::string_view text) noexcept;
} // namespace cyella
