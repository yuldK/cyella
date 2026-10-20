#include "application/hosting_credentials.h"

namespace cyella {
    void wipe_secret(std::string& text) noexcept
    {
        volatile char* const data { text.data() };
        for (std::size_t index { 0 }; index < text.size(); ++index)
            data[index] = '\0';
        text.clear();
    }
} // namespace cyella
