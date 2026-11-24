#include "platform/macos/macos_resources.h"

#include <cstddef>
#include <cstdint>

// 생성 파일(luil의 cmake/embed_binary.cmake)이 정의하는 배열이다.
extern const std::uint8_t cyella_locale_ko[];
extern const std::size_t cyella_locale_ko_size;
extern const std::uint8_t cyella_locale_en[];
extern const std::size_t cyella_locale_en_size;
extern const std::uint8_t cyella_locale_ko_macos[];
extern const std::size_t cyella_locale_ko_macos_size;
extern const std::uint8_t cyella_locale_en_macos[];
extern const std::size_t cyella_locale_en_macos_size;
extern const std::uint8_t cyella_symbol_svg[];
extern const std::size_t cyella_symbol_svg_size;
extern const std::uint8_t cyella_title_svg[];
extern const std::size_t cyella_title_svg_size;
extern const std::uint8_t cyella_title_sea_svg[];
extern const std::size_t cyella_title_sea_svg_size;

namespace cyella::macos {
    namespace {
        std::string_view view_of(const std::uint8_t* const data, const std::size_t size) noexcept
        {
            return { reinterpret_cast<const char*>(data), size };
        }
    } // namespace

    std::string_view embedded_bytes(const embedded_resource resource) noexcept
    {
        switch (resource)
        {
        case embedded_resource::locale_korean:
            return view_of(cyella_locale_ko, cyella_locale_ko_size);
        case embedded_resource::locale_english:
            return view_of(cyella_locale_en, cyella_locale_en_size);
        case embedded_resource::locale_korean_macos:
            return view_of(cyella_locale_ko_macos, cyella_locale_ko_macos_size);
        case embedded_resource::locale_english_macos:
            return view_of(cyella_locale_en_macos, cyella_locale_en_macos_size);
        case embedded_resource::app_symbol:
            return view_of(cyella_symbol_svg, cyella_symbol_svg_size);
        case embedded_resource::app_title:
            return view_of(cyella_title_svg, cyella_title_svg_size);
        case embedded_resource::app_title_sea:
            return view_of(cyella_title_sea_svg, cyella_title_sea_svg_size);
        }
        return {};
    }
} // namespace cyella::macos
