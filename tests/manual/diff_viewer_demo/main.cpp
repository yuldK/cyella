// diff 뷰어를 끝까지 이어 보는 수동 점검 프로그램이다. luil 창에 웹뷰 하나만 두고, 임시 저장소의 파일 diff를
// 읽어 보여 주며, 웹뷰에서 누른 stage·unstage·discard·외부 도구 열기가 실제 git에 닿는지 본다.
//
// 앱이 diff 뷰어를 프레임에 끼울 때와 같은 조각을 쓴다.
//  - `ui_frame::webviews`의 `ui_webview`(file:// 주소, 정책, post_message·post_revision)와 tree의 `webview_element`
//  - `diff_view_controller`: frame마다 `poll()`, 웹뷰 메시지는 `handle_web_message()`, 일은 `diff_view_effect`로 받아
//    worker에서 실행하고 결과를 앱 메시지로 돌려받는다.
//
//   diff_viewer_demo.exe [--repo=<경로>] [--path=<저장소 상대 경로>] [--view=unstaged|staged]
//                        [--layout=inline|split] [--theme=dark|light] [--untracked] [--keep]
//                        [--tool-ext=.png --tool-exe=<실행 파일> --tool-args=<템플릿>]
//                        [--log=<파일>] [--seconds=<초>]
//
// --repo가 없으면 임시 저장소를 만들고(`--keep`이 없으면 끝날 때 지운다), 여러 종류의 변경을 심어 둔다.

#include "application/app_messages.h"
#include "application/cancellation_registry.h"
#include "application/diff_view_controller.h"
#include "application/external_diff_opener.h"
#include "application/git_workspace.h"
#include "application/task_scheduler.h"
#include "infrastructure/diff_temp_directory.h"
#include "infrastructure/git_diff_gateway.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/operation_result_relay.h"
#include "platform/win32/win32_diff_tool_launcher.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_workspace_files.h"

