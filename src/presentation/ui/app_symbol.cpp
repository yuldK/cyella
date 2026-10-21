#include "presentation/ui/app_symbol.h"

#include "presentation/ui/element_kinds.h"

#include "luil/theme/ui_theme.h"

#include <utility>

#ifndef CYELLA_VERSION
#error "CYELLA_VERSION must be defined by the build."
#endif

namespace cyella::ui {
    namespace {
        luil::ui_image& app_symbol_storage() noexcept
        {
            static luil::ui_image image {};
            return image;
        }

        struct title_images
        {
            luil::ui_image on_light {};
            luil::ui_image on_dark {};
        };

        title_images& app_title_storage() noexcept
        {
            static title_images images {};
            return images;
        }

        // 두 장을 함께 들고, 그릴 때 바탕의 밝기로 하나를 그린다. 고른 쪽 그림 element에 자리와 그리기를 그대로 넘긴다.
        class app_title_element final : public luil::ui_element
        {
        public:
            app_title_element(std::u8string owner, std::u8string name)
                : ui_element { luil::ui_element_id { kind_image, owner } }
                , on_light_ { luil::ui_element_id { kind_image, owner + u8"-light" }, luil::image_config { .image = app_title_storage().on_light } }
                , on_dark_ { luil::ui_element_id { kind_image, owner + u8"-dark" }, luil::image_config { .image = app_title_storage().on_dark } }
                , name_ { std::move(name) }
            {
                set_hit_opaque(false);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                on_light_.arrange(context);
                on_dark_.arrange(context);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                if (luil::is_dark_background(context.palette.window_background))
                    on_dark_.draw(context, interaction);
                else
                    on_light_.draw(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::image, .name = name_ };
            }

        private:
            luil::image_element on_light_;
            luil::image_element on_dark_;
            std::u8string name_ {};
        };
    } // namespace

    void set_app_symbol(luil::ui_image image)
    {
        app_symbol_storage() = std::move(image);
    }

    const luil::ui_image& app_symbol() noexcept
    {
        return app_symbol_storage();
    }

    std::unique_ptr<luil::ui_element> make_app_symbol(std::u8string owner)
    {
        return std::make_unique<luil::image_element>(luil::ui_element_id { kind_image, std::move(owner) }, luil::image_config { .image = app_symbol() });
    }

    void set_app_title(luil::ui_image on_light, luil::ui_image on_dark)
    {
        app_title_storage() = { std::move(on_light), std::move(on_dark) };
    }

    bool app_title_available() noexcept
    {
        return app_title_storage().on_light.valid() && app_title_storage().on_dark.valid();
    }

    float app_title_aspect() noexcept
    {
        const luil::ui_image& image { app_title_storage().on_light };
        if (app_title_available() == false || image.height() <= 0)
            return 0.0f;
        return static_cast<float>(image.width()) / static_cast<float>(image.height());
    }

    std::unique_ptr<luil::ui_element> make_app_title(std::u8string owner, std::u8string name)
    {
        return std::make_unique<app_title_element>(std::move(owner), std::move(name));
    }

    std::u8string_view app_version() noexcept
    {
        return u8"" CYELLA_VERSION;
    }
} // namespace cyella::ui
