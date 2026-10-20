#include "presentation/ui/app_symbol.h"

#include "presentation/ui/element_kinds.h"

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

    std::u8string_view app_version() noexcept
    {
        return u8"" CYELLA_VERSION;
    }
} // namespace cyella::ui
