set(cyella_resource_directory "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/resources")
set(cyella_manifest_file "${cyella_resource_directory}/cyella.manifest")
set(cyella_resource_ids_file "${cyella_resource_directory}/resource_ids.h")
set(cyella_resource_template "${cyella_resource_directory}/cyella.rc.in")
# 앱 아이콘과 캡션 아이콘은 assets/cyella.svg에서 scripts/generate_app_icon.py로 만든다.
set(cyella_app_icon "${PROJECT_SOURCE_DIR}/assets/cyella.ico")
set(cyella_caption_icon "${PROJECT_SOURCE_DIR}/assets/cyella-caption.png")

file(TO_CMAKE_PATH "${cyella_manifest_file}" CYELLA_MANIFEST_PATH)
file(TO_CMAKE_PATH "${cyella_resource_ids_file}" CYELLA_RESOURCE_IDS_PATH)
file(TO_CMAKE_PATH "${cyella_app_icon}" CYELLA_APP_ICON_PATH)
file(TO_CMAKE_PATH "${cyella_caption_icon}" CYELLA_CAPTION_ICON_PATH)
configure_file(
    "${cyella_resource_template}"
    "${CYELLA_GENERATED_RESOURCE}"
    @ONLY)
set_source_files_properties(
    "${CYELLA_GENERATED_RESOURCE}"
    PROPERTIES
        GENERATED TRUE
        OBJECT_DEPENDS "${cyella_manifest_file};${cyella_resource_ids_file};${cyella_app_icon};${cyella_caption_icon}")

# 앱 코드 전체가 하나의 static library다. 계층(domain → application →
# infrastructure → presentation → platform)은 디렉터리 구조와 코드 리뷰로 지키며,
# target 경계로는 나누지 않는다.
#
# UI 기반 계층(element·tree·상호작용·창·렌더러·TSF·클립보드·글꼴·messaging)은
# luil이 맡는다. cyella는 앱 element와 driver·policy·delegate만 갖는다.
#
# 파일은 IDE 필터 단위로 적고, 그렇게 모인 것이 곧 라이브러리의 source 목록이다
# (cyella_add_source_filter, cmake/macro.cmake). 필터의 첫 단은 계층, 둘째 단은
# **하는 일**이다.
set(cyella_lib_files)

# domain: 순수 규칙과 값 타입이다. 다른 계층에 의존하지 않는다.
cyella_add_source_filter(cyella_lib_files "domain/graph"
    domain/commit_graph_layout.cpp
    domain/commit_graph_layout.h)

cyella_add_source_filter(cyella_lib_files "domain/diagnostics"
    domain/diagnostic.cpp
    domain/diagnostic.h)

cyella_add_source_filter(cyella_lib_files "domain/process"
    domain/path_syntax.cpp
    domain/path_syntax.h
    domain/process_execution.cpp
    domain/process_execution.h)

cyella_add_source_filter(cyella_lib_files "domain/git"
    domain/git_tool.cpp
    domain/git_tool.h)

cyella_add_source_filter(cyella_lib_files "domain/model"
    domain/commit_info.h
    domain/diff_file.h
    domain/file_change.h
    domain/ref.cpp
    domain/ref.h
    domain/repository_summary.cpp
    domain/repository_summary.h
    domain/stash_entry.h)

cyella_add_source_filter(cyella_lib_files "domain/diff"
    domain/patch_builder.cpp
    domain/patch_builder.h)

# operation 하나의 상태(contract의 operation_status)와 실패·입력 종류다.
cyella_add_source_filter(cyella_lib_files "domain/operations"
    domain/operation_status.cpp
    domain/operation_status.h)

cyella_add_source_filter(cyella_lib_files "domain/rules"
    domain/ref_name.cpp
    domain/ref_name.h)