#include "luil/luil.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {
    using namespace cyella;

    constexpr wchar_t window_class[] { L"Cyella.DiffViewerDemo.Window" };
    constexpr std::u8string_view webview_id { u8"diff" };

    // ------------------------------------------------------------------ 앱 메시지

    struct webview_intent
    {
        luil::webview_event event {};
    };

    struct sides_ready_message
    {
        std::uint64_t ticket { 0 };
        extracted_sides sides {};
    };

    struct external_done_message
    {
        external_open_result result {};
    };

    // ------------------------------------------------------------------ 명령줄

    struct demo_options
    {
        std::filesystem::path repository {};
        std::string path { "src/lane_layout.cpp" };
        diff_view view { diff_view::unstaged };
        bool untracked { false };
        diff_layout layout { diff_layout::inline_view };
        diff_theme_mode theme { diff_theme_mode::dark };
        bool keep { false };
        std::u8string tool_extension {};
        std::u8string tool_executable {};
        std::u8string tool_arguments {};
        std::filesystem::path log {};
        int seconds { 0 };
    };

    demo_options parse_options()
    {
        demo_options options {};
        const auto arguments { luil::win32::command_line_arguments() };
        if (arguments.has_value() == false)
            return options;
        for (const std::u8string& argument : *arguments)
        {
            const auto value_of = [&argument](const std::u8string_view prefix) -> std::optional<std::u8string> {
                if (argument.starts_with(prefix))
                    return argument.substr(prefix.size());
                return std::nullopt;
            };
            if (const auto value { value_of(u8"--repo=") }; value.has_value())
                options.repository = std::filesystem::path { *value };
            else if (const auto value_path { value_of(u8"--path=") }; value_path.has_value())
                options.path = std::string { value_path->begin(), value_path->end() };
            else if (const auto value_view { value_of(u8"--view=") }; value_view.has_value())
                options.view = *value_view == u8"staged" ? diff_view::staged : diff_view::unstaged;
            else if (const auto value_layout { value_of(u8"--layout=") }; value_layout.has_value())
                options.layout = *value_layout == u8"split" ? diff_layout::split : diff_layout::inline_view;
            else if (const auto value_theme { value_of(u8"--theme=") }; value_theme.has_value())
                options.theme = *value_theme == u8"light" ? diff_theme_mode::light : diff_theme_mode::dark;
            else if (argument == u8"--untracked")
                options.untracked = true;
            else if (argument == u8"--keep")
                options.keep = true;
            else if (const auto value_extension { value_of(u8"--tool-ext=") }; value_extension.has_value())
                options.tool_extension = *value_extension;
            else if (const auto value_executable { value_of(u8"--tool-exe=") }; value_executable.has_value())
                options.tool_executable = *value_executable;
            else if (const auto value_arguments { value_of(u8"--tool-args=") }; value_arguments.has_value())
                options.tool_arguments = *value_arguments;
            else if (const auto value_log { value_of(u8"--log=") }; value_log.has_value())
                options.log = std::filesystem::path { *value_log };
            else if (const auto value_seconds { value_of(u8"--seconds=") }; value_seconds.has_value())
                options.seconds = std::stoi(std::string { value_seconds->begin(), value_seconds->end() });
        }
        return options;
    }

    // ------------------------------------------------------------------ 꾸러미

    std::u8string to_u8(const std::string_view text)
    {
        return std::u8string { text.begin(), text.end() };
    }

    std::string to_text(const std::u8string_view text)
    {
        return std::string { text.begin(), text.end() };
    }

    // file:// 주소로 쓰는 경로의 percent-encoding은 프로토콜 헤더의 make_file_url이 한다.
    std::filesystem::path executable_directory()
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD length { GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())) };
        return std::filesystem::path { std::wstring { buffer.data(), length } }.parent_path();
    }

    std::u8string profile_directory()
    {
        wchar_t buffer[MAX_PATH] {};
        const DWORD length { GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH) };
        if (length == 0 || length >= MAX_PATH)
            return {};
        return (std::filesystem::path { std::wstring { buffer, length } } / L"cyella-diff-demo" / L"webview").u8string();
    }

    // 압축 없는(stored) zlib 조각으로 PNG를 쓴다. 이미지 비교를 눈으로 볼 만한 작은 그림이면 된다.
    std::array<std::uint32_t, 256> make_crc_table()
    {
        std::array<std::uint32_t, 256> made {};
        for (std::uint32_t index { 0 }; index < 256; ++index)
        {
            std::uint32_t value { index };
            for (int bit { 0 }; bit < 8; ++bit)
                value = (value & 1U) != 0 ? 0xEDB88320U ^ (value >> 1) : value >> 1;
            made[index] = value;
        }
        return made;
    }

    std::uint32_t crc32_of(const std::string& bytes)
    {
        static const std::array<std::uint32_t, 256> table = make_crc_table();
        std::uint32_t crc { 0xFFFFFFFFU };
        for (const char letter : bytes)
            crc = table[(crc ^ static_cast<unsigned char>(letter)) & 0xFFU] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFU;
    }

    void append_be32(std::string& target, const std::uint32_t value)
    {
        for (int shift { 24 }; shift >= 0; shift -= 8)
            target.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }

    void append_chunk(std::string& png, const std::string& type, const std::string& data)
    {
        append_be32(png, static_cast<std::uint32_t>(data.size()));
        const std::string body { type + data };
        png += body;
        append_be32(png, crc32_of(body));
    }

    std::string make_png(const std::uint32_t width, const std::uint32_t height, const bool warm)
    {
        std::string raw {};
        for (std::uint32_t y { 0 }; y < height; ++y)
        {
            raw.push_back('\0');
            for (std::uint32_t x { 0 }; x < width; ++x)
            {
                const auto red { static_cast<unsigned char>(warm ? 200 - x : 40 + x * 2 % 120) };
                const auto green { static_cast<unsigned char>(warm ? 90 + y : 110 + y) };
                const auto blue { static_cast<unsigned char>(warm ? 40 : 190 - y) };
                raw.push_back(static_cast<char>(red));
                raw.push_back(static_cast<char>(green));
                raw.push_back(static_cast<char>(blue));
                raw.push_back(static_cast<char>((x / 8 + y / 8) % 2 == 0 ? 255 : 200));
            }
        }
        std::string zlib { "\x78\x01" };
        std::uint32_t adler_a { 1 };
        std::uint32_t adler_b { 0 };
        for (const char letter : raw)
        {
            adler_a = (adler_a + static_cast<unsigned char>(letter)) % 65521U;
            adler_b = (adler_b + adler_a) % 65521U;
        }
        for (std::size_t offset { 0 }; offset < raw.size(); offset += 65535)
        {
            const std::size_t length { std::min<std::size_t>(65535, raw.size() - offset) };
            zlib.push_back(offset + length >= raw.size() ? '\x01' : '\x00');
            zlib.push_back(static_cast<char>(length & 0xFF));
            zlib.push_back(static_cast<char>((length >> 8) & 0xFF));
            zlib.push_back(static_cast<char>(~length & 0xFF));
            zlib.push_back(static_cast<char>((~length >> 8) & 0xFF));
            zlib.append(raw, offset, length);
        }
        append_be32(zlib, (adler_b << 16) | adler_a);

        std::string png { "\x89PNG\r\n\x1a\n", 8 };
        std::string header {};
        append_be32(header, width);
        append_be32(header, height);
        header += std::string { "\x08\x06\x00\x00\x00", 5 };
        append_chunk(png, "IHDR", header);
        append_chunk(png, "IDAT", zlib);
        append_chunk(png, "IEND", {});
        return png;
    }

    void write_file(const std::filesystem::path& path, const std::string_view content)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream { path, std::ios::binary | std::ios::trunc };
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    // ------------------------------------------------------------------ 구동기

    class demo_driver final : public luil::win32::logic_driver
    {
    public:
        explicit demo_driver(demo_options options)
            : options_ { std::move(options) }
            , controller_ { diff_view_options { diff_view_default_line_limit, true } }
        {}

        ~demo_driver() override
        {
            stop_workers();
        }

        void start() override
        {
            log("start");
            runner_ = win32::make_process_runner();
            files_ = win32::make_workspace_files();
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool_ = resolve_git_tool({}, win32::current_git_tool_environment(), *runner_, *probe, {});
            if (tool_.usable() == false)
            {
                log("git를 찾지 못했다");
                return;
            }

            if (options_.repository.empty())
            {
                made_repository_ = true;
                options_.repository = std::filesystem::temp_directory_path() / ("cyella-diff-demo-" + std::to_string(GetCurrentProcessId()));
                try
                {
                    seed_repository();
                }
                catch (const std::exception& error)
                {
                    // logic thread의 예외는 삼켜져 화면만 멎으므로 까닭을 남긴다.
                    log(std::string { "seed 실패: " } + error.what());
                    return;
                }
            }
            log("repository=" + options_.repository.string());

            executor_ = std::make_unique<git_operation_runner>(*runner_, tool_.executable, *files_);
            scheduler_ = std::make_unique<task_scheduler>();
            task_scheduler* const scheduler { scheduler_.get() };
            win32::operation_result_relay* const relay { &relay_ };
            workspace_ = std::make_unique<git_workspace>(
                *executor_, [scheduler](operation_request request) { return scheduler->submit(std::move(request)); },
                [relay](operation_event event) { relay->post(luil::app_message { std::move(event) }); }, &registry_);

            temp_ = std::make_unique<diff_temp_directory>();
            gateway_ = std::make_unique<git_diff_gateway>(*runner_, tool_.executable, *temp_);
            launcher_ = win32::make_diff_tool_launcher();
            opener_ = std::make_unique<external_diff_opener>(*gateway_, *launcher_);

            controller_.set_layout(options_.layout);
            diff_theme theme {};
            theme.mode = options_.theme;
            controller_.set_theme(theme);

            repository_id_ = workspace_->open_repository(options_.repository.u8string());
            request_diff();
        }

        void handle(luil::app_message message) override
        {
            drain_relay();
            if (message.get<close_intent>() != nullptr)
            {
                closed_.store(true);
                return;
            }
            if (const auto* const metrics { message.get<window_metrics_intent>() }; metrics != nullptr)
            {
                metrics_ = *metrics;
                return;
            }
            if (const auto* const web { message.get<webview_intent>() }; web != nullptr)
                handle_webview_event(web->event);
        }

        [[nodiscard]] std::shared_ptr<const luil::ui_frame> make_frame() override
        {
            const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
            const float width { metrics_.width > 0.0f ? metrics_.width : 1100.0f };
            const float height { metrics_.height > 0.0f ? metrics_.height : 700.0f };

            auto root { std::make_unique<luil::root_element>() };
            root->arrange({ { 0.0f, 0.0f, width, height }, scale });

            luil::caption_config caption_config {};
            caption_config.title = u8"cyella diff demo";
            caption_config.minimize_tooltip = u8"Minimize";
            caption_config.maximize_tooltip = u8"Maximize or restore";
            caption_config.close_tooltip = u8"Close";
            const float caption_height { luil::caption_element::height_for(caption_config) * scale };
            auto caption { std::make_unique<luil::caption_element>(caption_config) };
            caption->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
            root->add(std::move(caption));

            luil::webview_config placeholder {};
            placeholder.webview = std::u8string { webview_id };
            placeholder.name = u8"diff 뷰어";
            placeholder.placeholder = u8"WebView2 런타임이 없어 diff를 그릴 수 없다";
            auto element { std::make_unique<luil::webview_element>(luil::ui_element_id { luil::ui_element_kind::webview, std::u8string { webview_id } }, std::move(placeholder)) };
            element->arrange({ { 0.0f, caption_height, width, height - caption_height }, scale });
            root->add(std::move(element));

            auto frame { std::make_shared<luil::ui_frame>() };
            frame->tree = std::make_shared<const luil::ui_tree>(std::move(root));

            // frame마다 한 번 poll한다. 이번 frame까지 쌓인 변경이 메시지 하나로 묶인다.
            static_cast<void>(controller_.poll());

            luil::ui_webview view {};
            view.id = std::u8string { webview_id };
            view.user_data_folder = profile_directory();
            // 뷰어 파일은 실행 파일 옆 `diff-viewer` 폴더에서 file:// 로 연다. 허용하는 주소는 file 하나뿐이다.
            view.policy.allowed_schemes = { u8"file" };
            // 페이지가 보내는 가장 큰 메시지는 copy의 텍스트(최대 4 MiB)다. luil은 앱이 페이지로 보내는 쪽에는
            // 상한을 걸지 않는다.
            view.policy.maximum_message_bytes = diff_view_maximum_message_bytes;
            view.policy.developer_tools_enabled = true;
            view.navigate_revision = 1;
            view.navigate_url = to_u8(make_file_url((executable_directory() / L"diff-viewer" / L"index.html").u8string()));
            view.post_revision = controller_.post_revision();
            view.post_message = to_u8(controller_.latest_message());
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

        void cancel() noexcept override
        {
            registry_.cancel_all();
        }

        void stop_workers() override
        {
            registry_.cancel_all();
            if (scheduler_ != nullptr)
                scheduler_->shutdown();
            for (std::thread& worker : workers_)
                if (worker.joinable())
                    worker.join();
            workers_.clear();
            relay_.unbind();
            if (made_repository_ && options_.keep == false && options_.repository.empty() == false)
            {
                // 객체 파일은 읽기 전용이라 속성을 풀어야 지워진다.
                std::error_code ignored {};
                for (const auto& entry : std::filesystem::recursive_directory_iterator { options_.repository, ignored })
                    std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ignored);
                std::filesystem::remove_all(options_.repository, ignored);
                options_.repository.clear();
            }
        }

        [[nodiscard]] win32::operation_result_relay& relay() noexcept
        {
            return relay_;
        }

        void bind_host(luil::app_host* host) noexcept
        {
            host_.store(host);
        }

    private:
        void log(const std::string& line)
        {
            if (options_.log.empty())
                return;
            const std::lock_guard lock { log_mutex_ };
            std::ofstream stream { options_.log, std::ios::app };
            stream << line << "\n";
        }

        void git(const std::vector<std::u8string>& arguments, const std::string& input = {})
        {
            git_request_options request_options {};
            request_options.standard_input = input;
            const process_request request { make_git_process_request(tool_.executable, options_.repository.u8string(), arguments, git_command_class::local_write, request_options) };
            const process_result result { runner_->run(request, nullptr, {}) };
            if (result.succeeded() == false)
                log("git 실패: " + to_text(result.masked_command_line));
        }

        // 여러 종류의 변경을 심은 임시 저장소를 만든다.
        void seed_repository()
        {
            const std::filesystem::path root { options_.repository };
            std::filesystem::create_directories(root);

            std::string lane {};
            for (int line { 1 }; line <= 60; ++line)
                lane += "    int value_" + std::to_string(line) + " { " + std::to_string(line) + " }; // line " + std::to_string(line) + "\n";
            write_file(root / u8"src" / u8"lane_layout.cpp", "#include <vector>\nnamespace cyella {\nvoid f()\n{\n" + lane + "}\n}\n");
            write_file(root / u8"docs" / u8"한글 문서.md", "# 문서\n\n하나\n둘\n셋\n");
            write_file(root / "staged.txt", "a\nb\nc\nd\ne\n");
            write_file(root / "gone.txt", "one\ntwo\nthree\n");
            write_file(root / "art" / "logo.png", make_png(96, 64, false));
            std::string binary {};
            for (int index { 0 }; index < 256; ++index)
                binary.push_back(static_cast<char>(index));
            write_file(root / "data.bin", binary);

            git({ u8"init", u8"-q", u8"-b", u8"main" });
            git({ u8"config", u8"user.name", u8"demo" });
            git({ u8"config", u8"user.email", u8"demo@example.com" });
            git({ u8"config", u8"core.autocrlf", u8"false" });
            git({ u8"add", u8"-A" });
            git({ u8"commit", u8"-q", u8"-m", u8"base" });

            // 작업 트리 변경: 세 곳을 고친 파일(hunk 셋), 한글 이름 문서, 삭제, 이미지·바이너리 교체, 추적하지 않는 파일.
            std::string changed {};
            for (int line { 1 }; line <= 60; ++line)
                if (line == 4)
                    changed += "    int value_4 { 40 }; // changed four\n";
                else if (line == 5)
                    changed += "    int value_5 { 5 }; // line 5\n    int inserted_after_five { 0 };\n";
                else if (line == 30)
                    changed += "    int value_30 { 300 }; // thirty\n";
                else if (line == 55)
                    changed += "    int value_55 { 55 + 1 }; // line 55\n";
                else
                    changed += "    int value_" + std::to_string(line) + " { " + std::to_string(line) + " }; // line " + std::to_string(line) + "\n";
            write_file(root / u8"src" / u8"lane_layout.cpp", "#include <vector>\nnamespace cyella {\nvoid f()\n{\n" + changed + "}\n}\n");
            write_file(root / u8"docs" / u8"한글 문서.md", "# 문서\n\n하나\n둘 (고침)\n셋\n넷\n");
            write_file(root / "art" / "logo.png", make_png(96, 64, true));
            binary.push_back('\x7f');
            binary[10] = '\x00';
            write_file(root / "data.bin", binary);
            write_file(root / "new_file.txt", "untracked one\nuntracked two\nuntracked three\n");
            std::filesystem::remove(root / "gone.txt");
            write_file(root / "staged.txt", "a\nB\nc\nd\nE\n");
            git({ u8"add", u8"staged.txt" });
        }

        void request_diff()
        {
            diff_file_request request {};
            request.path = options_.path;
            request.view = options_.view;
            request.untracked = options_.untracked;
            const operation_id id { workspace_->submit(repository_id_, request) };
            log("diff_file 요청 op=" + std::to_string(id) + " path=" + options_.path);
        }

        void drain_relay()
        {
            relay_.drain([this](luil::app_message drained) {
                if (const auto* const event { drained.get<operation_event>() }; event != nullptr)
                {
                    if (workspace_ != nullptr)
                        workspace_->apply(*event);
                    if (std::holds_alternative<file_diff_loaded>(event->payload))
                        diff_ready_ = true;
                    if (const auto* const finished { std::get_if<operation_finished>(&event->payload) }; finished != nullptr && event->id == apply_operation_)
                        finish_apply(*finished);
                }
                else if (const auto* const ready { drained.get<sides_ready_message>() }; ready != nullptr)
                {
                    log(std::string { "sides ready ticket=" } + std::to_string(ready->ticket) + " ok=" + (ready->sides.ok ? "1" : "0"));
                    controller_.sides_prepared(ready->ticket, ready->sides);
                }
                else if (const auto* const done { drained.get<external_done_message>() }; done != nullptr)
                {
                    log("external open status=" + std::to_string(static_cast<int>(done->result.status)) + " tool=" + to_text(done->result.tool) + " detail=" + done->result.detail);
                }
            });

            if (diff_ready_)
            {
                diff_ready_ = false;
                const repository_session* const session { workspace_ != nullptr ? workspace_->find_session(repository_id_) : nullptr };
                if (session != nullptr && session->selected_diff().has_value())
                {
                    log("diff 표시 files=" + std::to_string(session->selected_diff()->files.size()));
                    run_effects(controller_.show(*session->selected_diff(), options_.repository.u8string()));
                }
            }
        }

        void finish_apply(const operation_finished& finished)
        {
            apply_operation_ = 0;
            log(std::string { "apply_patch 끝 state=" } + std::to_string(static_cast<int>(finished.state)));
            if (finished.state == operation_state::succeeded)
            {
                // 적용에 성공했으면 같은 파일의 diff를 다시 읽어 보인다.
                request_diff();
            }
            else
            {
                controller_.action_failed();
            }
        }

        void handle_webview_event(const luil::webview_event& event)
        {
            switch (event.kind)
            {
            case luil::webview_event_kind::navigation_started:
                controller_.page_reloading();
                log("navigation_started " + to_text(event.url));
                break;
            case luil::webview_event_kind::navigation_completed:
                log("navigation_completed");
                break;
            case luil::webview_event_kind::navigation_failed:
            case luil::webview_event_kind::navigation_blocked:
            case luil::webview_event_kind::events_dropped:
                log("webview error " + to_text(event.error) + " " + to_text(event.url));
                break;
            case luil::webview_event_kind::web_message_received: {
                const web_message_result result { controller_.handle_web_message(to_text(event.message)) };
                log(std::string { "web message " } + (result.accepted ? "accepted " : "rejected ") + result.reason + " " + to_text(event.message).substr(0, 120));
                run_effects(result.effects);
                break;
            }
            default:
                break;
            }
        }

        void run_effects(const std::vector<diff_view_effect>& effects)
        {
            for (const diff_view_effect& effect : effects)
            {
                if (const auto* const apply { std::get_if<apply_patch_effect>(&effect) }; apply != nullptr)
                {
                    apply_operation_ = workspace_->submit(repository_id_, apply->request);
                    log("apply_patch 요청 op=" + std::to_string(apply_operation_) + " cached=" + (apply->request.cached ? "1" : "0") + " reverse=" + (apply->request.reverse ? "1" : "0"));
                }
                else if (const auto* const prepare { std::get_if<prepare_sides_effect>(&effect) }; prepare != nullptr)
                {
                    const prepare_sides_effect copy { *prepare };
                    workers_.emplace_back([this, copy] { relay_.post(luil::app_message { sides_ready_message { copy.ticket, gateway_->extract(copy.plan, copy.write_files) } }); });
                }
                else if (const auto* const external { std::get_if<open_external_effect>(&effect) }; external != nullptr)
                {
                    external_open_request request {};
                    request.plan = external->plan;
                    request.git_executable = tool_.executable;
                    if (options_.tool_extension.empty() == false)
                    {
                        external_diff_tool entry {};
                        entry.extensions.push_back(options_.tool_extension);
                        entry.executable = options_.tool_executable;
                        entry.arguments = options_.tool_arguments;
                        request.tools.push_back(std::move(entry));
                    }
                    workers_.emplace_back([this, request] { relay_.post(luil::app_message { external_done_message { opener_->open(request) } }); });
                }
                else if (const auto* const copy { std::get_if<copy_text_effect>(&effect) }; copy != nullptr)
                {
                    log("copy " + std::to_string(copy->text.size()) + " bytes");
                    if (luil::app_host* const host { host_.load() }; host != nullptr)
                        host->post_clipboard_request(luil::clipboard_copy_request { to_u8(copy->text) });
                }
            }
        }

        // 멤버 순서가 파괴 순서의 반대다.
        demo_options options_;
        win32::operation_result_relay relay_ {};
        cancellation_registry registry_ {};
        std::unique_ptr<process_runner> runner_ {};
        std::unique_ptr<workspace_files> files_ {};
        git_tool_info tool_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        std::unique_ptr<git_workspace> workspace_ {};
        std::unique_ptr<task_scheduler> scheduler_ {};
        std::unique_ptr<diff_temp_directory> temp_ {};
        std::unique_ptr<git_diff_gateway> gateway_ {};
        std::unique_ptr<diff_tool_launcher> launcher_ {};
        std::unique_ptr<external_diff_opener> opener_ {};
        diff_view_controller controller_;
        std::vector<std::thread> workers_ {};
        std::string repository_id_ {};
        window_metrics_intent metrics_ {};
        operation_id apply_operation_ { 0 };
        bool diff_ready_ { false };
        bool made_repository_ { false };
        std::mutex log_mutex_ {};
        std::atomic<luil::app_host*> host_ { nullptr };
        std::atomic<bool> closed_ { false };
    };

    class demo_delegate final : public luil::win32::window_delegate
    {
    public:
        explicit demo_delegate(demo_driver& driver) noexcept
            : driver_ { driver }
        {}

        void on_started(luil::app_host& host) override
        {
            driver_.relay().bind(host);
            driver_.bind_host(&host);
        }

        [[nodiscard]] luil::app_message make_window_metrics_message(const float width, const float height, const float scale) override
        {
            return luil::app_message { window_metrics_intent { width, height, scale } };
        }

    private:
        demo_driver& driver_;
    };

    void close_after(const int seconds)
    {
        std::this_thread::sleep_for(std::chrono::seconds { seconds });
        if (const HWND window { FindWindowW(window_class, nullptr) }; window != nullptr)
            PostMessageW(window, WM_CLOSE, 0, 0);
    }
} // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    luil::win32::enable_per_monitor_dpi_awareness();
    const luil::win32::com_sta_scope com {};

    demo_options options { parse_options() };
    const int seconds { options.seconds };

    luil::win32::window_config config {};
    config.class_name = window_class;
    config.caption.title = u8"cyella diff demo";
    config.caption.minimize_tooltip = u8"Minimize";
    config.caption.maximize_tooltip = u8"Maximize or restore";
    config.caption.close_tooltip = u8"Close";
    config.initial_width = 1100;
    config.initial_height = 700;

    demo_driver driver { std::move(options) };
    demo_delegate delegate { driver };
    luil::win32::window_environment environment {};
    environment.driver = &driver;
    environment.delegate = &delegate;

    if (seconds > 0)
    {
        std::thread closer { close_after, seconds };
        closer.detach();
    }
    return luil::win32::run_application_window(config, environment);
}
