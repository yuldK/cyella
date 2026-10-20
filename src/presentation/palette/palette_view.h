#pragma once

#include "presentation/palette/palette_model.h"

#include "luil/ui/text_input_state.h"

#include <cstddef>
#include <string>
#include <vector>

namespace cyella {
    // 명령 팔레트의 snapshot이다. controller가 상태·설정·저장소에서 짓고 `ui::build_palette_dialog`가 이 값만 읽어 tree를 짓는다.
    struct palette_item_view
    {
        std::u8string key {};
        std::u8string title {};
        std::u8string detail {};
        bool enabled { true };
        // 고르면 UI thread의 폴더 대화 상자가 뜬다 (메시지가 아니라 UI 명령 액션을 낸다).
        bool ui_thread_command { false };
    };

    struct palette_view
    {
        palette_mode mode { palette_mode::commands };
        std::u8string title {};
        std::u8string placeholder {};
        luil::text_input_view filter {};
        std::vector<palette_item_view> items {};
        // 키보드 커서가 선 항목의 키다. 항상 목록에 있는 활성 항목이거나 (활성 항목이 없으면) 비어 있다.
        std::u8string cursor {};
        std::size_t matched { 0 };
    };
} // namespace cyella
