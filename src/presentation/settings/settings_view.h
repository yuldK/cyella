#pragma once

#include "application/file_probe.h"
#include "domain/app_settings.h"
#include "domain/external_diff_tool.h"
#include "domain/git_tool.h"
#include "domain/operation_status.h"
#include "domain/ssh_connection.h"
#include "presentation/hosting_view.h"
#include "presentation/settings/profiles_page.h"
#include "presentation/settings/settings_state.h"
#include "presentation/settings/shortcuts_page.h"
#include "presentation/settings/signing_page_view.h"
#include "presentation/ui_state.h"

#include "luil/ui/text_input_state.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 환경 설정 화면의 snapshot이다. `build_settings_view`가 상태·설정·작업 결과에서 짓는 순수 함수의 결과이고,
    // `ui::build_settings_dialog`가 이 값만 읽어 luil tree를 짓는다.

    // ------------------------------------------------------------------ 일반

    struct git_settings_view
    {
        // 입력 초안이다.
        luil::text_input_view path {};
        // 이번 실행이 쓰는 git이다 ("자동 탐색", "직접 지정"과 경로·버전).
        std::u8string origin {};
        std::u8string executable {};
        std::u8string version {};
        // 이번 실행의 git을 쓸 수 없을 때의 안내다. 비어 있으면 쓸 수 있다.
        std::u8string problem {};
        // 입력 초안의 검사 결과다. 비어 있으면 저장할 수 있다.
        std::u8string validation {};
        // 설정에 직접 지정한 경로가 있다.
        bool configured { false };
        // 초안이 저장된 값과 다르다.
        bool dirty { false };
        bool can_apply { false };
    };

    // ------------------------------------------------------------------ diff 도구

    struct tool_row_view
    {
        std::size_t index { 0 };
        std::u8string extensions {};
        std::u8string executable {};
        std::u8string arguments {};
        bool file_app { false };
    };

    struct tool_edit_view
    {
        bool is_new { true };
        bool file_app { false };
        luil::text_input_view extensions {};
        luil::text_input_view executable {};
        luil::text_input_view arguments {};
        // 샘플 경로로 풀어 본 실행 한 줄이다 (`"exe" arg1 arg2`). 템플릿이 올바르지 않으면 비어 있다.
        std::u8string preview {};
        std::vector<std::u8string> preview_arguments {};
        // 저장을 막는 문제다. 비어 있으면 저장할 수 있다.
        std::u8string error {};
        // 저장은 되지만 알려 둘 것이다 (절대 경로의 실행 파일이 없다).
        std::u8string warning {};
        bool can_save { false };
    };

    // ------------------------------------------------------------------ SSH

    enum class tone
    {
        neutral,
        good,
        warning,
        bad,
    };

    // ssh 도구로 낸 operation(키 만들기·연결 시험)의 상태다.
    struct task_view
    {
        // 낸 적이 없으면 `none`이다.
        enum class phase
        {
            none,
            running,
            waiting_for_input,
            finished,
        };

        phase state { phase::none };
        tone result_tone { tone::neutral };
        std::u8string text {};
        std::u8string detail {};
        operation_id id { 0 };
        bool cancellable { false };
    };

    struct ssh_key_row_view
    {
        std::u8string name {};
        std::u8string detail {};
        bool can_copy { false };
    };

    struct ssh_view
    {
        bool loading { false };
        // 아직 읽지 않았다 (처음 열린 직후).
        bool loaded { false };
        // 쓰는 ssh와 그 출처다 ("Git for Windows", "Windows OpenSSH"). 못 찾았으면 비어 있다.
        std::u8string tools_origin {};
        bool tools_found { false };
        std::u8string service_status {};
        tone service_tone { tone::neutral };
        std::u8string service_help {};
        // agent에 등록된 키 줄이다. 없으면 `agent_message`가 이유다.
        std::vector<std::u8string> identities {};
        std::u8string agent_message {};
        std::vector<ssh_key_row_view> keys {};
        std::u8string ssh_directory {};

        luil::text_input_view key_name {};
        luil::text_input_view key_comment {};
        // `ssh-keygen -t ed25519 -C <주석> -f <경로>` 미리보기다. passphrase는 명령줄에 없고 askpass로 묻는다.
        std::u8string keygen_command {};
        std::u8string keygen_error {};
        bool can_create { false };
        task_view keygen {};

        luil::text_input_view test_host {};
        std::u8string test_command {};
        std::u8string test_error {};
        bool can_test { false };
        task_view test {};
    };

    // ------------------------------------------------------------------ 호스팅

    struct hosting_settings_page_view
    {
        // 내장 호스트(github.com·gitlab.com)에 설정의 표를 더한 줄들이다. 로그인 상태는 이번 실행의 메모리 값이다.
        std::vector<hosting_host_row_view> hosts {};
        luil::text_input_view host {};
        luil::text_input_view api_base {};
        hosting_provider new_provider { hosting_provider::github };
        std::u8string error {};
        bool can_add { false };
    };

    struct settings_view
    {
        settings_page page { settings_page::general };
        float scroll { 0.0f };
        // 파일을 읽지 못해 이번 실행 동안 저장하지 않는다 (`app_settings_service::is_read_only`).
        bool read_only { false };
        // 마지막 저장이 실패해 디스크가 메모리보다 뒤처졌다.
        bool unsaved { false };
        std::u8string message {};

        theme_preference theme { theme_preference::system };
        // 저장된 키 컬러 id다 (`appearance.accent`). 비면 luil의 기본 키 컬러다.
        std::u8string accent {};
        // 저장된 탭 줄 모양과 상태 표시 자리다. 견본이 이 값으로 그린다.
        tab_shape tab_shape_choice { tab_shape::connected };
        tab_marker_place tab_marker_choice { tab_marker_place::badge };
        language_preference language { language_preference::system };
        luil::text_input_view ui_font {};
        luil::text_input_view code_font {};
        std::optional<font_list_state> font_list {};
        float font_scroll { 0.0f };
        bool fonts_dirty { false };
        // 커밋 작성자 사진을 Gravatar에서 받는다 (`appearance.gravatar`).
        bool gravatar { false };
        diff_settings diff {};
        git_settings_view git {};
        std::vector<tool_row_view> tools {};
        std::vector<tool_row_view> file_apps {};
        std::optional<tool_edit_view> edit {};
        ssh_view ssh {};
        // "서명" 쪽이다.
        signing_page_view signing {};
        // 호스팅 화면이다.
        hosting_settings_page_view hosting {};
        // 단축키·사용자 프로필 쪽은 그 쪽이 열려 있을 때만 값이 있다.
        std::optional<shortcuts_page_view> shortcuts {};
        std::optional<profiles_page_view> profiles {};
    };

    // ------------------------------------------------------------------ 보조 함수 (test가 직접 부른다)

    // `.png, .PSD tar.gz` 같은 입력을 확장자 목록으로 바꾼다. 쉼표·세미콜론·공백이 구분자이고, 점이 없어도 받아들인다.
    // 소문자 `.xxx` 꼴로 정규화하고 중복을 버린다.
    [[nodiscard]] std::vector<std::u8string> parse_extension_list(std::u8string_view text);
    [[nodiscard]] std::u8string format_extension_list(const std::vector<std::u8string>& extensions);

    // 미리보기가 쓰는 샘플 값이다 (`{left}` `{right}` `{path}`). 파일 확장자는 도구의 첫 확장자를 따른다(비면 `.png`).
    // `{base}`는 병합 도구용이라 비워 둔다. 그 토큰만 든 인자는 빠진다.
    [[nodiscard]] external_diff_values sample_external_diff_values(std::u8string_view extension);
    // 실행 파일과 풀린 인자를 한 줄로 보인다. 공백이 든 항목은 큰따옴표로 감싼다 (복사해서 쓰는 명령이 아니라 읽는 표시다).
    [[nodiscard]] std::u8string format_command_preview(std::u8string_view executable, const std::vector<std::u8string>& arguments);

    // 도구 한 줄의 입력을 검사하고 샘플로 풀어 본다. `probe`가 있으면 절대 경로의 실행 파일이 있는지도 본다.
    struct tool_draft_check
    {
        std::vector<std::u8string> extensions {};
        std::u8string error {};
        std::u8string warning {};
        std::vector<std::u8string> preview_arguments {};
        std::u8string preview {};
        bool ok { false };
    };
    [[nodiscard]] tool_draft_check check_tool_draft(std::u8string_view extensions, std::u8string_view executable, std::u8string_view arguments, const file_probe* probe);
    [[nodiscard]] tool_draft_check check_file_app_draft(std::u8string_view name, std::u8string_view executable, std::u8string_view arguments, const file_probe* probe);

    // 입력으로 만든 도구다. 검사를 통과한 입력에만 쓴다.
    [[nodiscard]] external_diff_tool make_external_tool(std::u8string_view extensions, std::u8string_view executable, std::u8string_view arguments);

    // git 경로 입력의 검사 결과다. 비어 있으면(자동 탐색) 문제가 없다.
    [[nodiscard]] std::u8string check_git_path(std::u8string_view text, const file_probe* probe);

    // `[user@]host[:port]`다. 호스트나 계정에 공백·따옴표·앞의 `-`가 있으면 값이 없다.
    struct ssh_target
    {
        std::u8string user_name { u8"git" };
        std::u8string host {};
        std::uint16_t port { 0 };

        [[nodiscard]] bool operator==(const ssh_target&) const noexcept = default;
    };
    [[nodiscard]] std::optional<ssh_target> parse_ssh_target(std::u8string_view text);

    // 키 파일 이름(폴더 없이)의 검사 결과다. 비어 있으면 쓸 수 있다.
    [[nodiscard]] std::u8string check_key_name(std::u8string_view name);

    // 호스팅 표에 더할 줄의 검사 결과다. 비어 있으면 더할 수 있다. 호스트 형식, API 주소 형식, 이미 있는 호스트를 가린다.
    [[nodiscard]] std::u8string check_hosting_draft(std::u8string_view host, hosting_provider provider, std::u8string_view api_base, const std::vector<hosting_host_setting>& existing);

    // ------------------------------------------------------------------ snapshot

    // 가장 최근에 낸 ssh 작업의 상태다 (workspace의 operation 표에서 읽은 값). 낸 적이 없으면 `id`가 0이다.
    struct ssh_task_input
    {
        operation_id id { 0 };
        std::optional<operation_state> state {};
        std::u8string summary {};
        std::u8string failure_summary {};
        std::u8string failure_detail {};
        // 상태가 `needs_input`이고 인증 질문이다 (끝난 결정이 아니라 도우미가 기다리는 중).
        bool waiting_for_prompt { false };
        bool cancellable { false };
        std::optional<ssh_connection_classification> classification {};
    };

    struct settings_view_inputs
    {
        const settings_state* state { nullptr };
        const app_settings* settings { nullptr };
        // 이번 실행이 찾은 git이다.
        const git_tool_info* git_tool { nullptr };
        const file_probe* probe { nullptr };
        bool read_only { false };
        bool unsaved { false };
        // 키를 만들 폴더다 (`%USERPROFILE%\.ssh`). 모르면 비어 있다.
        std::u8string ssh_directory {};
        // ssh·ssh-keygen을 찾았는가와 그 출처다.
        bool ssh_tools_found { false };
        std::u8string ssh_tools_origin {};
        ssh_task_input keygen {};
        ssh_task_input test {};
        std::optional<luil::text_composition_event> composition {};
        // 호스팅 화면의 호스트 줄이다 (`hosting_controller::host_rows`).
        std::vector<hosting_host_row_view> hosting_hosts {};
    };

    // 호스팅 화면의 값을 채운다 (`hosting_settings.cpp`).
    void fill_hosting_page(hosting_settings_page_view& view, const settings_view_inputs& inputs);

    [[nodiscard]] settings_view build_settings_view(const settings_view_inputs& inputs);
} // namespace cyella
