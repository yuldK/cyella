# Windows의 실행 파일 resource(.rc·manifest)와 실행 파일 target이다. src/CMakeLists.txt가 cyella_lib을 세운 뒤 include한다.
# include라 경로의 기준(CMAKE_CURRENT_SOURCE_DIR)은 src/다.

set(cyella_resource_directory "${CMAKE_CURRENT_SOURCE_DIR}/platform/win32/resources")
# manifest의 assemblyIdentity 버전이 VERSIONINFO처럼 project()의 버전을 따르게 템플릿에서 빌드 폴더로 만든다.
set(cyella_manifest_template "${cyella_resource_directory}/cyella.manifest.in")
set(cyella_manifest_file "${CYELLA_GENERATED_DIRECTORY}/cyella.manifest")
set(cyella_resource_ids_file "${cyella_resource_directory}/resource_ids.h")
set(cyella_resource_template "${cyella_resource_directory}/cyella.rc.in")
# 앱 아이콘은 assets/cyella.svg에서 scripts/generate_app_icon.py로 만든다. 앱 심볼과 타이틀은 SVG를 그대로 실어 벡터로 그린다.
set(cyella_app_icon "${PROJECT_SOURCE_DIR}/assets/cyella.ico")
set(cyella_app_symbol "${PROJECT_SOURCE_DIR}/assets/cyella.svg")
set(cyella_app_title "${PROJECT_SOURCE_DIR}/assets/cyella-title.svg")
set(cyella_app_title_sea "${PROJECT_SOURCE_DIR}/assets/cyella-title-sea.svg")
# 화면 문구 파일이다 (ADR-008). 언어마다 키 → 문구 JSON 하나이고 실행 파일에 RCDATA로 싣는다.
set(cyella_locale_ko "${PROJECT_SOURCE_DIR}/assets/locales/ko.json")
set(cyella_locale_en "${PROJECT_SOURCE_DIR}/assets/locales/en.json")

file(TO_CMAKE_PATH "${cyella_manifest_file}" CYELLA_MANIFEST_PATH)
file(TO_CMAKE_PATH "${cyella_resource_ids_file}" CYELLA_RESOURCE_IDS_PATH)
file(TO_CMAKE_PATH "${cyella_app_icon}" CYELLA_APP_ICON_PATH)
file(TO_CMAKE_PATH "${cyella_app_symbol}" CYELLA_SYMBOL_PATH)
file(TO_CMAKE_PATH "${cyella_app_title}" CYELLA_TITLE_PATH)
file(TO_CMAKE_PATH "${cyella_app_title_sea}" CYELLA_TITLE_SEA_PATH)
file(TO_CMAKE_PATH "${cyella_locale_ko}" CYELLA_LOCALE_KO_PATH)
file(TO_CMAKE_PATH "${cyella_locale_en}" CYELLA_LOCALE_EN_PATH)
configure_file(
    "${cyella_manifest_template}"
    "${cyella_manifest_file}"
    @ONLY)
configure_file(
    "${cyella_resource_template}"
    "${CYELLA_GENERATED_RESOURCE}"
    @ONLY)
set_source_files_properties(
    "${CYELLA_GENERATED_RESOURCE}"
    PROPERTIES
        GENERATED TRUE
        OBJECT_DEPENDS "${cyella_manifest_file};${cyella_resource_ids_file};${cyella_app_icon};${cyella_app_symbol};${cyella_app_title};${cyella_app_title_sea};${cyella_locale_ko};${cyella_locale_en}")

# 도우미(askpass·rebase 편집기)의 manifest만 싣는 .rc다. 도우미도 /MANIFEST:NO라 자동 manifest가 없으므로 앱처럼 직접 싣는다.
# git이 넘기는 경로(깊은 저장소의 `.git/rebase-merge/git-rebase-todo` 등)가 260자를 넘을 수 있고, longPathAware가 없으면
# 시스템의 긴 경로 설정이 켜져 있어도 이 프로세스는 그 경로를 열지 못한다. 앱의 manifest와 달리 창이 없어 DPI 설정은 두지 않는다.
# 앱의 manifest처럼 assemblyIdentity 버전이 project()의 버전을 따르게 템플릿에서 빌드 폴더로 만든다.
set(cyella_helper_manifest_template "${CYELLA_CMAKE_DIRECTORY}/resources/cyella_helper.manifest.in")
set(cyella_helper_manifest_file "${CYELLA_GENERATED_DIRECTORY}/cyella_helper.manifest")
set(cyella_helper_resource_template "${CYELLA_CMAKE_DIRECTORY}/resources/cyella_helper.rc.in")
set(CYELLA_GENERATED_HELPER_RESOURCE "${CYELLA_GENERATED_DIRECTORY}/cyella_helper.rc")

