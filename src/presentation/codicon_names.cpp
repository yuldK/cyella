#include "presentation/codicon_names.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>

namespace cyella {
    namespace {
        struct codicon_name
        {
            std::string_view name {};
            char32_t glyph { 0 };
        };

        // configure가 luil의 mapping.json에서 이름순으로 만든다 (cmake/codicon_names.cmake).
        constexpr codicon_name codicon_names[] {
#include "codicon_names.inc"
        };
    } // namespace

    char32_t find_codicon(const std::string_view name) noexcept
    {
        std::string key {};
        key.reserve(name.size());
        for (const char value : name)
            if (std::isspace(static_cast<unsigned char>(value)) == 0)
                key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
        if (key.empty())
            return 0;
        const auto found { std::lower_bound(std::begin(codicon_names), std::end(codicon_names), key, [](const codicon_name& entry, const std::string& wanted) { return entry.name < wanted; }) };
        return found != std::end(codicon_names) && found->name == key ? found->glyph : 0;
    }
} // namespace cyella
