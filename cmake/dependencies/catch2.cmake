include_guard(GLOBAL)

# Catch2는 test 구성에서만 필요하다. 실제 사용 환경은 앱만 빌드하므로 이 submodule이
# 초기화되어 있지 않아도 기본 빌드가 성립해야 한다. 이 파일은 CYELLA_BUILD_TESTS가
# 켜진 경우에만 include된다.
function(cyella_find_catch2)
    if(NOT EXISTS "${CYELLA_CATCH2_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "The Catch2 submodule is not initialized: ${CYELLA_CATCH2_ROOT}\n"
            "Catch2 is required only for the test configuration.\n"
            "Run: git submodule update --init third_party/catch2\n"
            "Or configure without tests (CYELLA_BUILD_TESTS=OFF).")
    endif()

    # Catch2 단언을 여러 스레드에서 안전하게 쓰기 위한 설정이다.
    set(CATCH_CONFIG_THREAD_SAFE_ASSERTIONS ON CACHE INTERNAL "")
    set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_EXTRAS OFF CACHE INTERNAL "")
    add_subdirectory("${CYELLA_CATCH2_ROOT}" "${CMAKE_BINARY_DIR}/third_party/catch2" EXCLUDE_FROM_ALL)

    if(NOT TARGET Catch2::Catch2WithMain)
        message(FATAL_ERROR
            "Catch2::Catch2WithMain was not defined by the submodule.")
    endif()

    # `catch_discover_tests`는 upstream의 extras에 있다.
    list(APPEND CMAKE_MODULE_PATH "${CYELLA_CATCH2_ROOT}/extras")
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
    include("${CYELLA_CATCH2_ROOT}/extras/Catch.cmake")

    # Catch2는 Cyella과 같은 정적 CRT로 빌드되어야 한다.
    set_target_properties(Catch2 Catch2WithMain PROPERTIES
        MSVC_RUNTIME_LIBRARY "${CYELLA_MSVC_RUNTIME_LIBRARY}")
    cyella_set_ide_folder("Tests" Catch2 Catch2WithMain)
endfunction()
