set(cyella_resource_directory "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/resources")
set(cyella_manifest_file "${cyella_resource_directory}/cyella.manifest")
set(cyella_resource_template "${cyella_resource_directory}/cyella.rc.in")

file(TO_CMAKE_PATH "${cyella_manifest_file}" CYELLA_MANIFEST_PATH)
configure_file(
    "${cyella_resource_template}"
    "${CYELLA_GENERATED_RESOURCE}"
    @ONLY)
set_source_files_properties(
    "${CYELLA_GENERATED_RESOURCE}"
    PROPERTIES
        GENERATED TRUE
        OBJECT_DEPENDS "${cyella_manifest_file}")

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

# operation 하나의 상태(contract의 operation_status)와 실패·입력 종류다.
cyella_add_source_filter(cyella_lib_files "domain/operations"
    domain/operation_status.cpp
    domain/operation_status.h)

cyella_add_source_filter(cyella_lib_files "domain/rules"
    domain/ref_name.cpp
    domain/ref_name.h)

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
    domain/credential_prompt.h)

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
    application/repository_watcher.h)

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
    infrastructure/ssh_tools.cpp
    infrastructure/ssh_tools.h)

# operation을 git 명령으로 바꿔 실행하는 실행기다. 명령 조립기·해석기·process runner를 잇는다.
cyella_add_source_filter(cyella_lib_files "infrastructure/git_operations"
    infrastructure/git_operation_context.cpp
    infrastructure/git_operation_context.h
    infrastructure/git_operation_runner.cpp
    infrastructure/git_operation_runner.h
    infrastructure/git_read_operations.cpp
    infrastructure/git_write_operations.cpp)

# presentation: view snapshot을 luil element tree로 짓는 앱 element 계층이다.
cyella_add_source_filter(cyella_lib_files "presentation/start_page"
    presentation/start_page_view.cpp
    presentation/start_page_view.h)

cyella_add_source_filter(cyella_lib_files "presentation/ui/foundation"
    presentation/ui/cyella_caption.h)

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

cyella_add_source_filter(cyella_lib_files "platform/settings"
    platform/win32/win32_app_settings_path.cpp
    platform/win32/win32_app_settings_path.h
    platform/win32/win32_file_io.cpp
    platform/win32/win32_file_io.h
    platform/win32/win32_settings_file_system.cpp
    platform/win32/win32_settings_file_system.h)

cyella_add_source_filter(cyella_lib_files "platform/watching"
    platform/win32/win32_repository_watcher.cpp
    platform/win32/win32_repository_watcher.h)

cyella_add_source_filter(cyella_lib_files "platform/authentication"
    platform/win32/askpass_bridge.cpp
    platform/win32/askpass_bridge.h
    platform/win32/askpass_helper_path.cpp
    platform/win32/askpass_helper_path.h
    platform/win32/askpass_protocol.cpp
    platform/win32/askpass_protocol.h
    platform/win32/win32_askpass_server.cpp
    platform/win32/win32_askpass_server.h
    platform/win32/win32_ssh_environment.cpp
    platform/win32/win32_ssh_environment.h)

cyella_add_source_filter(cyella_lib_files "platform/host"
    platform/win32/cyella_delegate.cpp
    platform/win32/cyella_delegate.h
    platform/win32/cyella_driver.cpp
    platform/win32/cyella_driver.h)

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
# 조각(manifest·VERSIONINFO).
set(cyella_app_files)
cyella_add_source_filter(cyella_app_files "app"
    platform/win32/main.cpp)
cyella_add_source_filter(cyella_app_files "resource"
    "${CYELLA_GENERATED_LUIL_RESOURCE}"
    "${CYELLA_GENERATED_RESOURCE}")
add_executable(cyella WIN32 ${cyella_app_files})
target_link_libraries(cyella PRIVATE cyella_lib)
cyella_disable_automatic_windows_manifest(cyella)

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

# 앱이 도우미를 자기 옆에서 찾으므로 둘을 같은 디렉터리에 설치한다.
add_dependencies(cyella cyella_askpass)
install(TARGETS cyella cyella_askpass RUNTIME DESTINATION .)