# 인터랙티브 rebase의 할 일 목록 규칙이다 (검사, git todo 형식, 자동 squash 정렬, 메시지 짝). 편집기 도우미도
# 이 파일을 직접 컴파일한다.
cyella_add_source_filter(cyella_lib_files "domain/rebase"
    domain/interactive_rebase.cpp
    domain/interactive_rebase.h)

# infrastructure: application 계약의 구현이다. git 출력 해석기와 명령줄 조립기는
# 프로세스를 실행하지 않는 순수 함수다 (실행은 process runner가 맡는다).
cyella_add_source_filter(cyella_lib_files "infrastructure/git_parsers"
    infrastructure/git_diff_parser.cpp
    infrastructure/git_diff_parser.h
    infrastructure/git_error_classifier.cpp
    infrastructure/git_error_classifier.h
    infrastructure/git_log_parser.cpp
    infrastructure/git_log_parser.h
    infrastructure/git_query_parser.cpp
    infrastructure/git_query_parser.h
    infrastructure/git_ref_parser.cpp
    infrastructure/git_ref_parser.h
    infrastructure/git_stash_parser.cpp
    infrastructure/git_stash_parser.h
    infrastructure/git_status_parser.cpp
    infrastructure/git_status_parser.h
    infrastructure/git_text.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/git_commands"
    infrastructure/git_command_builder.cpp
    infrastructure/git_command_builder.h)

cyella_add_source_filter(cyella_lib_files "domain/settings"
    domain/app_settings.cpp
    domain/app_settings.h
    domain/external_diff_tool.cpp
    domain/external_diff_tool.h
    domain/repository_path.cpp
    domain/repository_path.h)

cyella_add_source_filter(cyella_lib_files "domain/authentication"
    domain/credential_prompt.cpp
    domain/credential_prompt.h
    domain/ssh_connection.cpp
    domain/ssh_connection.h)

# GitHub·GitLab 연동: 원격 URL 해석과 PR·CI 값 타입이다.
cyella_add_source_filter(cyella_lib_files "domain/hosting"
    domain/hosting_models.cpp
    domain/hosting_models.h
    domain/hosting_remote.cpp
    domain/hosting_remote.h)

# application: 상태 소유와 계약(interface)이다. OS API를 직접 부르지 않고 luil
# UI 헤더를 include하지 않는다.
cyella_add_source_filter(cyella_lib_files "application/logic"
    application/app_messages.h)

cyella_add_source_filter(cyella_lib_files "application/process"
    application/cancellation_registry.cpp
    application/cancellation_registry.h
    application/file_probe.h
    application/process_cancellation.cpp
    application/process_cancellation.h
    application/process_request.cpp
    application/process_request.h
    application/process_runner.h
    application/text_transcoder.h)

cyella_add_source_filter(cyella_lib_files "application/authentication"
    application/askpass_service.h)

# diff 뷰어: 웹뷰 메시지 프로토콜(순수 함수), 컨트롤러(입출력 없이 효과를 값으로 돌려준다), 두 쪽 해석과
# 외부 도구 열기다.
cyella_add_source_filter(cyella_lib_files "application/diff_view"
    application/diff_sides.cpp
    application/diff_sides.h
    application/diff_tool_launcher.h
    application/diff_view_coordinator.cpp
    application/diff_view_coordinator.h
    application/diff_view_controller.cpp
    application/diff_view_controller.h
    application/diff_view_protocol.cpp
    application/diff_view_protocol.h
    application/external_diff_opener.cpp
    application/external_diff_opener.h)

cyella_add_source_filter(cyella_lib_files "application/scheduling"
    application/operation_executor.h
    application/task_scheduler.cpp
    application/task_scheduler.h)

cyella_add_source_filter(cyella_lib_files "application/settings"
    application/app_settings_service.cpp
    application/app_settings_service.h
    application/app_settings_store.h
    application/settings_file_system.h)

cyella_add_source_filter(cyella_lib_files "application/tabs"
    application/repository_tabs.cpp
    application/repository_tabs.h)

