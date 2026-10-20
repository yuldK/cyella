#include "domain/submodule.h"

namespace cyella {
    std::string_view submodule_state_name(const submodule_state state) noexcept
    {
        switch (state)
        {
        case submodule_state::initialized:
            return "initialized";
        case submodule_state::uninitialized:
            return "uninitialized";
        case submodule_state::modified:
            return "modified";
        case submodule_state::out_of_date:
            return "out_of_date";
        case submodule_state::conflict:
            return "conflict";
        }
        return "initialized";
    }
} // namespace cyella
