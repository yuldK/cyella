#pragma once

#include "application/operation_events.h"
#include "domain/operation_status.h"
#include "presentation/dialog_state.h"
#include "presentation/form_view.h"

#include "luil/ui/text_input_state.h"
#include "luil/ui/ui_interaction.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 상태에서 `form_view`를 짓는 순수 함수들이다. UI 요소도 시계도 모른다 — 같은 입력이면 같은 값이다.

    // 입력 초안을 칸으로 옮기는 데 필요한 문맥이다.
    struct form_build_context
    {
        const repository_snapshot* snapshot { nullptr };
        const form_inputs* inputs { nullptr };
        const std::optional<luil::text_composition_event>* composition { nullptr };
    };

    // 새 브랜치·태그 이름의 문제를 한글 한 줄로 말한다. 문제가 없으면 값이 없다. 이미 있는 이름도 문제다.
    [[nodiscard]] std::optional<std::u8string> branch_name_error(std::string_view name, const repository_snapshot& snapshot);
    [[nodiscard]] std::optional<std::u8string> tag_name_error(std::string_view name, const repository_snapshot& snapshot);

    // 브랜치 이름 앞뒤 공백을 뗀다. 칸의 글을 operation 값으로 바꿀 때 쓴다.
    [[nodiscard]] std::string trimmed_text(std::u8string_view text);

    [[nodiscard]] form_view build_create_branch_form(const dialog_state& dialog, const form_build_context& context);
    [[nodiscard]] form_view build_create_tag_form(const dialog_state& dialog, const form_build_context& context);
    [[nodiscard]] form_view build_push_form(const dialog_state& dialog, const form_build_context& context);
    [[nodiscard]] form_view build_set_upstream_form(const dialog_state& dialog, const form_build_context& context);
    [[nodiscard]] form_view build_add_remote_form(const dialog_state& dialog, const form_build_context& context);
    [[nodiscard]] form_view build_rename_branch_form(const dialog_state& dialog, const form_build_context& context);
    [[nodiscard]] form_view build_confirm_form(const dialog_state& dialog, const form_build_context& context);

    // `needs_input{dirty_worktree}`로 끝난 체크아웃의 정책 상자다.
    [[nodiscard]] form_view build_dirty_checkout_form(const operation_status& status, const dirty_checkout_state& state, const form_build_context& context);

    // 인증 질문(`passphrase`·`host_key`·`credentials`) 상자다. 비밀 입력은 점으로 가리고 값은 snapshot에 평문으로 있다.
    [[nodiscard]] form_view build_auth_form(const operation_status& status, const form_build_context& context);

    // 위 두 상자의 `form_inputs::identity`다. 질문이 바뀌면(ssh의 다음 시도도 새 번호) 열쇠가 바뀌어 입력이 지워진다.
    [[nodiscard]] std::string auth_identity(const operation_status& status);
    [[nodiscard]] std::string dirty_identity(const operation_status& status);
    [[nodiscard]] std::string dialog_identity(const dialog_state& dialog);

    // push 상자가 보낼 요청이다 (원격 선택·칸의 글을 반영한다).
    [[nodiscard]] push_execute_request make_push_request(const dialog_state& dialog, const form_inputs& inputs);

    // upstream 설정 상자가 보낼 요청이다 (원격 선택·칸의 글을 반영한다).
    [[nodiscard]] set_upstream_request make_set_upstream_request(const dialog_state& dialog, const form_inputs& inputs);
    // 고른 `<remote>/<branch>`를 쓸 수 없는 이유를 한 줄로 말한다. 쓸 수 있으면 값이 없다. 원격 추적 ref가 없으면 쓸 수 없다
    // (`git branch --set-upstream-to`가 거절한다 - 먼저 fetch하거나 push --set-upstream으로 원격 브랜치를 만들어야 한다).
    [[nodiscard]] std::optional<std::u8string> set_upstream_error(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);

    // 원격 추가 상자가 낼 operation 열이다. `remote add`이고, 게시를 켰으면 성공한 뒤 `push --set-upstream`이 잇는다.
    [[nodiscard]] std::vector<git_operation> make_add_remote_operations(const dialog_state& dialog, const form_inputs& inputs);
    // 원격 추가 상자에서 URL 대신 호스팅 서버에 새 저장소를 만들기로 골랐다.
    [[nodiscard]] bool add_remote_to_hosting(const dialog_state& dialog) noexcept;
    // 원격 이름·URL(호스트를 골랐으면 저장소 이름·소유자)을 쓸 수 없는 이유를 한 줄로 말한다. 칸 1이 아직 비었으면 말하지 않고 단추만 끈다.
    [[nodiscard]] std::optional<std::u8string> add_remote_error(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);

    // 조건 판정: 단추를 켤 수 있는가.
    [[nodiscard]] bool create_branch_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);
    [[nodiscard]] bool create_tag_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);
    [[nodiscard]] bool rename_branch_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);
    [[nodiscard]] bool push_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);
    [[nodiscard]] bool set_upstream_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);
    [[nodiscard]] bool add_remote_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot);
} // namespace cyella
