#include "platform/win32/cyella_delegate.h"

#include "application/app_messages.h"
#include "application/watch_messages.h"
#include "domain/text_catalog.h"
#include "platform/win32/cyella_driver.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui_intents.h"

#import <AppKit/AppKit.h>

#include <optional>
#include <string>
#include <utility>

// `platform/win32/cyella_delegate.h`의 macOS 구현이다. 창 사건을 앱 메시지로 바꾸는 부분은 Windows 판과 같고, OS 대화 상자만
// NSOpenPanel이다. luil의 macOS host는 메인 thread에서 delegate를 부르므로 패널을 모달로 바로 연다.
namespace cyella::win32 {
    namespace {
        NSString* ns_string(const std::u8string& text)
        {
            NSString* const value { [[NSString alloc] initWithBytes:text.data() length:text.size() encoding:NSUTF8StringEncoding] };
            return value != nil ? value : @"";
        }

        std::optional<std::u8string> chosen_path(NSOpenPanel* const panel)
        {
            NSURL* const url { panel.URLs.firstObject };
            if (url == nil || url.isFileURL == NO)
                return std::nullopt;
            const char* const path { url.fileSystemRepresentation };
            if (path == nullptr)
                return std::nullopt;
            return std::u8string { reinterpret_cast<const char8_t*>(path) };
        }

        // 폴더 고르기다. 취소했거나 열지 못했으면 빈 값이다.
        std::optional<std::u8string> choose_folder(const std::u8string& title)
        {
            @autoreleasepool
            {
                NSOpenPanel* const panel { [NSOpenPanel openPanel] };
                panel.canChooseDirectories = YES;
                panel.canChooseFiles = NO;
                panel.allowsMultipleSelection = NO;
                panel.canCreateDirectories = YES;
                panel.message = ns_string(title);
                if ([panel runModal] != NSModalResponseOK)
                    return std::nullopt;
                return chosen_path(panel);
            }
        }

        // 실행 파일 고르기다. 앱 번들(`.app`)은 열어 보지 않고 파일처럼 다룬다 — 실행 파일은 번들 안의 것을 직접 고른다.
        std::optional<std::u8string> choose_executable(const std::u8string& title)
        {
            @autoreleasepool
            {
                NSOpenPanel* const panel { [NSOpenPanel openPanel] };
                panel.canChooseDirectories = NO;
                panel.canChooseFiles = YES;
                panel.allowsMultipleSelection = NO;
                panel.treatsFilePackagesAsDirectories = YES;
                panel.message = ns_string(title);
                if ([panel runModal] != NSModalResponseOK)
                    return std::nullopt;
                return chosen_path(panel);
            }
        }
    } // namespace

    cyella_delegate::cyella_delegate(operation_result_relay* relay, cyella_driver* driver) noexcept
        : relay_ { relay }
        , driver_ { driver }
    {}

    void cyella_delegate::on_started(luil::app_host& host)
    {
        if (driver_ != nullptr)
            driver_->bind_host(&host);
        if (relay_ != nullptr)
            relay_->bind(host);
        // 다른 앱에서 돌아오면 선택된 탭을 다시 읽는다.
        if (relay_ != nullptr && foreground_ == nullptr)
            foreground_ = foreground_monitor::start([relay = relay_] { relay->post(luil::app_message { window_focus_intent {} }); });
    }

    cyella_delegate::~cyella_delegate() = default;

    void cyella_delegate::execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command)
    {
        const auto command_id { static_cast<ui_command_id>(command.command) };
        if (command_id == ui_command_id::choose_git_executable || command_id == ui_command_id::choose_tool_executable)
        {
            const bool for_git { command_id == ui_command_id::choose_git_executable };
            if (std::optional<std::u8string> path { choose_executable(for_git ? tr(u8"app.dialog.choose_git_executable") : tr(u8"app.dialog.choose_diff_tool_executable")) }; path.has_value())
                host.post_app_message(luil::app_message { settings_file_chosen_intent { for_git ? file_purpose::git_executable : file_purpose::tool_executable, std::move(*path) } });
            return;
        }

        if (command_id == ui_command_id::choose_suggestion_executable)
        {
            if (std::optional<std::u8string> path { choose_executable(tr(u8"app.dialog.choose_suggestion_executable")) }; path.has_value())
                host.post_app_message(luil::app_message { settings_file_chosen_intent { file_purpose::suggestion_executable, std::move(*path) } });
            return;
        }

        folder_purpose purpose { folder_purpose::open };
        std::u8string title {};
        switch (command_id)
        {
        case ui_command_id::open_folder:
            title = tr(u8"app.dialog.choose_repository_folder");
            break;
        case ui_command_id::init_folder:
            purpose = folder_purpose::init;
            title = tr(u8"app.dialog.choose_init_folder");
            break;
        case ui_command_id::clone_target_folder:
            purpose = folder_purpose::clone_target;
            title = tr(u8"app.dialog.choose_clone_parent_folder");
            break;
        case ui_command_id::locate_folder:
            purpose = folder_purpose::locate;
            title = tr(u8"app.dialog.choose_repository_new_location");
            break;
        default:
            return;
        }
        if (std::optional<std::u8string> path { choose_folder(title) }; path.has_value())
            host.post_app_message(luil::app_message { folder_chosen_intent { purpose, std::move(*path), purpose == folder_purpose::locate ? command.argument : std::u8string {} } });
    }

    luil::app_message cyella_delegate::make_window_metrics_message(const float width, const float height, const float scale)
    {
        return luil::app_message { window_metrics_intent { width, height, scale } };
    }

    luil::app_message cyella_delegate::make_window_placement_message(const luil::window_placement& placement)
    {
        const saved_window_placement saved { .x = placement.x, .y = placement.y, .width = placement.width, .height = placement.height, .maximized = placement.maximized };
        return luil::app_message { window_placement_intent { saved } };
    }

    luil::app_message cyella_delegate::make_system_appearance_message(const luil::system_appearance& appearance)
    {
        static_cast<void>(appearance);
        return luil::app_message { system_appearance_intent {} };
    }
} // namespace cyella::win32
