include(Catch)

# test 파일도 라이브러리와 같은 방식으로 IDE 필터를 짠다 (cyella_add_source_filter).
# UI 기반 계층의 test는 luil 저장소가 갖는다. 여기에는 cyella의 의미를 검증하는
# test만 둔다. luil의 .rc 조각을 함께 넣어 코디콘 폰트·고지 resource가 test 실행
# 파일에도 실린다.
set(cyella_test_files)

cyella_add_source_filter(cyella_test_files "resource"
    "${CYELLA_GENERATED_LUIL_RESOURCE}"
    "${CYELLA_GENERATED_RESOURCE}")

cyella_add_source_filter(cyella_test_files "domain/graph"
    commit_graph_layout_tests.cpp)

cyella_add_source_filter(cyella_test_files "domain/settings"
    app_settings_tests.cpp
    external_diff_tool_tests.cpp)

cyella_add_source_filter(cyella_test_files "platform/host"
    cyella_driver_tests.cpp)

cyella_add_source_filter(cyella_test_files "platform/settings"
    win32_settings_tests.cpp)

# 실제 filesystem에서 감시 스레드를 돌리는 통합 test다. 태그는 `[integration]`이다.
cyella_add_source_filter(cyella_test_files "platform/watching"
    win32_repository_watcher_tests.cpp)

cyella_add_source_filter(cyella_test_files "domain/authentication"
    credential_prompt_tests.cpp)

cyella_add_source_filter(cyella_test_files "platform/authentication"
    askpass_git_integration_tests.cpp
    askpass_workspace_integration_tests.cpp
    askpass_server_tests.cpp
    ssh_askpass_integration_tests.cpp)

cyella_add_source_filter(cyella_test_files "support"
    askpass_test_support.h
    process_test_support.h
    temporary_directory_support.h)

cyella_add_source_filter(cyella_test_files "application"
    process_runner_tests.cpp
    process_support_tests.cpp
    repository_change_classifier_tests.cpp
    watch_refresh_policy_tests.cpp
    repository_tabs_tests.cpp
    task_scheduler_tests.cpp)

cyella_add_source_filter(cyella_test_files "infrastructure"
    git_execution_policy_tests.cpp
    git_progress_parser_tests.cpp
    git_tool_discovery_tests.cpp
    json_app_settings_store_tests.cpp
    secret_masking_tests.cpp
    ssh_operations_tests.cpp
    ssh_tools_tests.cpp)

cyella_add_source_filter(cyella_test_files "presentation"
    diff_webview_tests.cpp
    extension_view_tests.cpp
    graph_geometry_tests.cpp
    settings_tests.cpp
    settings_ui_tests.cpp
    shell_controller_tests.cpp
    dialog_controller_tests.cpp
    dialog_ui_tests.cpp
    menu_model_tests.cpp
    selector_model_tests.cpp
    shell_layout_tests.cpp
    start_page_view_tests.cpp
    ui_tree_perf_tests.cpp
    view_snapshot_support.h
    view_snapshot_tests.cpp)

# 프로세스 실행기 test가 띄우는 도우미다. stdin 반향, NUL을 포함한 바이트 출력, 손자
# 프로세스 생성처럼 실제 git으로는 흉내 내기 어려운 동작을 한다. console 앱이고 앱 코드와
# 링크하지 않으며 .rc도 싣지 않는다.
set(cyella_test_child_files)
cyella_add_source_filter(cyella_test_child_files "support"
    cyella_test_child.cpp)
add_executable(cyella_test_child ${cyella_test_child_files})
target_link_libraries(cyella_test_child
    PRIVATE
        cyella_project_options)
cyella_disable_automatic_windows_manifest(cyella_test_child)

# domain: 값 타입과 규칙의 test다. git이 필요 없다.
cyella_add_source_filter(cyella_test_files "application/diff_view"
    diff_view_controller_tests.cpp
    diff_view_coordinator_tests.cpp
    diff_view_git_tests.cpp
    diff_view_protocol_tests.cpp
    external_diff_opener_tests.cpp)

cyella_add_source_filter(cyella_test_files "domain"
    patch_builder_tests.cpp
    path_syntax_tests.cpp
    ref_name_tests.cpp
    repository_summary_tests.cpp)

# infrastructure: git 출력 해석기는 실제 git으로 만든 고정 출력(tests/data/git)을
# 읽어 검증한다. 명령줄 조립기는 순수 함수라 인자 목록만 비교한다.
cyella_add_source_filter(cyella_test_files "infrastructure/git_parsers"
    git_diff_parser_tests.cpp
    git_error_classifier_tests.cpp
    git_log_parser_tests.cpp
    git_query_parser_tests.cpp
    git_ref_parser_tests.cpp
    git_stash_parser_tests.cpp
    git_status_parser_tests.cpp)

cyella_add_source_filter(cyella_test_files "infrastructure/git_commands"
    git_command_builder_tests.cpp)

# submodule·worktree·Git LFS의 해석기·조립기 test다. 고정 출력은 tests/data/git에 있다.
cyella_add_source_filter(cyella_test_files "infrastructure/git_extensions"
    git_extension_parser_tests.cpp
    git_extension_runner_tests.cpp)