cyella_add_source_filter(cyella_lib_files "application/watching"
    application/repository_change_classifier.cpp
    application/repository_change_classifier.h
    application/repository_watcher.h
    application/watch_messages.h
    application/watch_refresh_policy.cpp
    application/watch_refresh_policy.h)

# 저장소 세션: operation 요청 값, worker 사건, 탭별 상태와 operation 표다. git을 직접
# 실행하지 않고 실행기 계약(git_operation_executor)을 부른다.
cyella_add_source_filter(cyella_lib_files "application/repository"
    application/git_operation.cpp
    application/git_operation.h
    application/git_operation_executor.h
    application/git_workspace.cpp
    application/git_workspace.h
    application/operation_events.h
    application/repository_session.cpp
    application/repository_session.h
    application/workspace_files.h)

# GitHub·GitLab 연동의 호출 조립이다. 자격 증명 도우미(git credential)와 luil의 HTTP client 위에 서고,
# 토큰은 메모리에만 둔다.
cyella_add_source_filter(cyella_lib_files "application/hosting"
    application/hosting_credentials.cpp
    application/hosting_credentials.h
    application/hosting_messages.h
    application/hosting_transport.cpp
    application/hosting_transport.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/hosting"
    infrastructure/git_credential_source.cpp
    infrastructure/git_credential_source.h
    infrastructure/hosting_api.cpp
    infrastructure/hosting_api.h
    infrastructure/hosting_service.cpp
    infrastructure/hosting_service.h
    infrastructure/luil_hosting_transport.cpp
    infrastructure/luil_hosting_transport.h)

# infrastructure: application 계약의 구현이다 (Git 호출 등). Win32 API를 직접 부르지 않는다.
cyella_add_source_filter(cyella_lib_files "infrastructure/process"
    infrastructure/command_line_builder.cpp
    infrastructure/command_line_builder.h
    infrastructure/process_output_pipeline.cpp
    infrastructure/process_output_pipeline.h
    infrastructure/secret_masking.cpp
    infrastructure/secret_masking.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/git"
    infrastructure/git_command_runner.cpp
    infrastructure/git_command_runner.h
    infrastructure/git_execution_policy.cpp
    infrastructure/git_execution_policy.h
    infrastructure/git_progress_parser.cpp
    infrastructure/git_progress_parser.h
    infrastructure/git_tool_discovery.cpp
    infrastructure/git_tool_discovery.h
    infrastructure/git_version.cpp
    infrastructure/git_version.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/settings"
    infrastructure/json_app_settings_store.cpp
    infrastructure/json_app_settings_store.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/authentication"
    infrastructure/askpass_environment.cpp
    infrastructure/askpass_environment.h
    infrastructure/ssh_info.cpp
    infrastructure/ssh_info.h
    infrastructure/ssh_tools.cpp
    infrastructure/ssh_tools.h)

# diff 뷰어의 git 입출력(blob을 임시 파일로 꺼내기, diff.tool 읽기)과 임시 폴더다.
cyella_add_source_filter(cyella_lib_files "infrastructure/diff_view"
    infrastructure/diff_temp_directory.cpp
    infrastructure/diff_temp_directory.h
    infrastructure/git_diff_gateway.cpp
    infrastructure/git_diff_gateway.h)

# operation을 git 명령으로 바꿔 실행하는 실행기다. 명령 조립기·해석기·process runner를 잇는다.
cyella_add_source_filter(cyella_lib_files "infrastructure/git_operations"
    infrastructure/git_interactive_rebase.cpp
    infrastructure/git_interactive_rebase.h
    infrastructure/git_operation_context.cpp
    infrastructure/git_operation_context.h
    infrastructure/git_operation_runner.cpp
    infrastructure/git_operation_runner.h
    infrastructure/git_read_operations.cpp
    infrastructure/git_write_operations.cpp
    infrastructure/ssh_operations.cpp)

