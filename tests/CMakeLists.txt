include(Catch)

# test 파일도 라이브러리와 같은 방식으로 IDE 필터를 짠다 (cyella_add_source_filter).
# UI 기반 계층의 test는 luil 저장소가 갖는다. 여기에는 cyella의 의미를 검증하는
# test만 둔다. luil의 .rc 조각을 함께 넣어 코디콘 폰트·고지 resource가 test 실행
# 파일에도 실린다.
set(cyella_test_files)

cyella_add_source_filter(cyella_test_files "resource"
    "${CYELLA_GENERATED_LUIL_RESOURCE}"
    "${CYELLA_GENERATED_RESOURCE}")

cyella_add_source_filter(cyella_test_files "platform/host"
    cyella_driver_tests.cpp)

add_executable(cyella_tests ${cyella_test_files})
target_include_directories(cyella_tests
    PRIVATE
        "${CYELLA_SOURCE_DIRECTORY}"
        "${CYELLA_TEST_DIRECTORY}")
target_link_libraries(cyella_tests
    PRIVATE
        cyella_lib
        Catch2::Catch2WithMain)
cyella_disable_automatic_windows_manifest(cyella_tests)

# hang 유형의 회귀가 CI에서 무기한 대기 대신 실패로 드러나도록 모든 Catch2 test에
# timeout을 부여한다.
catch_discover_tests(cyella_tests PROPERTIES TIMEOUT 120)

add_test(NAME cyella_source_style
    COMMAND "${CMAKE_COMMAND}"
        "-DPOWERSHELL_EXECUTABLE=${CYELLA_POWERSHELL_EXECUTABLE}"
        "-DROOT_DIRECTORY=${PROJECT_SOURCE_DIR}"
        -P "${CYELLA_SOURCE_STYLE_CMAKE_SCRIPT}")
