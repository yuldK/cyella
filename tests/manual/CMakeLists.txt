# 사람이 눈으로 확인하는 수동 점검 프로그램이다. CTest에 등록하지 않는다.
# 앱 코드(cyella_lib)와 luil만 링크하고 실행 파일 옆에 필요한 자산을 복사한다.

# luil 웹뷰가 file:// 문서를 열고 chrome.webview.postMessage를 주고받는지 본다
# (ADR-002의 file:// 결정을 확인한 근거다).
set(cyella_webview_file_probe_files)
cyella_add_source_filter(cyella_webview_file_probe_files "resource"
    "${CYELLA_GENERATED_LUIL_RESOURCE}"
    "${CYELLA_GENERATED_RESOURCE}")
cyella_add_source_filter(cyella_webview_file_probe_files "app"
    webview_file_probe/main.cpp)
add_executable(webview_file_probe WIN32 ${cyella_webview_file_probe_files})
target_link_libraries(webview_file_probe PRIVATE cyella_lib)
cyella_disable_automatic_windows_manifest(webview_file_probe)
add_custom_command(TARGET webview_file_probe POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/webview_file_probe/probe.html"
        "$<TARGET_FILE_DIR:webview_file_probe>/probe.html")


# diff 뷰어를 끝까지 이어 보는 프로그램이다. 임시 저장소의 diff를 웹뷰로 보이고 stage·discard·외부 도구
# 열기를 실제 git에 적용한다.
set(cyella_diff_viewer_demo_files)
cyella_add_source_filter(cyella_diff_viewer_demo_files "resource"
    "${CYELLA_GENERATED_LUIL_RESOURCE}"
    "${CYELLA_GENERATED_RESOURCE}")
cyella_add_source_filter(cyella_diff_viewer_demo_files "app"
    diff_viewer_demo/main.cpp)
add_executable(diff_viewer_demo WIN32 ${cyella_diff_viewer_demo_files})
target_link_libraries(diff_viewer_demo PRIVATE cyella_lib)
cyella_disable_automatic_windows_manifest(diff_viewer_demo)
cyella_copy_diff_viewer_assets(diff_viewer_demo)

cyella_set_ide_folder("Tests/manual" webview_file_probe diff_viewer_demo)
