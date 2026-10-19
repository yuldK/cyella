cmake_minimum_required(VERSION 4.2.0 FATAL_ERROR)

cmake_policy(SET CMP0091 NEW)

set(CYELLA_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MSVC_RUNTIME_LIBRARY "${CYELLA_MSVC_RUNTIME_LIBRARY}")

project(cyella VERSION 0.1.0 LANGUAGES CXX RC)

set(CYELLA_CXX_STANDARD 20)
set(CYELLA_MINIMUM_MSVC_VERSION 1940)
set(CYELLA_MINIMUM_WINDOWS_SDK_VERSION "10.0.22621.0")
set(CYELLA_TARGET_ARCHITECTURE "x64")
set(CYELLA_SUPPORTED_GENERATORS
    "Visual Studio 17 2022"
    "Visual Studio 18 2026"
)

set(CYELLA_CMAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/cmake")
set(CYELLA_THIRD_PARTY_DIRECTORY "${PROJECT_SOURCE_DIR}/third_party")
set(CYELLA_SOURCE_DIRECTORY "${PROJECT_SOURCE_DIR}/src")
set(CYELLA_TEST_DIRECTORY "${PROJECT_SOURCE_DIR}/tests")
set(CYELLA_GENERATED_DIRECTORY "${PROJECT_BINARY_DIR}/generated")
set(CYELLA_GENERATED_RESOURCE "${CYELLA_GENERATED_DIRECTORY}/cyella.rc")
# luil이 요구하는 resource(코디콘 폰트·라이선스·고지) 조각이다. cyella.rc와 함께
# 실행 파일과 test 실행 파일에 들어간다. ID 규칙: 101~104는 luil이 쓰고,
# 앱 자신의 resource(아이콘 등)는 105부터 정의한다.
set(CYELLA_GENERATED_LUIL_RESOURCE "${CYELLA_GENERATED_DIRECTORY}/luil_resources.rc")
set(CYELLA_THIRD_PARTY_NOTICES
    "${CYELLA_GENERATED_DIRECTORY}/third_party_notices.txt")
set(CYELLA_SOURCE_STYLE_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/check_source_style.ps1")
set(CYELLA_SOURCE_STYLE_CMAKE_SCRIPT "${CYELLA_CMAKE_DIRECTORY}/run_style_check.cmake")

# 의존성 위치. 전부 submodule이다. Skia·WebView2 SDK는 luil의 패키지 스크립트가
# third_party/luil 안에 준비한다.
set(CYELLA_NLOHMANN_JSON_ROOT "${CYELLA_THIRD_PARTY_DIRECTORY}/nlohmann-json")
set(CYELLA_CATCH2_ROOT "${CYELLA_THIRD_PARTY_DIRECTORY}/catch2")

set(CMAKE_CXX_STANDARD ${CYELLA_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# C++20 module은 쓰지 않는다. 생성기의 module 의존성 스캔(/scanDependencies)을 끄면
# 빌드가 빨라지고 파일 하나만 골라 컴파일하는 도구 경로가 전체 스캔에 막히지 않는다.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# 기본 솔루션에는 실제 빌드에 필요한 target(cyella, cyella_lib)만 둔다. test와
# 개발 도구 target은 명시적으로 켤 때만 만들어진다.
#   cmake --preset vs2022-tests     → test target과 CTest 등록 포함
#   -DCYELLA_BUILD_TOOLING=ON       → clang-format·style 검사 target 포함
option(CYELLA_BUILD_TESTS "Build the Cyella test targets." OFF)
option(CYELLA_BUILD_TOOLING "Build the clang-format and source style targets." OFF)
option(CYELLA_ENABLE_MSVC_ANALYZE "Enable MSVC /analyze for project sources." OFF)

include("${CYELLA_CMAKE_DIRECTORY}/macro.cmake")
include("${CYELLA_CMAKE_DIRECTORY}/compiler/msvc.cmake")
include("${CYELLA_CMAKE_DIRECTORY}/platform/windows.cmake")

cyella_validate_windows_platform(
    ARCHITECTURE "${CYELLA_TARGET_ARCHITECTURE}"
    MINIMUM_SDK_VERSION "${CYELLA_MINIMUM_WINDOWS_SDK_VERSION}"
    SUPPORTED_GENERATORS ${CYELLA_SUPPORTED_GENERATORS}
)
cyella_validate_msvc(MINIMUM_VERSION "${CYELLA_MINIMUM_MSVC_VERSION}")
cyella_configure_msvc_link_directories()

add_library(cyella_project_options INTERFACE)
target_compile_features(cyella_project_options INTERFACE cxx_std_${CYELLA_CXX_STANDARD})
cyella_apply_windows_options(cyella_project_options)
cyella_apply_msvc_options(
    cyella_project_options
    ENABLE_ANALYZE "${CYELLA_ENABLE_MSVC_ANALYZE}"
)

find_program(CYELLA_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

# 의존성은 submodule과 luil이 준비한 패키지로만 구성한다. 취득을
# 자동화하지 않으므로 configure는 네트워크를 전혀 쓰지 않는다.
include("${CYELLA_CMAKE_DIRECTORY}/dependencies.cmake")
cyella_find_dependencies(BUILD_TESTS ${CYELLA_BUILD_TESTS})

# UI 프레임워크 luil. submodule 안의 Skia 패키지·WebView2 SDK는 luil의
# 스크립트가 준비하며 configure는 존재만 검사한다. Skia·nlohmann(single_include)·
# WebView2 로더는 luil::luil이 PUBLIC으로 나른다.
#  - LUIL_BUILD_TESTS는 반드시 OFF다. 켜면 luil이 자기 Catch2를 같은 바이너리
#    디렉터리(third_party/catch2)에 add_subdirectory해 cyella의 것과 충돌한다.
#  - SYSTEM: luil 공개 헤더를 cyella의 /W4·/analyze 밖에 둔다.
#  - 정적 CRT 설정은 project() 앞의 것을 luil이 그대로 따른다.
set(LUIL_BUILD_TESTS OFF)
set(LUIL_BUILD_EXAMPLES OFF)
set(LUIL_BUILD_TOOLING OFF)
add_subdirectory(third_party/luil SYSTEM)
# luil의 고지·resource 함수는 호출 scope의 세 변수를 읽는다. 하위 디렉터리의
# 일반 변수는 부모로 올라오지 않으므로 여기서 다시 정한다.
set(LUIL_SOURCE_DIRECTORY "${CYELLA_THIRD_PARTY_DIRECTORY}/luil/src")
set(LUIL_ASSET_DIRECTORY "${CYELLA_THIRD_PARTY_DIRECTORY}/luil/assets/codicons")
set(LUIL_NLOHMANN_JSON_ROOT "${CYELLA_THIRD_PARTY_DIRECTORY}/luil/third_party/nlohmann_json")
# 제3자 고지와 .rc 조각은 src/와 tests/가 함께 쓰므로 여기서 한 번 만든다.
luil_generate_third_party_notices("${CYELLA_THIRD_PARTY_NOTICES}")
luil_configure_resource_script("${CYELLA_GENERATED_LUIL_RESOURCE}" "${CYELLA_THIRD_PARTY_NOTICES}")

# `include(CTest)`는 Continuous·Experimental·Nightly 등 dashboard target을 함께
# 만들어 솔루션을 부풀린다. 필요한 것은 test 등록뿐이라 enable_testing만 쓴다.
if(CYELLA_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)
if(CYELLA_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(CYELLA_BUILD_TOOLING)
    cyella_find_visual_studio_clang_format(CYELLA_CLANG_FORMAT_EXECUTABLE)
    cyella_add_clang_format_targets(
        CLANG_FORMAT_EXECUTABLE "${CYELLA_CLANG_FORMAT_EXECUTABLE}"
        FORMAT_TARGET cyella_format
        CHECK_TARGET cyella_format_check
        SOURCE_DIRECTORIES
            "${CYELLA_SOURCE_DIRECTORY}"
            "${CYELLA_TEST_DIRECTORY}"
        FILE_EXTENSIONS cpp h
        CHECK_COMMAND
            "${CYELLA_POWERSHELL_EXECUTABLE}"
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CYELLA_SOURCE_STYLE_SCRIPT}"
            -root "${PROJECT_SOURCE_DIR}"
    )
    cyella_set_ide_folder("Tooling"
        cyella_format
        cyella_format_check)
endif()

# IDE 필터 구성은 target 정의와 분리해 이 한 곳에서만 관리한다. 실행 파일 target인
# `cyella`만 최상위에 남겨 솔루션을 열었을 때 진입점이 바로 보이게 한다.
cyella_set_ide_folder("Libraries" cyella_lib luil)
if(CYELLA_BUILD_TESTS)
    cyella_set_ide_folder("Tests" cyella_tests cyella_test_child)
endif()
