include_guard(GLOBAL)

# nlohmann/json은 submodule의 CMake 프로젝트를 그대로 쓴다. `nlohmann_json::nlohmann_json`
# target과 include 경로를 upstream이 제공하므로 `src`의 링크 구문은 바뀌지 않는다.
function(cyella_find_nlohmann_json)
    if(NOT EXISTS "${CYELLA_NLOHMANN_JSON_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "The nlohmann/json submodule is not initialized: "
            "${CYELLA_NLOHMANN_JSON_ROOT}\n"
            "Run: git submodule update --init third_party/nlohmann-json")
    endif()

    # test와 install 규칙은 Cyella 솔루션을 부풀리기만 한다.
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    add_subdirectory("${CYELLA_NLOHMANN_JSON_ROOT}" "${CMAKE_BINARY_DIR}/third_party/nlohmann-json" EXCLUDE_FROM_ALL)

    if(NOT TARGET nlohmann_json::nlohmann_json)
        message(FATAL_ERROR
            "nlohmann_json::nlohmann_json was not defined by the submodule.")
    endif()
endfunction()
