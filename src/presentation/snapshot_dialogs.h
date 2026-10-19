#pragma once

#include "application/repository_session.h"
#include "domain/operation_status.h"
#include "presentation/drop_hints.h"
#include "presentation/form_view.h"
#include "presentation/graph_index.h"
#include "presentation/menu_model.h"
#include "presentation/ui_state.h"
#include "presentation/view_snapshot.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace cyella {
    // 대화 상자·메뉴·충돌 화면·배너 조치를 snapshot으로 옮기는 순수 함수들이다. `build_view_snapshot`과 controller가 같은
    // 규칙을 쓰도록 한곳에 둔다 — 어느 상자가 지금 떠 있는지는 화면과 입력 처리가 같은 답을 해야 한다.

    // 지금 떠 있는 모달 입력 상자가 무엇에서 왔는가.
    enum class form_source_kind
    {
        none,
        // 인증 질문(`needs_input{passphrase|host_key|credentials}`)이다. 가장 위에 선다.
        auth,
        // 사용자가 연 대화 상자다 (`ui_state::dialog`).
        dialog,
        // 체크아웃이 작업 트리 변경에 막혀 끝났다 (`needs_input{dirty_worktree}`).
        dirty_checkout,
    };

    struct form_source
    {
        form_source_kind kind { form_source_kind::none };
        // auth·dirty_checkout의 operation이다.
        const operation_status* status { nullptr };
    };

    // 사용자가 기다리는 작업인가 (자동 갱신과 화면을 채우는 읽기는 아니다).
    [[nodiscard]] bool is_user_operation_status(const operation_status& status) noexcept;

    // `repository_operations`는 선택된 저장소의 operation, `setup_operations`는 clone·init처럼 저장소 id가 없는 것이다.
    // 인증은 둘 다 본다. 나머지는 저장소 쪽만 본다. `tab`이 null이면 시작 탭이다.
    [[nodiscard]] form_source resolve_form_source(std::span<const operation_status* const> repository_operations, std::span<const operation_status* const> setup_operations,
        const ui_state& ui, const tab_ui_state* tab);

    // `source`의 모달 입력 상자 값이다. 없으면 빈 값이다.
    [[nodiscard]] std::optional<form_view> build_form_view(const form_source& source, const ui_state& ui, const tab_ui_state* tab, const repository_snapshot* snapshot);

    // `form_inputs::identity`로 쓰는 열쇠다. 상자가 없으면 빈 문자열이다.
    [[nodiscard]] std::string form_identity(const form_source& source, const ui_state& ui);

    // 메뉴 항목을 정하는 저장소 상태다. 끌어 놓기 관계는 읽어 둔 이력(`index`)에서 먼저 얻고, 모르면 세션이 받아 둔
    // `is_ancestor`·`count_commits` 답을 쓴다.
    [[nodiscard]] menu_context build_menu_context(const repository_session& session, const menu_target& target, const graph_index* index);

    [[nodiscard]] std::optional<menu_view> build_menu_view(const ui_state& ui, const repository_session* session, const graph_index* index, float scale);

    // 충돌 해결 화면이다. 멈춘 작업이 없으면 빈 값이다.
    [[nodiscard]] std::optional<conflict_view> build_conflict_view(const repository_session& session, std::span<const operation_status* const> operations);

    [[nodiscard]] std::vector<banner_action_view> banner_actions_for(const operation_status& status);

    [[nodiscard]] std::shared_ptr<const drop_hint_data> build_drop_hints(const repository_snapshot& snapshot, std::shared_ptr<const graph_index> index);
} // namespace cyella
