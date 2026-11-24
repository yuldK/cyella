# macOS의 실행 파일 target과 앱 번들이다. src/CMakeLists.txt가 cyella_lib을 세운 뒤 include한다.
# include라 경로의 기준(CMAKE_CURRENT_SOURCE_DIR)은 src/다.
#
# 번들 배치 (Windows의 `bin\` 배치와 같은 뜻이다):
#   Cyella.app/Contents/MacOS/Cyella                  앱
#   Cyella.app/Contents/MacOS/cyella-askpass          askpass 도우미 (앱이 자기 옆에서 찾는다)
#   Cyella.app/Contents/MacOS/cyella-rebase-editor    rebase 편집기 도우미 (같다)
#   Cyella.app/Contents/Resources/diff-viewer/        diff 뷰어 (WKWebView가 `file://`로 연다)
#   Cyella.app/Contents/Resources/third_party_notices.txt
#  - 도우미는 코드라 `Contents/MacOS`에 둔다. 코드가 아닌 파일이 `Contents/MacOS`에 있으면 codesign이 번들 서명을 거절한다.
#  - 문구 파일과 앱 심볼·타이틀 SVG는 실행 파일에 바이트 배열로 실렸다 (src/CMakeLists.txt의 embedded 목록).

# AppKit·Foundation은 delegate(파일 고르기)와 앞 창 전환 감시가, CoreServices는 파일 감시(FSEvents)와 브라우저 열기
# (Launch Services)가 직접 쓴다. luil도 같은 framework를 PUBLIC으로 나르지만 cyella가 직접 쓰는 것은 여기 적는다.
target_link_libraries(cyella_lib
    PUBLIC
        "-framework AppKit"
        "-framework CoreServices"
        "-framework Foundation")

set(cyella_app_files)
cyella_add_source_filter(cyella_app_files "app"
    platform/macos/macos_main.mm)
add_executable(cyella ${cyella_app_files})
target_link_libraries(cyella PRIVATE cyella_lib)
cyella_add_macos_bundle(cyella
    IDENTIFIER "io.github.yuldk.cyella"
    NAME "Cyella"
    EXECUTABLE "Cyella"
    TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/resources/Info.plist.in"
    ICON "cyella.icns")

# askpass 도우미다. git과 OpenSSH가 비밀이 필요할 때 실행하며 앱의 Unix 소켓에 묻는다. luil·Skia에 링크하지 않는다.
set(cyella_askpass_files)
cyella_add_source_filter(cyella_askpass_files "askpass"
    platform/macos/askpass_helper_main.cpp
    platform/win32/askpass_protocol.cpp
    platform/win32/askpass_protocol.h)
add_executable(cyella_askpass ${cyella_askpass_files})
set_target_properties(cyella_askpass PROPERTIES OUTPUT_NAME "cyella-askpass")
target_include_directories(cyella_askpass PRIVATE "${CYELLA_SOURCE_DIRECTORY}")
target_link_libraries(cyella_askpass PRIVATE cyella_project_options)

# 인터랙티브 rebase의 편집기 도우미다 (ADR-005). Windows 판처럼 할 일 규칙과 카탈로그 소스만 직접 컴파일한다.
set(cyella_rebase_editor_files)
cyella_add_source_filter(cyella_rebase_editor_files "rebase_editor"
    domain/interactive_rebase.cpp
    domain/interactive_rebase.h
    domain/korean_josa.cpp
    domain/korean_josa.h
    domain/text_catalog.cpp
    domain/text_catalog.h
    platform/macos/rebase_editor_main.cpp)
add_executable(cyella_rebase_editor ${cyella_rebase_editor_files})
set_target_properties(cyella_rebase_editor PROPERTIES OUTPUT_NAME "cyella-rebase-editor")
target_include_directories(cyella_rebase_editor PRIVATE "${CYELLA_SOURCE_DIRECTORY}")
target_link_libraries(cyella_rebase_editor PRIVATE cyella_project_options)

# 앱 아이콘(.icns)은 assets/cyella.ico에서 만든다. sips가 ICO의 가장 큰 그림을 icns로 옮긴다.
set(cyella_icon_file "${CMAKE_CURRENT_BINARY_DIR}/generated/cyella.icns")
add_custom_command(
    OUTPUT "${cyella_icon_file}"
    COMMAND sips -s format icns "${PROJECT_SOURCE_DIR}/assets/cyella.ico" --out "${cyella_icon_file}" >/dev/null
    DEPENDS "${PROJECT_SOURCE_DIR}/assets/cyella.ico"
    COMMENT "Converting the app icon to icns")
add_custom_target(cyella_icon DEPENDS "${cyella_icon_file}")
add_dependencies(cyella cyella_icon cyella_askpass cyella_rebase_editor)

# 도우미·자원을 번들 안에 놓고 마지막에 ad-hoc 서명한다.
#  - 도우미를 먼저 서명하고 번들을 서명해야 번들의 봉인이 도우미의 서명을 담는다.
#  - 배포용 서명(Developer ID·공증)이 아니다. 이 기계에서 돌리는 개발 빌드용이다.
add_custom_command(TARGET cyella POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:cyella_askpass>" "$<TARGET_FILE:cyella_rebase_editor>"
        "$<TARGET_BUNDLE_CONTENT_DIR:cyella>/MacOS/"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CYELLA_DIFF_VIEWER_DIRECTORY}"
        "$<TARGET_BUNDLE_CONTENT_DIR:cyella>/Resources/diff-viewer"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CYELLA_THIRD_PARTY_NOTICES}" "${cyella_icon_file}"
        "$<TARGET_BUNDLE_CONTENT_DIR:cyella>/Resources/"
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_CONTENT_DIR:cyella>/MacOS/cyella-askpass"
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_CONTENT_DIR:cyella>/MacOS/cyella-rebase-editor"
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:cyella>"
    COMMENT "Assembling and signing the Cyella app bundle (ad hoc)"
    VERBATIM)

install(TARGETS cyella BUNDLE DESTINATION .)