# submodule·worktree·Git LFS: 값 타입, git 출력 해석기, 명령 조립기, operation 실행이다.
cyella_add_source_filter(cyella_lib_files "domain/extensions"
    domain/lfs.cpp
    domain/lfs.h
    domain/submodule.cpp
    domain/submodule.h
    domain/worktree.h)

cyella_add_source_filter(cyella_lib_files "presentation/extensions"
    presentation/diff_special.cpp
    presentation/diff_special.h
    presentation/extension_intents.h
    presentation/form_extension.cpp
    presentation/form_extension.h
    presentation/shell_controller_extension.cpp
    presentation/menu_extension.cpp
    presentation/menu_extension.h
    presentation/sidebar_extension.cpp
    presentation/sidebar_extension.h)

# GitHub·GitLab 화면: 컨트롤러, 사이드바 구역, PR 만들기 대화 상자, 상세 판이다. luil 없이 test한다.
cyella_add_source_filter(cyella_lib_files "presentation/hosting"
    presentation/form_hosting.cpp
    presentation/form_hosting.h
    presentation/hosting_controller.cpp
    presentation/hosting_controller.h
    presentation/hosting_intents.h
    presentation/hosting_sidebar.cpp
    presentation/hosting_sidebar.h
    presentation/hosting_view.h
    presentation/shell_controller_hosting.cpp)

cyella_add_source_filter(cyella_lib_files "presentation/ui/hosting"
    presentation/ui/hosting_ui.cpp
    presentation/ui/hosting_ui.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/git_extensions"
    infrastructure/git_extension_arguments.h
    infrastructure/git_extension_operations.cpp
    infrastructure/git_extension_operations.h
    infrastructure/git_lfs_commands.cpp
    infrastructure/git_lfs_commands.h
    infrastructure/git_lfs_operations.cpp
    infrastructure/git_lfs_parser.cpp
    infrastructure/git_lfs_parser.h
    infrastructure/git_submodule_commands.cpp
    infrastructure/git_submodule_commands.h
    infrastructure/git_submodule_operations.cpp
    infrastructure/git_submodule_parser.cpp
    infrastructure/git_submodule_parser.h
    infrastructure/git_worktree_commands.cpp
    infrastructure/git_worktree_commands.h
    infrastructure/git_worktree_operations.cpp
    infrastructure/git_worktree_parser.cpp
    infrastructure/git_worktree_parser.h)

# 커밋 서명: 서명 상태·설정·gpg 키 값 타입, git·gpg 출력 해석기, 명령 조립기, 검증 operation이다.
cyella_add_source_filter(cyella_lib_files "domain/signing"
    domain/commit_signature.cpp
    domain/commit_signature.h
    domain/gpg_key.h
    domain/signing_config.cpp
    domain/signing_config.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/signing"
    infrastructure/git_signing_commands.cpp
    infrastructure/git_signing_commands.h
    infrastructure/git_signing_failure.cpp
    infrastructure/git_signing_failure.h
    infrastructure/git_signing_operations.cpp
    infrastructure/git_signing_operations.h
    infrastructure/git_signing_parser.cpp
    infrastructure/git_signing_parser.h
    infrastructure/signing_info.cpp
    infrastructure/signing_info.h)

# presentation: view snapshot을 luil element tree로 짓는 앱 element 계층이다.
cyella_add_source_filter(cyella_lib_files "presentation/start_page"
    presentation/start_page_view.cpp
    presentation/start_page_view.h)

# 화면 상태와 snapshot이다. snapshot builder와 배치·그래프 계산은 순수 함수라 UI 없이 test한다.
cyella_add_source_filter(cyella_lib_files "presentation/shell"
    presentation/build_view_snapshot.cpp
    presentation/build_view_snapshot.h
    presentation/frame_feedback.h
    presentation/graph_geometry.cpp
    presentation/diff_view_intents.h
    presentation/graph_geometry.h
    presentation/shell_controller.cpp
    presentation/shell_controller.h
    presentation/shell_layout.cpp
    presentation/shell_layout.h
    presentation/ui_intents.h
    presentation/ui_state.h
    presentation/view_snapshot.h)

