#pragma once

#include "domain/diagnostic.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // ADR-001이 정한 최소 지원 버전이다.
    inline constexpr std::array<std::uint32_t, 3> minimum_git_version { 2u, 43u, 0u };

    enum class git_tool_availability
    {
        // 아직 조사하지 않았다.
        unknown,
        // 자동 탐색으로도 실행 파일을 찾지 못했다.
        not_found,
        // 설정에 지정한 경로가 절대 경로가 아니거나 실행할 수 없다. 이 경우 자동 탐색으로
        // 물러서지 않는다. 사용자가 의도해서 지정한 값이기 때문이다.
        path_invalid,
        // 실행은 됐지만 `--version` 출력을 해석하지 못했다.
        version_unreadable,
        too_old,
        available,
    };

    struct git_tool_version
    {
        std::uint32_t major { 0 };
        std::uint32_t minor { 0 };
        std::uint32_t patch { 0 };

        [[nodiscard]] bool operator==(const git_tool_version&) const noexcept = default;
        [[nodiscard]] auto operator<=>(const git_tool_version&) const noexcept = default;
    };

    struct git_tool_info
    {
        git_tool_availability availability { git_tool_availability::unknown };
        // 발견한 절대 경로다. `not_found`면 비어 있다.
        std::u8string executable {};
        // `--version`의 첫 줄 원문이다. 진단과 사용자 표시에 사용한다.
        std::u8string reported_version {};
        git_tool_version version {};
        // 설정으로 사용자가 직접 지정한 경로인지 여부다.
        bool manually_configured { false };
        std::vector<diagnostic> diagnostics {};

        [[nodiscard]] bool usable() const noexcept;
    };

    [[nodiscard]] git_tool_version minimum_supported_git_version() noexcept;
    [[nodiscard]] std::u8string_view git_tool_availability_name(git_tool_availability availability) noexcept;
    [[nodiscard]] std::u8string_view git_tool_unavailable_message(git_tool_availability availability) noexcept;
    [[nodiscard]] std::u8string format_git_tool_version(const git_tool_version& version);
} // namespace cyella
