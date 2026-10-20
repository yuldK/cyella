#pragma once

#include <filesystem>
#include <optional>

namespace cyella::win32 {
    inline constexpr const wchar_t* rebase_editor_file_name { L"cyella-rebase-editor.exe" };

    // 인터랙티브 rebase의 편집기 도우미를 실행 중인 앱 옆에서 찾는다 (설치 규칙이 같은 폴더에 둔다). 없으면 값이 없고,
    // 그때 `interactive_rebase`는 시작하지 않고 이유를 알린다 (ADR-005).
    [[nodiscard]] std::optional<std::filesystem::path> locate_rebase_editor_helper();
} // namespace cyella::win32