# 환경 설정 화면: 상태·메시지·snapshot builder와 그 controller다. luil 없이 test한다.
cyella_add_source_filter(cyella_lib_files "presentation/settings"
    presentation/settings/settings_controller.cpp
    presentation/settings/settings_controller.h
    presentation/settings/settings_intents.h
    presentation/settings/settings_state.h
    presentation/settings/hosting_settings.cpp
    presentation/settings/settings_view.cpp
    presentation/settings/settings_view.h)
# 대화 상자·메뉴·끌어 놓기·충돌 해결의 상태와 규칙이다. 모두 UI 없이 test하는 순수 함수다.
cyella_add_source_filter(cyella_lib_files "presentation/dialogs"
    presentation/dialog_intents.h
    presentation/dialog_state.h
    presentation/drop_hints.cpp
    presentation/drop_hints.h
    presentation/form_builders.cpp
    presentation/form_builders.h
    presentation/form_view.h
    presentation/graph_index.cpp
    presentation/graph_index.h
    presentation/korean_josa.cpp
    presentation/korean_josa.h
    presentation/menu_model.cpp
    presentation/menu_model.h
    presentation/selector_intents.h
    presentation/selector_model.cpp
    presentation/selector_model.h
    presentation/shell_controller_dialogs.cpp
    presentation/shell_controller_selectors.cpp
    presentation/snapshot_dialogs.cpp
    presentation/snapshot_dialogs.h)

# 커밋 서명의 화면 쪽이다: 서명 배지·표지 값, 커밋 상자의 서명 토글, 보이는 커밋의 서명 검증 요청.
cyella_add_source_filter(cyella_lib_files "presentation/signing"
    presentation/settings/signing_intents.h
    presentation/settings/signing_page_controller.cpp
    presentation/settings/signing_page_controller.h
    presentation/settings/signing_page_state.h
    presentation/settings/signing_page_view.cpp
    presentation/settings/signing_page_view.h
    presentation/shell_controller_signing.cpp
    presentation/signing_intents.h
    presentation/signing_view.cpp
    presentation/signing_view.h)

# 인터랙티브 rebase 편집기의 상태·메시지·규칙·snapshot과 그 controller다. luil 없이 test한다.
cyella_add_source_filter(cyella_lib_files "presentation/rebase"
    presentation/rebase/rebase_editor_controller.cpp
    presentation/rebase/rebase_editor_controller.h
    presentation/rebase/rebase_editor_intents.h
    presentation/rebase/rebase_editor_model.cpp
    presentation/rebase/rebase_editor_model.h
    presentation/rebase/rebase_editor_state.h
    presentation/rebase/rebase_editor_view.cpp
    presentation/rebase/rebase_editor_view.h
    presentation/rebase/shell_controller_rebase.cpp)

# 인터랙티브 rebase 편집기의 luil element다 (대화 상자, 여러 줄 메시지 칸).
cyella_add_source_filter(cyella_lib_files "presentation/ui/rebase"
    presentation/ui/rebase/multiline_text_element.cpp
    presentation/ui/rebase/multiline_text_element.h
    presentation/ui/rebase/rebase_editor_ui.cpp
    presentation/ui/rebase/rebase_editor_ui.h)

# 대화 상자·메뉴·라벨 칩·충돌 해결 화면의 luil element다.
cyella_add_source_filter(cyella_lib_files "presentation/ui/dialogs"
    presentation/ui/conflict_ui.cpp
    presentation/ui/form_ui.cpp
    presentation/ui/overlay_ui.cpp
    presentation/ui/selector_ui.cpp
    presentation/ui/ref_chip_element.cpp
    presentation/ui/ref_chip_element.h)

