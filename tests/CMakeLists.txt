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

cyella_add_source_filter(cyella_test_files "platform/host"
    cyella_driver_tests.cpp)

cyella_add_source_filter(cyella_test_files "support"
    process_test_support.h)

cyella_add_source_filter(cyella_test_files "application"
    process_runner_tests.cpp
    process_support_tests.cpp
    task_scheduler_tests.cpp)

cyella_add_source_filter(cyella_test_files "infrastructure"
    git_execution_policy_tests.cpp
    git_progress_parser_tests.cpp
    git_tool_discovery_tests.cpp
    secret_masking_tests.cpp)

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
cyella_add_source_filter(cyella_test_files "domain"
    ref_name_tests.cpp
    repository_summary_tests.cpp)

# infrastructure: git 출력 해석기는 실제 git으로 만든 고정 출력(tests/data/git)을
# 읽어 검증한다. 명령줄 조립기는 순수 함수라 인자 목록만 비교한다.
cyella_add_source_filter(cyella_test_files "infrastructure/git_parsers"
    git_diff_parser_tests.cpp
    git_error_classifier_tests.cpp
    git_log_parser_tests.cpp
    git_ref_parser_tests.cpp
    git_stash_parser_tests.cpp
    git_status_parser_tests.cpp)

cyella_add_source_filter(cyella_test_files "infrastructure/git_commands"
    git_command_builder_tests.cpp)

# 실제 git으로 임시 저장소를 만들어 조립기와 해석기를 이어서 검증한다.
cyella_add_source_filter(cyella_test_files "infrastructure/git_integration"
    git_integration_tests.cpp
    git_test_support.cpp
    git_test_support.h)

add_executable(cyella_tests ${cyella_test_files})
# test가 도우미를 먼저 빌드하게 하고, 경로는 생성 시점의 target 파일로 받는다.
add_dependencies(cyella_tests cyella_test_child)
target_compile_definitions(cyella_tests
    PRIVATE
        "CYELLA_TEST_CHILD_PATH=\"$<TARGET_FILE:cyella_test_child>\"")
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
        Catch2::Catch2WithMain)
cyella_disable_automatic_windows_manifest(cyella_tests)

# hang 유형의 회귀가 CI에서 무기한 대기 대신 실패로 드러나도록 모든 Catch2 test에
# timeout을 부여한다.
# `[perf]` 측정(10만 커밋 레이아웃 등)은 숨김 태그라 기본 목록에 없고 ctest에도 넣지 않는다.
# 직접 부른다:  cyella_tests.exe "[perf]"
catch_discover_tests(cyella_tests TEST_SPEC "~[perf]" PROPERTIES TIMEOUT 120)

add_test(NAME cyella_source_style
    COMMAND "${CMAKE_COMMAND}"
        "-DPOWERSHELL_EXECUTABLE=${CYELLA_POWERSHELL_EXECUTABLE}"
        "-DROOT_DIRECTORY=${PROJECT_SOURCE_DIR}"
        -P "${CYELLA_SOURCE_STYLE_CMAKE_SCRIPT}")
