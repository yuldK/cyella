#include "platform/win32/cyella_delegate.h"
#include "platform/win32/cyella_driver.h"
#include "platform/win32/resources/resource_ids.h"
#include "platform/win32/utf8.h"
#include "platform/win32/win32_app_settings_path.h"
#include "presentation/ui/app_symbol.h"
#include "presentation/ui/cyella_caption.h"
#include "presentation/ui/cyella_policy.h"

#include "luil/luil.h"
#include "luil/text/fonts.h"
#include "luil/ui/image_decode.h"

#include <windows.h>

#include <shellapi.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {
    // 명령줄이다. 위치 인자 하나는 시작할 때 여는 저장소이고, `--settings=<경로>`는 설정 파일을 바꾼다
    // (검증할 때 사용자의 `%APPDATA%\cyella\settings.json`을 건드리지 않으려는 것이다). `--no-webview`는 diff를
    // 웹뷰 없이 텍스트 대체 보기로만 보인다 (WebView2 Runtime이 없는 PC의 화면을 확인할 때 쓴다). `--open-url-log=<경로>`는 "브라우저에서 열기"가
    // 브라우저 대신 주소를 그 파일에 적게 하고, `--hosting-no-proxy`는 GitHub·GitLab 호출이 시스템 proxy를 따르지 않게 한다 (둘 다 가짜 서버로 검증할 때 쓴다).
    cyella::win32::cyella_driver_options command_line_options()
    {
        cyella::win32::cyella_driver_options options {};
        int count { 0 };
        LPWSTR* const arguments { ::CommandLineToArgvW(::GetCommandLineW(), &count) };
        if (arguments != nullptr)
        {
            constexpr std::u8string_view settings_prefix { u8"--settings=" };
            constexpr std::u8string_view ssh_directory_prefix { u8"--ssh-dir=" };
            constexpr std::u8string_view open_url_log_prefix { u8"--open-url-log=" };
            for (int index { 1 }; index < count; ++index)
            {
                const std::u8string argument { cyella::win32::utf16_to_utf8(arguments[index]).value.value_or(std::u8string {}) };
                if (argument.starts_with(settings_prefix))
                    options.settings_path = argument.substr(settings_prefix.size());
                else if (argument == u8"--no-webview")
                    options.force_text_diff = true;
                else if (argument.starts_with(ssh_directory_prefix))
                    options.ssh_directory = argument.substr(ssh_directory_prefix.size());
                else if (argument.starts_with(open_url_log_prefix))
                    options.open_url_log = argument.substr(open_url_log_prefix.size());
                else if (argument == u8"--hosting-no-proxy")
                    options.hosting_system_proxy = false;
                else if (options.initial_repository.empty())
                    options.initial_repository = argument;
            }
            ::LocalFree(arguments);
        }
        if (options.settings_path.empty())
            options.settings_path = cyella::win32::app_settings_file_path();
        return options;
    }

    // 실행 파일에 실은 SVG(앱 심볼, 타이틀)를 벡터 그림으로 읽는다. 없거나 읽지 못하면 빈 그림이라 그 자리는 글리프나 글자로 물러선다.
    luil::ui_image load_resource_svg(const int id)
    {
        const HMODULE module { ::GetModuleHandleW(nullptr) };
        const HRSRC resource { ::FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA) };
        if (resource == nullptr)
            return {};
        const HGLOBAL loaded { ::LoadResource(module, resource) };
        const DWORD size { ::SizeofResource(module, resource) };
        const void* const data { loaded != nullptr ? ::LockResource(loaded) : nullptr };
        if (data == nullptr || size == 0)
            return {};
        luil::image_decode_error error {};
        return luil::decode_svg_bytes(std::span { static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size) }, error);
    }
} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // DPI 인지는 창을 만들기 전에 한 번 켠다.
    luil::win32::enable_per_monitor_dpi_awareness();
    // COM STA는 TSF(텍스트 입력)·파일 dialog·접근성이 요구한다.
    const luil::win32::com_sta_scope com {};

    // 기본 UI 글꼴 우선순위다. 설치되어 있지 않으면 luil이 내장 기본으로 물러선다.
    luil::set_ui_typeface_families({ u8"Malgun Gothic" });
    // 캡션 값(`cyella_caption`)을 만들기 전에 앱 심볼과 타이틀 그림을 정한다.
    cyella::ui::set_app_symbol(load_resource_svg(IDR_CYELLA_SYMBOL));
    cyella::ui::set_app_title(load_resource_svg(IDR_CYELLA_TITLE), load_resource_svg(IDR_CYELLA_TITLE_SEA));

    // 창의 겉모습과 정책이다. 길이는 전부 논리 96 DPI 기준 px다.
    luil::win32::window_config config {};
    config.class_name = L"Cyella.Window";
    config.icon_resource = IDI_CYELLA;
    // caption 값은 한곳에서만 만든다. 비클라이언트 hit test와 tree의 caption
    // element가 같은 값을 읽어야 버튼 자리가 어긋나지 않는다.
    config.caption = cyella::ui::cyella_caption();
    config.initial_width = 1280;
    config.initial_height = 720;
    config.minimum_client_width = 480;
    config.minimum_client_height = 320;
    config.resize_border_thickness = 4;
    config.resize_corner_thickness = 10;

    // driver·policy·delegate는 창이 도는 동안 살아 있어야 한다. delegate는 driver의 relay를 host에
    // 묶고, policy는 텍스트 입력·휠·단축키를 앱 메시지로 옮긴다.
    cyella::win32::cyella_driver driver { command_line_options() };
    cyella::ui::cyella_policy policy {};
    cyella::win32::cyella_delegate delegate { &driver.relay(), &driver };
    luil::win32::window_environment environment {};
    environment.driver = &driver;
    environment.policy = &policy;
    environment.delegate = &delegate;

    // 메시지 루프가 끝날 때까지 여기서 돈다. 반환값이 프로세스 종료 코드다.
    return luil::win32::run_application_window(config, environment);
}
