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

# domain: 순수 규칙과 값 타입이다. 다른 계층에 의존하지 않는다. 아직 파일이 없다.
# infrastructure: application 계약의 구현이다 (Git 호출 등). 아직 파일이 없다.

# application: 상태 소유와 계약(interface)이다. OS API를 직접 부르지 않고 luil
# UI 헤더를 include하지 않는다.
cyella_add_source_filter(cyella_lib_files "application/logic"
    application/app_messages.h)

# presentation: view snapshot을 luil element tree로 짓는 앱 element 계층이다.
cyella_add_source_filter(cyella_lib_files "presentation/ui/foundation"
    presentation/ui/cyella_caption.h)

# platform/win32: Win32 어댑터와 luil 앱 조립이다. 창·렌더러·입력은 luil 안에
# 있다. 실행 파일만 이 계층을 쓴다.
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
        luil::luil)

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

install(TARGETS cyella RUNTIME DESTINATION .)
