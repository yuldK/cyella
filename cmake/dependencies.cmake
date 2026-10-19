include_guard(GLOBAL)

# 의존성 구성을 한 곳에 모은다. 취득은 submodule로 끝나며, 이
# 파일이 실행하는 어떤 경로에도 네트워크 접근이 없다. Skia는 luil의 패키지로
# 오므로 여기 없다.
# 함수 본문의 CMAKE_CURRENT_LIST_DIR은 호출 지점 기준으로 평가되므로 include
# 시점의 경로를 따로 붙잡아 둔다.
set(CYELLA_DEPENDENCIES_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/dependencies")

# 파일을 include하는 것만으로는 부작용이 없다. Catch2를 실제로 구성할지는
# cyella_find_dependencies의 BUILD_TESTS가 정한다.
include("${CYELLA_DEPENDENCIES_DIRECTORY}/nlohmann_json.cmake")
include("${CYELLA_DEPENDENCIES_DIRECTORY}/catch2.cmake")

function(cyella_find_dependencies)
    cmake_parse_arguments(PARSE_ARGV 0 arguments "" "BUILD_TESTS" "")
    if(arguments_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "Unknown arguments passed to cyella_find_dependencies: "
            "${arguments_UNPARSED_ARGUMENTS}")
    endif()

    cyella_find_nlohmann_json()

    # Catch2는 test 구성에서만 요구한다. 앱만 빌드하는 환경에서는 submodule이
    # 초기화되어 있지 않아도 된다.
    if(arguments_BUILD_TESTS)
        cyella_find_catch2()
    endif()

    # 하위 함수가 설정한 구성 매핑을 호출자 범위로 올린다.
    set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL
        "${CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL}" PARENT_SCOPE)
    set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO
        "${CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO}" PARENT_SCOPE)
    set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
endfunction()
