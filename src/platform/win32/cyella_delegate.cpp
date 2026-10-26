#include "platform/win32/cyella_delegate.h"

#include "application/app_messages.h"
#include "application/watch_messages.h"
#include "domain/text_catalog.h"
#include "platform/win32/cyella_driver.h"
#include "platform/win32/utf8.h"
#include "presentation/dialog_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui_intents.h"

#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cyella::win32 {
    // luil에는 호버 intent 훅이 없어 UI thread에서 최신 상호작용만 살핀다. 상태가 바뀔 때만
    // 메시지를 보내므로 이 타이머는 logic frame이나 그래프를 주기적으로 다시 짓지 않는다.
    class ref_hover_monitor
    {
    public:
        explicit ref_hover_monitor(luil::app_host& host)
            : host_ { host }
        {
            if (active_ == nullptr)
            {
                timer_ = SetTimer(nullptr, 0, 60, &on_timer);
                if (timer_ != 0)
                    active_ = this;
            }
        }

        ~ref_hover_monitor()
        {
            if (timer_ != 0)
                KillTimer(nullptr, timer_);
            if (active_ == this)
                active_ = nullptr;
        }

    private:
        static void CALLBACK on_timer(HWND, UINT, UINT_PTR, DWORD)
        {
            if (active_ != nullptr)
                active_->update();
        }

        void update()
        {
            const auto frame { host_.acquire_frame() };
            if (frame == nullptr || frame->tree == nullptr)
                return;
            const auto interaction { host_.acquire_interaction() };
            const auto now { std::chrono::steady_clock::now() };
            const bool list_open { std::any_of(frame->popups.begin(), frame->popups.end(), [](const luil::ui_popup& popup) { return popup.id == u8"ref-list"; }) };
            const bool source { interaction.hovered.kind == ui::kind_ref_overflow && interaction.hovered_surface.empty() && interaction.drag.has_value() == false };
            if (source)
            {
                last_inside_ = now;
                if (sent_ == interaction.hovered || (frame->popups.empty() == false && list_open == false) || interaction.hover_started_at.has_value() == false
                    || now - *interaction.hover_started_at < std::chrono::milliseconds { 180 })
                    return;
                const luil::ui_element* const element { frame->tree->find(interaction.hovered) };
                if (element == nullptr || element->visible() == false)
                    return;
                const luil::ui_action* const open { element->action(luil::ui_trigger::left_click) };
                if (open == nullptr)
                    return;
                for (const luil::input_action& action : (*open)({ .element = element->id() }))
                    if (const auto* const message { std::get_if<luil::app_message>(&action) }; message != nullptr)
                        host_.post_app_message(*message);
                sent_ = interaction.hovered;
                hover_open_ = true;
            }
            else if (interaction.hovered_surface == u8"ref-list")
                last_inside_ = now;
            else if (list_open == false)
            {
                sent_ = {};
                hover_open_ = false;
            }
            else if (hover_open_ && now - last_inside_ >= std::chrono::milliseconds { 400 })
            {
                host_.post_app_message(luil::app_message { close_ref_list_intent {} });
                hover_open_ = false;
            }
        }

        inline static ref_hover_monitor* active_ { nullptr };
        luil::app_host& host_;
        UINT_PTR timer_ { 0 };
        luil::ui_element_id sent_ {};
        std::chrono::steady_clock::time_point last_inside_ {};
        bool hover_open_ { false };
    };

    namespace {
        // 대화 상자 문구를 Windows 문자열로 바꾼다. 바꾸지 못하면 빈 문자열이다.
        [[nodiscard]] std::wstring wide_text(const std::u8string& text)
        {
            auto converted { utf8_to_utf16(text) };
            return converted.value.has_value() ? std::move(*converted.value) : std::wstring {};
        }

        // 고른 항목의 파일 시스템 경로를 UTF-8로 꺼낸다. 실패하면 빈 값이다.
        [[nodiscard]] std::optional<std::u8string> selected_path(IFileOpenDialog& dialog)
        {
            std::optional<std::u8string> chosen {};
            IShellItem* item { nullptr };
            if (SUCCEEDED(dialog.GetResult(&item)) && item != nullptr)
            {
                PWSTR path { nullptr };
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
                {
                    auto converted { utf16_to_utf8(path) };
                    if (converted.value.has_value())
                        chosen = std::move(*converted.value);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
            return chosen;
        }

        // 폴더 고르기 대화 상자다. 취소했거나 열지 못했으면 빈 값이다.
        [[nodiscard]] std::optional<std::u8string> choose_folder(const wchar_t* const title)
        {
            IFileOpenDialog* dialog { nullptr };
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || dialog == nullptr)
                return std::nullopt;
            std::optional<std::u8string> chosen {};
            FILEOPENDIALOGOPTIONS options { 0 };
            if (SUCCEEDED(dialog->GetOptions(&options)))
                static_cast<void>(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM));
            static_cast<void>(dialog->SetTitle(title));
            // 소유 창은 앞에 있는 cyella 창이다. 대화 상자가 그 뒤로 숨지 않는다.
            if (SUCCEEDED(dialog->Show(GetActiveWindow())))
                chosen = selected_path(*dialog);
            dialog->Release();
            return chosen;
        }

        // 실행 파일 고르기 대화 상자다 (`.exe`가 첫 필터이고 모든 파일도 고를 수 있다). 취소했거나 열지 못했으면 빈 값이다.
        [[nodiscard]] std::optional<std::u8string> choose_executable(const wchar_t* const title)
        {
            IFileOpenDialog* dialog { nullptr };
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || dialog == nullptr)
                return std::nullopt;
            std::optional<std::u8string> chosen {};
            FILEOPENDIALOGOPTIONS options { 0 };
            if (SUCCEEDED(dialog->GetOptions(&options)))
                static_cast<void>(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST));
            const std::wstring executable_files { wide_text(tr(u8"app.dialog.executable_files_filter")) };
            const std::wstring all_files { wide_text(tr(u8"app.dialog.all_files_filter")) };
            const COMDLG_FILTERSPEC filters[] { { executable_files.c_str(), L"*.exe;*.cmd;*.bat" }, { all_files.c_str(), L"*.*" } };
            static_cast<void>(dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters));
            static_cast<void>(dialog->SetTitle(title));
            if (SUCCEEDED(dialog->Show(GetActiveWindow())))
                chosen = selected_path(*dialog);
            dialog->Release();
            return chosen;
        }
    } // namespace

    cyella_delegate::cyella_delegate(operation_result_relay* relay, cyella_driver* driver) noexcept
        : relay_ { relay }
        , driver_ { driver }
    {}

    void cyella_delegate::on_started(luil::app_host& host)
    {
        // 묶이기 전에 도착한 결과가 있으면 bind가 한 번 깨운다.
        if (driver_ != nullptr)
            driver_->bind_host(&host);
        if (relay_ != nullptr)
            relay_->bind(host);
        // 창이 앞으로 돌아오면 선택된 탭을 다시 읽는다. relay에 넣으면 logic thread가 깨어난다.
        if (relay_ != nullptr && foreground_ == nullptr)
            foreground_ = foreground_monitor::start([relay = relay_] { relay->post(luil::app_message { window_focus_intent {} }); });
        ref_hover_ = std::make_unique<ref_hover_monitor>(host);
    }

    cyella_delegate::~cyella_delegate() = default;

    void cyella_delegate::execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command)
    {
        // 환경 설정의 파일 고르기다. 결과는 `settings_file_chosen_intent`로 logic thread에 돌려준다.
        const auto command_id { static_cast<ui_command_id>(command.command) };
        if (command_id == ui_command_id::choose_git_executable || command_id == ui_command_id::choose_tool_executable)
        {
            const bool for_git { command_id == ui_command_id::choose_git_executable };
            const std::wstring title { wide_text(for_git ? tr(u8"app.dialog.choose_git_executable") : tr(u8"app.dialog.choose_diff_tool_executable")) };
            if (std::optional<std::u8string> path { choose_executable(title.c_str()) }; path.has_value())
                host.post_app_message(luil::app_message { settings_file_chosen_intent { for_git ? file_purpose::git_executable : file_purpose::tool_executable, std::move(*path) } });
            return;
        }

        folder_purpose purpose { folder_purpose::open };
        std::u8string title {};
        switch (static_cast<ui_command_id>(command.command))
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
        if (std::optional<std::u8string> path { choose_folder(wide_text(title).c_str()) }; path.has_value())
            host.post_app_message(luil::app_message { folder_chosen_intent { purpose, std::move(*path), purpose == folder_purpose::locate ? command.argument : std::u8string {} } });
    }

    luil::app_message cyella_delegate::make_window_metrics_message(const float width, const float height, const float scale)
    {
        return luil::app_message { window_metrics_intent { width, height, scale } };
    }

    luil::app_message cyella_delegate::make_window_placement_message(const luil::window_placement& placement)
    {
        // 전체 화면이면 사각형은 나왔을 때 돌아갈 자리다. cyella는 전체 화면으로 열지 않으므로 그 자리만 적는다.
        const saved_window_placement saved { .x = placement.x, .y = placement.y, .width = placement.width, .height = placement.height, .maximized = placement.maximized };
        return luil::app_message { window_placement_intent { saved } };
    }
} // namespace cyella::win32