cyella_add_source_filter(cyella_lib_files "presentation/ui/foundation"
    presentation/ui/cyella_caption.cpp
    presentation/ui/cyella_caption.h
    presentation/ui/cyella_policy.cpp
    presentation/ui/cyella_policy.h
    presentation/ui/element_kinds.h
    presentation/ui/ui_helpers.cpp
    presentation/ui/ui_helpers.h)

# snapshot을 luil element tree로 짓는다. 판마다 파일이 하나다.
cyella_add_source_filter(cyella_lib_files "presentation/ui/screens"
    presentation/ui/build_ui_tree.cpp
    presentation/ui/build_ui_tree.h
    presentation/ui/diff_special_card.cpp
    presentation/ui/diff_special_card.h
    presentation/ui/diff_webview.cpp
    presentation/ui/diff_webview.h
    presentation/ui/diff_webview_theme.cpp
    presentation/ui/diff_webview_theme.h
    presentation/ui/graph_row_element.cpp
    presentation/ui/graph_row_element.h
    presentation/ui/list_rows.cpp
    presentation/ui/list_rows.h
    presentation/ui/repository_ui.cpp
    presentation/ui/sidebar_extension_rows.cpp
    presentation/ui/sidebar_extension_rows.h
    presentation/ui/settings/settings_page_parts.h
    presentation/ui/settings/settings_ui.cpp
    presentation/ui/settings/settings_ui.h
    presentation/ui/settings/signing_ui.cpp
    presentation/ui/settings/signing_ui.h
    presentation/ui/start_page_ui.cpp
    presentation/ui/ui_parts.h)

# platform/win32: Win32 어댑터와 luil 앱 조립이다. 창·렌더러·입력은 luil 안에
# 있다. 실행 파일만 이 계층을 쓴다.
cyella_add_source_filter(cyella_lib_files "platform/process"
    platform/win32/operation_result_relay.cpp
    platform/win32/operation_result_relay.h
    platform/win32/utf8.cpp
    platform/win32/utf8.h
    platform/win32/win32_file_probe.cpp
    platform/win32/win32_file_probe.h
    platform/win32/win32_process_runner.cpp
    platform/win32/win32_process_runner.h
    platform/win32/win32_text_transcoder.cpp
    platform/win32/win32_text_transcoder.h
    platform/win32/win32_workspace_files.cpp
    platform/win32/win32_workspace_files.h)

# GitHub·GitLab 호출 조립과 브라우저 열기다.
cyella_add_source_filter(cyella_lib_files "platform/hosting"
    platform/win32/hosting_bridge.cpp
    platform/win32/hosting_bridge.h
    platform/win32/win32_url_opener.cpp
    platform/win32/win32_url_opener.h)

cyella_add_source_filter(cyella_lib_files "platform/diff_view"
    platform/win32/win32_diff_tool_launcher.cpp
    platform/win32/win32_diff_tool_launcher.h)

cyella_add_source_filter(cyella_lib_files "platform/webview"
    platform/win32/win32_webview_support.cpp
    platform/win32/win32_webview_support.h)

cyella_add_source_filter(cyella_lib_files "platform/settings"
    platform/win32/win32_app_settings_path.cpp
    platform/win32/win32_app_settings_path.h
    platform/win32/win32_file_io.cpp
    platform/win32/win32_file_io.h
    platform/win32/win32_settings_file_system.cpp
    platform/win32/win32_settings_file_system.h)

cyella_add_source_filter(cyella_lib_files "platform/watching"
    platform/win32/win32_foreground_monitor.cpp
    platform/win32/win32_foreground_monitor.h
    platform/win32/win32_repository_watcher.cpp
    platform/win32/win32_repository_watcher.h)

