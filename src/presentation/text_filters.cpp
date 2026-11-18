#include "presentation/text_filters.h"

namespace cyella {
    std::u8string multiline_text(const std::u8string_view text)
    {
        std::u8string result {};
        result.reserve(text.size());
        for (std::size_t index { 0 }; index < text.size(); ++index)
        {
            if (text[index] == u8'\r')
            {
                result.push_back(u8'\n');
                if (index + 1 < text.size() && text[index + 1] == u8'\n')
                    ++index;
                continue;
            }
            result.push_back(text[index]);
        }
        return result;
    }
} // namespace cyella
