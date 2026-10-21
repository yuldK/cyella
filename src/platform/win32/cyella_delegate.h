#pragma once

#include "platform/win32/operation_result_relay.h"
#include "platform/win32/win32_foreground_monitor.h"

#include "luil/luil.h"

#include <memory>

namespace cyella::win32 {
    class cyella_driver;
    class ref_hover_monitor;

    // luil UI thread에 꽂는 cyella 훅이다. 창 사건을 앱 메시지로 바꾸기만 하고
    // 상태는 직접 고치지 않는다.
    //
    // OS 대화 상자(폴더 고르기)는 UI thread의 일이라 여기서 연다. element는 `app_ui_command`
    // (`ui_command_id`)를 내고, 고른 결과는 `folder_chosen_intent`로 logic thread에 돌려준다.
    class cyella_delegate final : public luil::win32::window_delegate
    {
    public:
        // relay는 driver가 소유한다. host가 준비되면 묶어 worker 결과가 logic thread를 깨우게 한다.
        // driver는 host가 준비되면 클립보드 요청을 보낼 길을 얻는다.
        explicit cyella_delegate(operation_result_relay* relay = nullptr, cyella_driver* driver = nullptr) noexcept;

        ~cyella_delegate() override;

        void on_started(luil::app_host& host) override;
        void execute_app_ui_command(luil::app_host& host, const luil::app_ui_command& command) override;
        [[nodiscard]] luil::app_message make_window_metrics_message(float width, float height, float scale) override;

    private:
        operation_result_relay* relay_ { nullptr };
        cyella_driver* driver_ { nullptr };
        // 다른 앱에서 창으로 돌아오는 순간을 듣는다 (`window_focus_intent`). UI thread에서 만들고 이 객체와 함께 없어진다.
        std::unique_ptr<foreground_monitor> foreground_ {};
        std::unique_ptr<ref_hover_monitor> ref_hover_ {};
    };
} // namespace cyella::win32