cyella_add_source_filter(cyella_lib_files "platform/authentication"
    platform/win32/askpass_bridge.cpp
    platform/win32/askpass_bridge.h
    platform/win32/askpass_helper_path.cpp
    platform/win32/askpass_helper_path.h
    platform/win32/askpass_protocol.cpp
    platform/win32/askpass_protocol.h
    platform/win32/rebase_editor_path.cpp
    platform/win32/rebase_editor_path.h
    platform/win32/win32_askpass_server.cpp
    platform/win32/win32_askpass_server.h
    platform/win32/win32_ssh_environment.cpp
    platform/win32/win32_ssh_environment.h)

cyella_add_source_filter(cyella_lib_files "platform/host"
    platform/win32/cyella_delegate.cpp
    platform/win32/cyella_delegate.h
    platform/win32/cyella_driver.cpp
    platform/win32/cyella_driver.h
    platform/win32/cyella_driver_signing.cpp)

# 저장소별 사용자 프로필, 명령 팔레트, 단축키 설정이다. 새 파일은 이 블록에 모은다.
cyella_add_source_filter(cyella_lib_files "domain/profiles"
    domain/git_identity.cpp
    domain/git_identity.h
    domain/hangul_search.cpp
    domain/hangul_search.h
    domain/user_profile.cpp
    domain/user_profile.h)

cyella_add_source_filter(cyella_lib_files "infrastructure/profiles"
    infrastructure/git_identity_gateway.cpp
    infrastructure/git_identity_gateway.h
    infrastructure/json_command_settings.cpp
    infrastructure/json_command_settings.h)

cyella_add_source_filter(cyella_lib_files "presentation/commands"
    presentation/commands/command_intents.h
    presentation/commands/command_registry.cpp
    presentation/commands/command_registry.h
    presentation/commands/key_chord.cpp
    presentation/commands/key_chord.h)

cyella_add_source_filter(cyella_lib_files "presentation/palette"
    presentation/palette/palette_intents.h
    presentation/palette/palette_model.cpp
    presentation/palette/palette_model.h
    presentation/palette/palette_state.h
    presentation/palette/palette_view.h
    presentation/palette/shell_controller_palette.cpp)

# 환경 설정의 단축키·사용자 프로필 쪽: 상태·snapshot·메시지와 controller 처리, 그리고 저장소별 프로필을 shell_controller에 잇는 부분이다.
cyella_add_source_filter(cyella_lib_files "presentation/settings/profiles"
    presentation/settings/profiles_intents.h
    presentation/settings/profiles_page.cpp
    presentation/settings/profiles_page.h
    presentation/settings/settings_controller_profiles.cpp
    presentation/settings/settings_controller_shortcuts.cpp
    presentation/settings/shell_controller_profiles.cpp
    presentation/settings/shortcuts_intents.h
    presentation/settings/shortcuts_page.cpp
    presentation/settings/shortcuts_page.h)

cyella_add_source_filter(cyella_lib_files "platform/identity"
    platform/win32/win32_git_environment.cpp
    platform/win32/win32_git_environment.h)

cyella_add_source_filter(cyella_lib_files "presentation/ui/palette"
    presentation/ui/command_elements.cpp
    presentation/ui/command_elements.h
    presentation/ui/palette_ui.cpp
    presentation/ui/palette_ui.h
    presentation/ui/settings/profiles_ui.cpp
    presentation/ui/settings/settings_page_parts.h
    presentation/ui/settings/settings_pages_ui.h
    presentation/ui/settings/shortcuts_ui.cpp)

add_library(cyella_lib STATIC ${cyella_lib_files})
target_include_directories(cyella_lib
    PUBLIC
        "${CYELLA_SOURCE_DIRECTORY}")
# luil이 Skia·nlohmann(single_include)·WebView2 로더·Windows 시스템 라이브러리를
# PUBLIC으로 나른다.
target_link_libraries(cyella_lib
    PUBLIC
        cyella_project_options
        nlohmann_json::nlohmann_json
        luil::luil
        # askpass 서버(난수·보안 기술자·서비스 제어 관리자)와 설정 파일 위치
        # (SHGetKnownFolderPath·CoTaskMemFree)가 직접 쓰는 시스템 라이브러리다.
        bcrypt
        advapi32
        ole32
        shell32)

