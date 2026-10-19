// luil 웹뷰가 file:// 문서를 열고 양방향 메시지를 주고받는지 확인하는 수동 점검 프로그램이다.
//
// 창을 하나 띄워 probe.html을 file:// 주소로 열고, 사건을 로그 파일에 적은 뒤 정해진 시간이 지나면
// 스스로 닫는다. 화면을 보지 않고 셸에서 결과 파일만 읽어 확인할 수 있게 하려는 것이다.
//
//   webview_file_probe.exe [--log=<경로>] [--seconds=<초>] [--payload-mib=<앱이 페이지로 보낼 MiB>]

#include "luil/luil.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    constexpr wchar_t window_class[] { L"Cyella.WebviewFileProbe.Window" };
    constexpr std::u8string_view webview_id { u8"probe" };

    struct metrics_intent
    {
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
    };

    struct close_intent
    {};

    struct webview_intent
    {
        luil::webview_event event {};
    };

    // 1x1 PNG다. <img>가 file:// 문서에서 file:// 그림을 읽는지 보는 데 쓴다.
    // 중괄호 목록은 줄을 나누면 닫는 중괄호가 홀로 서지 못해 스타일 검사에 걸리므로 문자열로 둔다.
    constexpr char tiny_png[] = "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d"
                                "\x49\x48\x44\x52\x00\x00\x00\x01\x00\x00\x00\x01"
                                "\x08\x06\x00\x00\x00\x1f\x15\xc4\x89\x00\x00\x00"
                                "\x0d\x49\x44\x41\x54\x78\xda\x63\xfc\xcf\xc0\x50"
                                "\x0f\x00\x04\x85\x01\x80\x84\xa9\x8c\x21\x00\x00"
                                "\x00\x00\x49\x45\x4e\x44\xae\x42\x60\x82";

    bool is_unreserved(const char8_t letter)
    {
        if ((letter >= u8'0' && letter <= u8'9') || (letter >= u8'a' && letter <= u8'z') || (letter >= u8'A' && letter <= u8'Z'))
            return true;
        return letter == u8'-' || letter == u8'_' || letter == u8'.' || letter == u8'~' || letter == u8'/' || letter == u8':';
    }

    std::u8string file_url(const std::filesystem::path& path)
    {
        std::u8string url { u8"file:///" };
        for (const char8_t letter : path.generic_u8string())
        {
            if (is_unreserved(letter))
            {
                url.push_back(letter);
                continue;
            }
            constexpr char8_t digits[] { u8"0123456789ABCDEF" };
            url.push_back(u8'%');
            url.push_back(digits[(letter >> 4) & 0x0F]);
            url.push_back(digits[letter & 0x0F]);
        }
        return url;
    }

    std::filesystem::path executable_directory()
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD length { GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())) };
        return std::filesystem::path { std::wstring { buffer.data(), length } }.parent_path();
    }

    void close_after(const int seconds)
    {
        std::this_thread::sleep_for(std::chrono::seconds { seconds });
        if (const HWND window { FindWindowW(window_class, nullptr) }; window != nullptr)
            PostMessageW(window, WM_CLOSE, 0, 0);
    }

    class probe_driver final : public luil::logic_driver
    {
    public:
        probe_driver(std::filesystem::path log_path, std::u8string page_url, const std::size_t payload_mebibytes)
            : log_ { log_path, std::ios::binary | std::ios::trunc }
            , page_url_ { std::move(page_url) }
            , payload_mebibytes_ { payload_mebibytes }
        {}

        void handle(luil::app_message message) override
        {
            if (message.get<close_intent>() != nullptr)
            {
                closed_.store(true);
                return;
            }
            if (const auto* const metrics { message.get<metrics_intent>() }; metrics != nullptr)
            {
                metrics_ = *metrics;
                return;
            }
            if (const auto* const web { message.get<webview_intent>() }; web != nullptr)
            {
                const luil::webview_event& event { web->event };
                const std::size_t preview { std::min<std::size_t>(event.message.size(), 160) };
                log(std::to_string(static_cast<int>(event.kind)) + " url=" + std::string { event.url.begin(), event.url.end() } + " error=" + std::string { event.error.begin(), event.error.end() }
                    + " message=" + std::string { event.message.begin(), event.message.begin() + static_cast<std::ptrdiff_t>(preview) });
                if (event.kind == luil::webview_event_kind::web_message_received && event.message.find(u8"\"ready\"") != std::u8string::npos)
                {
                    // 페이지가 듣기 시작한 뒤에야 보낸다. 2 MiB는 기본 상한(256 KiB)의 여덟 배다.
                    std::u8string payload { u8"{\"type\":\"ping\",\"seq\":1,\"payload\":\"" };
                    payload.append(payload_mebibytes_ * 1024 * 1024, u8'p');
                    payload += u8"\"}";
                    post_message_ = std::move(payload);
                    ++post_revision_;
                }
            }
        }

        [[nodiscard]] std::shared_ptr<const luil::ui_frame> make_frame() override
        {
            const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
            const float width { metrics_.width > 0.0f ? metrics_.width : 800.0f };
            const float height { metrics_.height > 0.0f ? metrics_.height : 500.0f };

            auto root { std::make_unique<luil::root_element>() };
            root->arrange({ { 0.0f, 0.0f, width, height }, scale });

            luil::webview_config placeholder {};
            placeholder.webview = std::u8string { webview_id };
            placeholder.name = u8"probe";
            placeholder.placeholder = u8"웹뷰가 서지 않았다";
            auto element { std::make_unique<luil::webview_element>(luil::ui_element_id { luil::ui_element_kind::webview, std::u8string { webview_id } }, std::move(placeholder)) };
            element->arrange({ { 0.0f, 0.0f, width, height }, scale });
            root->add(std::move(element));

            auto frame { std::make_shared<luil::ui_frame>() };
            frame->tree = std::make_shared<const luil::ui_tree>(std::move(root));

            luil::ui_webview view {};
            view.id = std::u8string { webview_id };
            wchar_t local[MAX_PATH] {};
            const DWORD length { GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) };
            view.user_data_folder = (std::filesystem::path { std::wstring { local, length } } / L"cyella-probe" / L"webview").u8string();
            view.policy.allowed_schemes = { u8"file" };
            view.policy.developer_tools_enabled = true;
            view.navigate_revision = 1;
            view.navigate_url = page_url_;
            view.post_revision = post_revision_;
            view.post_message = post_message_;
            view.on_event = [](luil::webview_event event) { return luil::app_message { webview_intent { std::move(event) } }; };
            frame->webviews.push_back(std::move(view));
            return frame;
        }

        [[nodiscard]] luil::app_message make_close_message() override
        {
            return luil::app_message { close_intent {} };
        }

        [[nodiscard]] bool shutdown_completed() const override
        {
            return closed_.load();
        }

    private:
        void log(const std::string& line)
        {
            log_ << line << "\n";
            log_.flush();
        }

        std::ofstream log_;
        std::u8string page_url_;
        std::size_t payload_mebibytes_ { 2 };
        metrics_intent metrics_ {};
        std::uint64_t post_revision_ { 0 };
        std::u8string post_message_ {};
        std::atomic<bool> closed_ { false };
    };

    class probe_delegate final : public luil::win32::window_delegate
    {
    public:
        [[nodiscard]] luil::app_message make_window_metrics_message(const float width, const float height, const float scale) override
        {
            return luil::app_message { metrics_intent { width, height, scale } };
        }
    };
} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    luil::win32::enable_per_monitor_dpi_awareness();
    const luil::win32::com_sta_scope com {};

    std::filesystem::path log_path { std::filesystem::temp_directory_path() / L"webview_file_probe.log" };
    int seconds { 8 };
    std::size_t payload_mebibytes { 2 };
    if (const auto arguments { luil::win32::command_line_arguments() }; arguments.has_value())
    {
        for (const std::u8string& argument : *arguments)
            if (argument.starts_with(u8"--log="))
                log_path = std::filesystem::path { std::u8string_view { argument }.substr(6) };
            else if (argument.starts_with(u8"--seconds="))
                seconds = std::stoi(std::string { argument.begin() + 10, argument.end() });
            else if (argument.starts_with(u8"--payload-mib="))
                payload_mebibytes = static_cast<std::size_t>(std::stoi(std::string { argument.begin() + 14, argument.end() }));
    }

    const std::filesystem::path image_path { std::filesystem::temp_directory_path() / L"웹뷰 probe 그림.png" };
    {
        std::ofstream image { image_path, std::ios::binary | std::ios::trunc };
        image.write(tiny_png, sizeof(tiny_png) - 1);
    }
    const std::u8string page_url { file_url(executable_directory() / L"probe.html") + u8"?image=" + file_url(image_path) };

    luil::win32::window_config config {};
    config.class_name = window_class;
    config.caption.title = u8"webview file probe";
    config.caption.minimize_tooltip = u8"Minimize";
    config.caption.maximize_tooltip = u8"Maximize or restore";
    config.caption.close_tooltip = u8"Close";
    config.initial_width = 800;
    config.initial_height = 500;

    probe_driver driver { log_path, page_url, payload_mebibytes };
    probe_delegate delegate {};
    luil::win32::window_environment environment {};
    environment.driver = &driver;
    environment.delegate = &delegate;

    // 정해진 시간 뒤 창을 닫는다. 자동 점검이 화면 조작 없이 끝나게 하려는 것이다.
    std::thread closer { close_after, seconds };
    closer.detach();

    return luil::win32::run_application_window(config, environment);
}