# 실제 git(과 git-lfs)으로 submodule·worktree·LFS operation을 끝까지 돌린다. 태그는 `[integration]`이다.
cyella_add_source_filter(cyella_test_files "application/repository_integration/extensions"
    git_extension_integration_tests.cpp)

# 저장소 세션: 가짜 runner(정해 둔 응답)와 수동 dispatcher로 명령 순서, 상태 전이, 늦은 결과
# 버리기, needs_input, 실패 분류를 검증한다. git과 디스크가 필요 없다.
cyella_add_source_filter(cyella_test_files "application/repository"
    fake_git_runner.h
    git_operation_runner_tests.cpp
    git_workspace_auth_tests.cpp
    git_workspace_tests.cpp)

# 저장소 세션을 실제 git과 임시 저장소(로컬 bare 원격)로 끝까지 돌린다.
cyella_add_source_filter(cyella_test_files "application/repository_integration"
    repository_session_integration_tests.cpp)

# 인터랙티브 rebase: 할 일 규칙의 단위 test와, 실제 git·편집기 도우미로 끝까지 돌리는 통합 test다.
cyella_add_source_filter(cyella_test_files "rebase"
    interactive_rebase_integration_tests.cpp
    interactive_rebase_tests.cpp
    rebase_editor_tests.cpp
    rebase_shell_tests.cpp)

# 실제 git으로 임시 저장소를 만들어 조립기와 해석기를 이어서 검증한다.
cyella_add_source_filter(cyella_test_files "infrastructure/git_integration"
    git_integration_tests.cpp
    patch_builder_git_tests.cpp
    git_test_support.cpp
    git_test_support.h
    temp_directory_tests.cpp)

# 명령 팔레트·프로필: 퍼지 일치(초성 검색), 명령 레지스트리·단축키, 팔레트 모델, 사용자 프로필과 git 신원이다.
cyella_add_source_filter(cyella_test_files "palette"
    command_registry_tests.cpp
    git_identity_integration_tests.cpp
    hangul_search_tests.cpp
    palette_model_tests.cpp
    user_profile_tests.cpp
    palette_profile_shell_tests.cpp)

add_executable(cyella_tests ${cyella_test_files})
# test가 도우미를 먼저 빌드하게 하고, 경로는 생성 시점의 target 파일로 받는다.
add_dependencies(cyella_tests cyella_test_child cyella_askpass cyella_rebase_editor)
target_compile_definitions(cyella_tests
    PRIVATE
        "CYELLA_TEST_CHILD_PATH=\"$<TARGET_FILE:cyella_test_child>\""
        "CYELLA_ASKPASS_PATH=\"$<TARGET_FILE:cyella_askpass>\""
        "CYELLA_REBASE_EDITOR_PATH=\"$<TARGET_FILE:cyella_rebase_editor>\"")
target_include_directories(cyella_tests
    PRIVATE
        "${CYELLA_SOURCE_DIRECTORY}"
        "${CYELLA_TEST_DIRECTORY}")
target_compile_definitions(cyella_tests
    PRIVATE
        CYELLA_TEST_DATA_DIRECTORY="${CYELLA_TEST_DIRECTORY}/data")
target_link_libraries(cyella_tests
    PRIVATE
        cyella_lib
        ws2_32
        Catch2::Catch2WithMain)
cyella_disable_automatic_windows_manifest(cyella_tests)

# hang 유형의 회귀가 CI에서 무기한 대기 대신 실패로 드러나도록 모든 Catch2 test에
# timeout을 부여한다.
# `[perf]` 측정(10만 커밋 레이아웃 등)은 숨김 태그라 기본 목록에 없고 ctest에도 넣지 않는다.
# 직접 부른다:  cyella_tests.exe "[perf]"
catch_discover_tests(cyella_tests TEST_SPEC "~[perf]" PROPERTIES TIMEOUT 120)

# 수동 점검 프로그램(CTest에 등록하지 않는다).
add_subdirectory(manual)

# diff 뷰어의 순수 JS(단어 단위 차이, 줄별 구문 강조 조립, 행 계산)를 node로 시험한다. node가 없으면
# 등록하지 않는다. 브라우저·DOM이 필요한 부분은 수동 점검 틀(tests/manual/diff_viewer_harness)이 맡는다.
find_program(CYELLA_NODE_EXECUTABLE NAMES node)
if(CYELLA_NODE_EXECUTABLE)
    add_test(NAME cyella_diff_viewer_js
        COMMAND "${CYELLA_NODE_EXECUTABLE}" --test "${CYELLA_TEST_DIRECTORY}/js/diff_viewer.test.js")
    set_tests_properties(cyella_diff_viewer_js PROPERTIES TIMEOUT 120)
endif()

add_test(NAME cyella_source_style
    COMMAND "${CMAKE_COMMAND}"
        "-DPOWERSHELL_EXECUTABLE=${CYELLA_POWERSHELL_EXECUTABLE}"
        "-DROOT_DIRECTORY=${PROJECT_SOURCE_DIR}"
        -P "${CYELLA_SOURCE_STYLE_CMAKE_SCRIPT}")
