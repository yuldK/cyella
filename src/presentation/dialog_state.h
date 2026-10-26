#pragma once

#include "application/git_operation.h"
#include "domain/operation_status.h"
#include "presentation/menu_model.h"

#include "luil/text/text_edit.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 대화 상자와 메뉴의 화면 상태다. 껐다 켜면 잃어도 되는 것만 담는다. git 상태와 operation 상태는 workspace가 정본이다.

    // 사용자가 연 대화 상자의 종류다. 체크아웃 정책(`needs_input{dirty_worktree}`)과 인증 질문은 operation이 연다 —
    // 그쪽 상태는 `form_inputs`에만 입력 초안이 있고 여기에는 오지 않는다.
    enum class dialog_kind
    {
        none,
        create_branch,
        create_tag,
        push,
        rename_branch,
        confirm,
        // worktree 추가, Git LFS 추적 패턴.
        add_worktree,
        lfs_track,
        // upstream 설정: 원격과 원격 브랜치를 골라 `branch --set-upstream-to`를 낸다. push 없이 잇기만 한다.
        set_upstream,
        // GitHub·GitLab에 PR·MR을 만든다 (`form_hosting.h`가 `dialog_state` 필드의 쓰임을 정한다).
        create_pull_request,
        // 원격 추가와 게시: `remote add`를 내고, 켜 두면 이어서 현재 브랜치를 `push --set-upstream`한다.
        add_remote,
    };

    // 확인 뒤에 낼 operation 열이다. 둘째부터는 앞이 **성공해야** 이어서 낸다 (원격 태그를 먼저 지우고 로컬을 지운다,
    // pull 뒤 push). 확인 상자의 체크 칸을 켜면 `alternate`를 대신 낸다.
    struct confirm_plan
    {
        std::vector<git_operation> primary {};
        std::vector<git_operation> alternate {};
    };

    struct dialog_state
    {
        dialog_kind kind { dialog_kind::none };
        // 열 때마다 오른다. 입력 초안이 어느 대화 상자의 것인지 가르는 열쇠다.
        std::uint64_t serial { 0 };
        // 대상이다. create_branch·create_tag는 커밋 SHA(비면 HEAD), push·rename_branch는 브랜치 이름이다.
        std::string subject {};
        // 대상을 사람에게 보이는 글이다 ("HEAD", "abc1234 커밋 제목").
        std::u8string subject_label {};
        // 대화 상자마다 다르게 쓰는 칸이다.
        //  - create_branch: a=만든 뒤 체크아웃. create_tag: a=annotated.
        //  - push: a=upstream 설정, b=태그도 push, c=force-with-lease.
        //  - set_upstream: 플래그가 없다. subject=로컬 브랜치, choices=원격, 칸 0=원격 브랜치 이름.
        //  - add_remote: subject=게시할 로컬 브랜치(비면 추가만 한다), a=추가한 뒤 push, 칸 0=원격 이름, 칸 1=URL.
        bool flag_a { false };
        bool flag_b { false };
        bool flag_c { false };
        // 고른 선택지다 (push의 원격, create_tag의 종류).
        std::size_t choice { 0 };
        // push의 원격 이름들이다.
        std::vector<std::string> choices {};
        // confirm
        std::u8string title {};
        std::vector<std::u8string> lines {};
        std::u8string confirm_label {};
        bool danger { false };
        // 비어 있으면 체크 칸이 없다.
        std::u8string check_label {};
        bool check_warning { false };
        // 체크 칸이 "다시 보지 않기"다. 켠 채 확인하면 detached HEAD 경고를 더는 보이지 않는다 (`plan.alternate`를 쓰지 않는다).
        bool remember_detached { false };
        confirm_plan plan {};
    };

    // 열려 있는 입력 대화 상자의 텍스트 칸이다. 대화 상자마다 최대 셋이고, 어느 대화 상자의 것인지 `identity`가 정한다.
    // 열쇠가 바뀌면(다른 대화 상자, 인증의 다음 질문) 비밀이 남지 않게 지우고 비운다.
    struct form_inputs
    {
        std::string identity {};
        std::array<luil::text::text_edit_state, 3> fields {};
        // 비밀 칸을 평문으로 보인다.
        bool reveal { false };
        // 체크 칸의 값이다 (인증의 "이 질문에만", 체크아웃의 "추적 안 된 파일도").
        std::array<bool, 4> checks {};
        std::size_t choice { 0 };
        float scroll { 0.0f };
    };

    // 체크아웃 정책 대화 상자의 선택이다. operation이 열고 정책으로 다시 낸 뒤 닫는다.
    struct dirty_checkout_state
    {
        operation_id operation { 0 };
        // 변경을 버리고 이동을 골랐고 한 번 더 확인하는 중이다.
        bool confirming_discard { false };
    };

    // 열린 메뉴다. 항목은 snapshot마다 `target`과 저장소 상태로 다시 정해지고, 고른 키로 controller가 같은 규칙을 다시 불러
    // 실행한다. 좌표는 창 좌표의 물리 픽셀이다.
    struct menu_state
    {
        menu_target target {};
        float x { 0.0f };
        float y { 0.0f };
        // 끌어 놓기 메뉴가 모르는 관계를 이미 물어 보았다.
        bool asked_facts { false };
    };

    // 화면에 이미 한 번 보인 경고 같은 일회성 표지다.
    struct dialog_memory
    {
        // detached HEAD 경고를 다시 보이지 않는다 (앱을 켜 두는 동안만).
        bool detached_warning_suppressed { false };
    };
} // namespace cyella
