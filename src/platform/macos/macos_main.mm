#include "infrastructure/json_window_placement.h"
#include "platform/macos/macos_resources.h"
#include "platform/macos/posix_file.h"
#include "platform/win32/cyella_delegate.h"
#include "platform/win32/cyella_driver.h"
#include "platform/win32/win32_app_settings_path.h"
#include "platform/win32/win32_locale.h"
#include "platform/win32/win32_settings_file_system.h"
#include "presentation/ui/app_symbol.h"
#include "presentation/ui/cyella_policy.h"

#include "luil/luil.h"
#include "luil/macos/macos_app.h"
#include "luil/ui/image_decode.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// cyella의 macOS 진입점이다 (Windows 판은 platform/win32/main.cpp). 메인 thread가 luil의 UI thread이고, run_application이
// 창이 닫혀 종료가 끝날 때까지 돈다. driver·policy·delegate는 Windows와 같은 객체다.
namespace {
    // 터미널에서 `Cyella.app/Contents/MacOS/Cyella .`처럼 준 상대 경로를 지금 폴더 기준의 절대 경로로 바꾼다. 바꾸지 못하면
    // 받은 값을 그대로 둔다.
    std::u8string absolute_argument_path(const std::u8string& argument)
    {
        const std::string text { cyella::macos::to_narrow(argument) };
        char resolved[PATH_MAX] {};
        if (::realpath(text.c_str(), resolved) == nullptr)
            return argument;
        return cyella::macos::to_u8(resolved);
    }

    struct launch_options
    {
        cyella::win32::cyella_driver_options driver {};
        bool smoke_test { false };
    };

    // 명령줄이다. 뜻은 Windows 판과 같다: 위치 인자 하나는 시작할 때 여는 저장소, `--settings=<경로>`는 설정 파일,
    // `--no-webview`는 텍스트 diff, `--ssh-dir=<경로>`는 SSH 폴더, `--open-url-log=<경로>`·`--hosting-no-proxy`는 검증용이다.
    // `--smoke-test`는 창을 보이지 않은 채 첫 frame을 그리고 끝낸다 (luil의 renderer smoke test).
    launch_options command_line_options(const int argc, const char* const* const argv)
    {
        launch_options options {};
        constexpr std::u8string_view settings_prefix { u8"--settings=" };
        constexpr std::u8string_view ssh_directory_prefix { u8"--ssh-dir=" };
        constexpr std::u8string_view open_url_log_prefix { u8"--open-url-log=" };
        const std::vector<std::u8string> arguments { luil::macos::command_line_arguments(argc, argv) };
        for (std::size_t index { 1 }; index < arguments.size(); ++index)
        {
            const std::u8string& argument { arguments[index] };
            if (argument.starts_with(settings_prefix))
                options.driver.settings_path = argument.substr(settings_prefix.size());
            else if (argument == u8"--no-webview")
                options.driver.force_text_diff = true;
            else if (argument.starts_with(ssh_directory_prefix))
                options.driver.ssh_directory = argument.substr(ssh_directory_prefix.size());
            else if (argument.starts_with(open_url_log_prefix))
                options.driver.open_url_log = argument.substr(open_url_log_prefix.size());
            else if (argument == u8"--hosting-no-proxy")
                options.driver.hosting_system_proxy = false;
            else if (argument == u8"--smoke-test")
                options.smoke_test = true;
            // Launch Services가 옛 macOS에서 붙이던 프로세스 일련 번호(`-psn_0_12345`)다.
            else if (argument.starts_with(u8"-psn_"))
                continue;
            else if (options.driver.initial_repository.empty())
                options.driver.initial_repository = absolute_argument_path(argument);
        }
        if (options.driver.settings_path.empty())
            options.driver.settings_path = cyella::win32::app_settings_file_path();
        return options;
    }

    // 실행 파일에 실은 SVG를 벡터 그림으로 읽는다. 읽지 못하면 빈 그림이라 그 자리는 글리프나 글자로 물러선다.
    luil::ui_image load_embedded_svg(const cyella::macos::embedded_resource resource)
    {
        const std::string_view bytes { cyella::macos::embedded_bytes(resource) };
        if (bytes.empty())
            return {};
        luil::image_decode_error error {};
        return luil::decode_svg_bytes(std::span { reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size() }, error);
    }
} // namespace

int main(const int argc, const char* const* const argv)
{
    // 화면 문구의 언어를 다른 무엇보다 먼저 정한다. driver가 만드는 문구와 작업 thread가 모두 이 카탈로그를 읽는다.
    launch_options launch { command_line_options(argc, argv) };
    cyella::win32::cyella_driver_options& options { launch.driver };
    const cyella::language_preference language { cyella::win32::read_language_preference(options.settings_path) };
    cyella::win32::install_ui_language(cyella::resolve_ui_language(language, cyella::win32::system_ui_language()));

    // 기본 UI 글꼴은 luil의 macOS 기본(시스템 서체)이다. 한글은 시스템의 글꼴 대체가 Apple SD Gothic Neo로 그린다.
    // 시작 페이지·환경 설정의 앱 심볼과 타이틀 그림이다. 제목 막대는 시스템의 것이라 캡션 element는 자리를 차지하지 않는다.
    cyella::ui::set_app_symbol(load_embedded_svg(cyella::macos::embedded_resource::app_symbol));
    cyella::ui::set_app_title(load_embedded_svg(cyella::macos::embedded_resource::app_title), load_embedded_svg(cyella::macos::embedded_resource::app_title_sea));

    luil::macos::application_config config {};
    config.title = u8"Cyella";
    config.initial_width = 1280;
    config.initial_height = 720;
    config.minimum_width = 480;
    config.minimum_height = 320;
    config.smoke_test = launch.smoke_test;
    // 지난 실행의 창 배치로 연다. macOS host는 창을 만들 때 배치를 받지 않으므로 첫 frame의 배치 요청으로 싣는다.
    // 어느 화면에도 걸치지 않는 자리면 luil이 주 화면 가운데로 옮긴다.
    options.window_placement = cyella::load_window_placement(*cyella::win32::make_settings_file_system(), options.settings_path);
    options.apply_window_placement_in_frame = options.window_placement.has_value();

    cyella::win32::cyella_driver driver { std::move(options) };
    cyella::ui::cyella_policy policy {};
    cyella::win32::cyella_delegate delegate { &driver.relay(), &driver };
    luil::macos::application_environment environment {};
    environment.driver = &driver;
    environment.policy = &policy;
    environment.delegate = &delegate;
    return luil::macos::run_application(config, environment);
}
