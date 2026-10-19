#include "platform/win32/cyella_delegate.h"
#include "platform/win32/cyella_driver.h"
#include "presentation/ui/cyella_caption.h"

#include "luil/luil.h"
#include "luil/text/fonts.h"

#include <windows.h>

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // DPI 인지는 창을 만들기 전에 한 번 켠다.
    luil::win32::enable_per_monitor_dpi_awareness();
    // COM STA는 TSF(텍스트 입력)·파일 dialog·접근성이 요구한다.
    const luil::win32::com_sta_scope com {};

    // 기본 UI 글꼴 우선순위다. 설치되어 있지 않으면 luil이 내장 기본으로 물러선다.
    luil::set_ui_typeface_families({ u8"Malgun Gothic" });

    // 창의 겉모습과 정책이다. 길이는 전부 논리 96 DPI 기준 px다.
    luil::win32::window_config config {};
    config.class_name = L"Cyella.Window";
    // caption 값은 한곳에서만 만든다. 비클라이언트 hit test와 tree의 caption
    // element가 같은 값을 읽어야 버튼 자리가 어긋나지 않는다.
    config.caption = cyella::ui::cyella_caption();
    config.initial_width = 1280;
    config.initial_height = 720;
    config.minimum_client_width = 480;
    config.minimum_client_height = 320;
    config.resize_border_thickness = 4;
    config.resize_corner_thickness = 10;

    // driver와 delegate는 창이 도는 동안 살아 있어야 한다. policy는 텍스트 입력·휠·
    // 단축키가 생길 때 더하며 지금은 nullptr로 둔다.
    cyella::win32::cyella_driver driver {};
    cyella::win32::cyella_delegate delegate {};
    luil::win32::window_environment environment {};
    environment.driver = &driver;
    environment.delegate = &delegate;

    // 메시지 루프가 끝날 때까지 여기서 돈다. 반환값이 프로세스 종료 코드다.
    return luil::win32::run_application_window(config, environment);
}