# resource는 두 .rc다: luil 조각(코디콘 폰트·라이선스·고지, ID 101~104)과 cyella
# 조각(manifest·앱 아이콘(ID 105)·캡션 아이콘 PNG(ID 106)·VERSIONINFO).
set(cyella_app_files)
cyella_add_source_filter(cyella_app_files "app"
    platform/win32/main.cpp)
cyella_add_source_filter(cyella_app_files "resource"
    "${CYELLA_GENERATED_LUIL_RESOURCE}"
    "${CYELLA_GENERATED_RESOURCE}")
add_executable(cyella WIN32 ${cyella_app_files})
target_link_libraries(cyella PRIVATE cyella_lib)
cyella_disable_automatic_windows_manifest(cyella)
cyella_copy_diff_viewer_assets(cyella)

# askpass 도우미다. git과 OpenSSH가 비밀이 필요할 때 실행하는 작은 프로그램이며 앱의 named pipe에
# 묻는다. luil·Skia에 링크하지 않으려고 앱 라이브러리와 별개로 필요한 소스만 직접 컴파일한다.
# 콘솔 창이 뜨지 않도록 WIN32 subsystem이고, 앱이 실행 파일 옆에서 찾으므로 이름이 고정이다.
set(cyella_askpass_files)
cyella_add_source_filter(cyella_askpass_files "askpass"
    platform/win32/askpass_helper_main.cpp
    platform/win32/askpass_protocol.cpp
    platform/win32/askpass_protocol.h
    platform/win32/utf8.cpp
    platform/win32/utf8.h)
add_executable(cyella_askpass WIN32 ${cyella_askpass_files})
set_target_properties(cyella_askpass PROPERTIES OUTPUT_NAME "cyella-askpass")
target_include_directories(cyella_askpass PRIVATE "${CYELLA_SOURCE_DIRECTORY}")
target_link_libraries(cyella_askpass
    PRIVATE
        cyella_project_options
        shell32)
cyella_disable_automatic_windows_manifest(cyella_askpass)

# 인터랙티브 rebase의 편집기 도우미다 (ADR-005). git이 `GIT_SEQUENCE_EDITOR`·`GIT_EDITOR`로 실행해 앱이 준비한
# 할 일 목록과 메시지를 파일에 쓴다. askpass 도우미처럼 luil·Skia에 링크하지 않고 할 일 규칙만 직접 컴파일한다.
set(cyella_rebase_editor_files)
cyella_add_source_filter(cyella_rebase_editor_files "rebase_editor"
    domain/interactive_rebase.cpp
    domain/interactive_rebase.h
    platform/win32/rebase_editor_main.cpp)
add_executable(cyella_rebase_editor WIN32 ${cyella_rebase_editor_files})
set_target_properties(cyella_rebase_editor PROPERTIES OUTPUT_NAME "cyella-rebase-editor")
target_include_directories(cyella_rebase_editor PRIVATE "${CYELLA_SOURCE_DIRECTORY}")
target_link_libraries(cyella_rebase_editor
    PRIVATE
        cyella_project_options
        shell32)
cyella_disable_automatic_windows_manifest(cyella_rebase_editor)

# 앱이 도우미를 자기 옆에서 찾으므로 둘을 같은 디렉터리에 설치한다.
add_dependencies(cyella cyella_askpass cyella_rebase_editor)
install(TARGETS cyella cyella_askpass cyella_rebase_editor RUNTIME DESTINATION .)
# 웹뷰가 실행 파일 옆의 이 폴더에서 `file://`로 뷰어를 연다.
install(DIRECTORY "${CYELLA_DIFF_VIEWER_DIRECTORY}/" DESTINATION diff-viewer)
