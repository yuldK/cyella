#include "platform/win32/cyella_delegate.h"

#include "application/app_messages.h"
#include "application/watch_messages.h"
#include "platform/win32/cyella_driver.h"
#include "platform/win32/utf8.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui_intents.h"

#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace cyella::win32 {
    namespace {
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
            const COMDLG_FILTERSPEC filters[] { { L"실행 파일 (*.exe;*.cmd;*.bat)", L"*.exe;*.cmd;*.bat" }, { L"모든 파일", L"*.*" } };
            static_cast<void>(dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters));
            static_cast<void>(dialog->SetTitle(title));
            if (SUCCEEDED(dialog->Show(GetActiveWindow())))
                chosen = selected_path(*dialog);
            dialog->Release();
            return chosen;
        }
    } // namespace

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
    }

    void cyella_delegate::execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command)
    {
        // 환경 설정의 파일 고르기다. 결과는 `settings_file_chosen_intent`로 logic thread에 돌려준다.
        const auto command_id { static_cast<ui_command_id>(command.command) };
        if (command_id == ui_command_id::choose_git_executable || command_id == ui_command_id::choose_tool_executable)
        {
            const bool for_git { command_id == ui_command_id::choose_git_executable };
            if (std::optional<std::u8string> path { choose_executable(for_git ? L"git 실행 파일 선택" : L"외부 diff 도구 실행 파일 선택") }; path.has_value())
                host.post_app_message(luil::app_message { settings_file_chosen_intent { for_git ? file_purpose::git_executable : file_purpose::tool_executable, std::move(*path) } });
            return;
        }

        folder_purpose purpose { folder_purpose::open };
        const wchar_t* title { L"열 저장소 폴더 선택" };
        switch (static_cast<ui_command_id>(command.command))
        {
        case ui_command_id::open_folder:
            break;
        case ui_command_id::init_folder:
            purpose = folder_purpose::init;
            title = L"새 저장소를 만들 폴더 선택";
            break;
        case ui_command_id::clone_target_folder:
            purpose = folder_purpose::clone_target;
            title = L"Clone 받을 상위 폴더 선택";
            break;
        case ui_command_id::locate_folder:
            purpose = folder_purpose::locate;
            title = L"저장소의 새 위치 선택";
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
} // namespace cyella::win32
