#include "platform/win32/cyella_driver.h"

#include "presentation/ui/cyella_caption.h"

#include <memory>
#include <utility>

namespace cyella::win32 {
    namespace {
        constexpr luil::ui_element_kind kind_placeholder_label { luil::application_element_kind(0) };

        // 논리 픽셀(96 DPI 기준) 상수다. 그릴 때 배율을 곱한다.
        constexpr float default_window_width { 1280.0f };
        constexpr float default_window_height { 720.0f };
        constexpr float content_padding { 24.0f };
        constexpr float placeholder_font_size { 14.0f };
    } // namespace

    void cyella_driver::handle(luil::app_message message)
    {
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
    }

    std::shared_ptr<const luil::win32::ui_frame> cyella_driver::make_frame()
    {
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float width { metrics_.width > 0.0f ? metrics_.width : default_window_width };
        const float height { metrics_.height > 0.0f ? metrics_.height : default_window_height };

        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, width, height }, scale });

        // 데스크톱은 custom caption, 모바일은 앱 바다. 높이는 논리 픽셀이라 배율을 곱한다.
        const luil::caption_config caption_config { ui::cyella_caption() };
        float caption_height { 0.0f };
        if (luil::current_ui_platform().window_caption)
        {
            caption_height = luil::caption_element::height_for(caption_config) * scale;
            auto caption { std::make_unique<luil::caption_element>(caption_config) };
            caption->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
            root->add(std::move(caption));
        }
        else
        {
            const luil::app_bar_config app_bar_config { .title = caption_config.title };
            caption_height = luil::app_bar_element::height_for(app_bar_config) * scale;
            auto app_bar { std::make_unique<luil::app_bar_element>(app_bar_config) };
            app_bar->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
            root->add(std::move(app_bar));
        }

        luil::stack_config column_config {};
        column_config.padding = luil::edge_insets::all(content_padding);
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_placeholder_label, u8"layout" }, column_config) };

        luil::label_config label_config {};
        label_config.text = u8"cyella";
        label_config.font_size = placeholder_font_size;
        label_config.color = luil::label_color_role::primary;
        auto label { std::make_unique<luil::label_element>(luil::ui_element_id { kind_placeholder_label }, label_config) };
        column->add(std::move(label), luil::label_element::height_for(label_config));

        // caption 아래 나머지 영역이 내용의 slot이다.
        column->arrange({ { 0.0f, caption_height, width, height - caption_height }, scale });
        root->add(std::move(column));

        auto frame { std::make_shared<luil::win32::ui_frame>() };
        frame->tree = std::make_shared<const luil::ui_tree>(std::move(root));
        return frame;
    }

    luil::app_message cyella_driver::make_close_message()
    {
        return luil::app_message { close_intent {} };
    }

    bool cyella_driver::shutdown_completed() const
    {
        return closed_.load();
    }
} // namespace cyella::win32