file(TO_CMAKE_PATH "${cyella_helper_manifest_file}" CYELLA_HELPER_MANIFEST_PATH)
configure_file(
    "${cyella_helper_manifest_template}"
    "${cyella_helper_manifest_file}"
    @ONLY)
configure_file(
    "${cyella_helper_resource_template}"
    "${CYELLA_GENERATED_HELPER_RESOURCE}"
    @ONLY)
set_source_files_properties(
    "${CYELLA_GENERATED_HELPER_RESOURCE}"
    PROPERTIES
        GENERATED TRUE
        OBJECT_DEPENDS "${cyella_helper_manifest_file}")


# askpass 서버(난수·보안 기술자·서비스 제어 관리자)와 설정 파일 위치(SHGetKnownFolderPath·CoTaskMemFree)가 직접 쓰는
# 시스템 라이브러리다.
target_link_libraries(cyella_lib
    PUBLIC
        bcrypt
        advapi32
        ole32
        shell32)

# resource는 두 .rc다: luil 조각(코디콘 폰트·라이선스·고지, ID 101~104)과 cyella
# 조각(manifest·앱 아이콘(ID 105)·앱 심볼 SVG(ID 106)·타이틀 SVG 두 장(ID 107·108)·문구 파일 두 개(ID 109·110)·VERSIONINFO).
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
cyella_add_source_filter(cyella_askpass_files "resource"
    "${CYELLA_GENERATED_HELPER_RESOURCE}")
add_executable(cyella_askpass WIN32 ${cyella_askpass_files})
set_target_properties(cyella_askpass PROPERTIES OUTPUT_NAME "cyella-askpass")
target_include_directories(cyella_askpass PRIVATE "${CYELLA_SOURCE_DIRECTORY}")
target_link_libraries(cyella_askpass
    PRIVATE
        cyella_project_options
        shell32)
cyella_disable_automatic_windows_manifest(cyella_askpass)
# 도우미는 git이 저장소 폴더에서 띄운다. 현재 폴더나 PATH에 놓인 같은 이름의 DLL을 읽지 않게, 정적으로 잇는 DLL은 System32에서만
# 찾는다(LOAD_LIBRARY_SEARCH_SYSTEM32). CRT는 정적으로 잇고 나머지는 시스템 DLL이라 이것으로 충분하다. 주 앱은 WebView2·셸 확장·IME가
# 다른 폴더의 DLL을 쓰므로 실제 실행으로 확인하기 전에는 걸지 않는다.
target_link_options(cyella_askpass PRIVATE /DEPENDENTLOADFLAG:0x800)

# 인터랙티브 rebase의 편집기 도우미다 (ADR-005). git이 `GIT_SEQUENCE_EDITOR`·`GIT_EDITOR`로 실행해 앱이 준비한
# 할 일 목록과 메시지를 파일에 쓴다. askpass 도우미처럼 luil·Skia에 링크하지 않고 할 일 규칙만 직접 컴파일한다.
# 할 일 규칙의 검사 문구가 카탈로그를 쓰므로 카탈로그 소스도 함께 넣는다. 도우미는 문구를 보이지 않으므로 카탈로그를
# 정하지 않는다 (ADR-008).
set(cyella_rebase_editor_files)
cyella_add_source_filter(cyella_rebase_editor_files "rebase_editor"
    domain/interactive_rebase.cpp
    domain/interactive_rebase.h
    domain/korean_josa.cpp
    domain/korean_josa.h
    domain/text_catalog.cpp
    domain/text_catalog.h
    platform/win32/rebase_editor_main.cpp)
cyella_add_source_filter(cyella_rebase_editor_files "resource"
    "${CYELLA_GENERATED_HELPER_RESOURCE}")
add_executable(cyella_rebase_editor WIN32 ${cyella_rebase_editor_files})
set_target_properties(cyella_rebase_editor PROPERTIES OUTPUT_NAME "cyella-rebase-editor")
target_include_directories(cyella_rebase_editor PRIVATE "${CYELLA_SOURCE_DIRECTORY}")
target_link_libraries(cyella_rebase_editor
    PRIVATE
        cyella_project_options
        shell32)
cyella_disable_automatic_windows_manifest(cyella_rebase_editor)
target_link_options(cyella_rebase_editor PRIVATE /DEPENDENTLOADFLAG:0x800)

# 앱이 도우미를 자기 옆에서 찾으므로 둘을 같은 디렉터리에 설치한다.
add_dependencies(cyella cyella_askpass cyella_rebase_editor)
install(TARGETS cyella cyella_askpass cyella_rebase_editor RUNTIME DESTINATION .)
# 웹뷰가 실행 파일 옆의 이 폴더에서 `file://`로 뷰어를 연다.
install(DIRECTORY "${CYELLA_DIFF_VIEWER_DIRECTORY}/" DESTINATION diff-viewer)
