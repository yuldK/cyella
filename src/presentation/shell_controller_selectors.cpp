#include "domain/repository_path.h"
#include "domain/text_catalog.h"
#include "presentation/selector_intents.h"
#include "presentation/selector_model.h"
#include "presentation/shell_controller.h"

#include <charconv>
#include <chrono>
#include <string>
#include <utility>

namespace cyella {
    namespace {
        // 바깥 누름이 목록을 닫은 직후 같은 상자를 누른 것은 "다시 열기"가 아니라 "닫기"를 누른 것이다. 누름과 뗌 사이의 시간이다.
        constexpr std::chrono::milliseconds toggle_window { 400 };

        std::optional<std::u8string> key_argument(const std::u8string& key, const std::u8string_view prefix)
        {
            if (key.starts_with(prefix) == false || key.size() == prefix.size())
                return std::nullopt;
            return key.substr(prefix.size());
        }

        std::string to_text(const std::u8string& text)
        {
            return std::string { text.begin(), text.end() };
        }
    } // namespace

    void shell_controller::close_selector(const bool dismissed)
    {
        if (ui_.selector.has_value() == false)
            return;
        if (dismissed)
            selector_dismissed_ = std::pair { ui_.selector->kind, std::chrono::steady_clock::now() };
        ui_.selector.reset();
    }

    void shell_controller::open_selector(const open_selector_intent& intent)
    {
        // 열려 있던 같은 상자를 다시 눌렀거나, 방금 바깥 누름으로 닫힌 상자를 누르는 중이다: 닫는다.
        if (ui_.selector.has_value() && ui_.selector->kind == intent.kind)
        {
            close_selector(false);
            return;
        }
        if (selector_dismissed_.has_value() && selector_dismissed_->first == intent.kind && std::chrono::steady_clock::now() - selector_dismissed_->second < toggle_window)
        {
            selector_dismissed_.reset();
            return;
        }
        selector_dismissed_.reset();
        if (intent.kind == selector_kind::branch && selected_session() == nullptr)
            return;
        ui_.menu.reset();
        ui_.selector = selector_state { .kind = intent.kind, .x = intent.x, .y = intent.y, .width = intent.width, .filter = {} };
    }

    void shell_controller::select_from_selector(const std::u8string& key)
    {
        close_selector(false);
        if (const std::optional<std::u8string> tab { key_argument(key, selector_tab_key_prefix) }; tab.has_value())
        {
            repository_tab_id id { 0 };
            const std::string digits { to_text(*tab) };
            if (std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc {})
                return;
            if (const std::optional<std::size_t> index { tabs_.index_of(id) }; index.has_value())
                select_tab_index(*index);
            return;
        }
        if (const std::optional<std::u8string> path { key_argument(key, selector_recent_key_prefix) }; path.has_value())
        {
            if (repository_exists(*path))
                open_repository(*path);
            else
                show_notice(tr(u8"selector.recent_missing", { { u8"path", *path } }), true);
            return;
        }
        if (const std::optional<std::u8string> app { key_argument(key, selector_repository_app_key_prefix) }; app.has_value())
        {
            std::size_t index { 0 };
            const std::string digits { to_text(*app) };
            if (std::from_chars(digits.data(), digits.data() + digits.size(), index).ec == std::errc {})
                open_repository_in_app(index);
            return;
        }
        if (const std::optional<std::u8string> name { key_argument(key, selector_branch_key_prefix) }; name.has_value())
        {
            const repository_session* const session { selected_session() };
            if (session == nullptr)
                return;
            const repository_summary& summary { session->snapshot().summary };
            const std::string branch { to_text(*name) };
            // 이미 그 브랜치에 있으면 할 일이 없다. 나머지는 사이드바 더블클릭과 같은 체크아웃이다 (작업 트리가 막으면 정책 대화 상자가 뜬다).
            if (summary.is_detached || branch != summary.current_branch)
                static_cast<void>(submit(checkout_branch_request { branch }));
        }
    }

    void shell_controller::open_repository_in_app(const std::size_t index)
    {
        open_folder_in_app(index, selected_repository_path());
    }

    void shell_controller::open_folder_in_app(const std::size_t index, const std::u8string& folder)
    {
        if (folder.empty())
            return;
        const std::vector<external_file_app> apps { repository_open_apps(settings().external_file_apps) };
        if (index >= apps.size() || dependencies_.open_folder_app == nullptr)
            return;
        dependencies_.open_folder_app(apps[index], folder);
    }

    bool shell_controller::handle_selector_message(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<open_selector_intent>() }; intent != nullptr)
        {
            open_selector(*intent);
            return true;
        }
        if (message.get<close_selector_intent>() != nullptr)
        {
            // 바깥을 눌러 닫혔다. 바로 이어지는 같은 상자의 뗌을 "열기"로 읽지 않는다.
            close_selector(true);
            return true;
        }
        if (const auto* const intent { message.get<selector_select_intent>() }; intent != nullptr)
        {
            select_from_selector(intent->key);
            return true;
        }
        return false;
    }
} // namespace cyella
