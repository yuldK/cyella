#pragma once

#include <string_view>

namespace cyella {
    enum class path_kind
    {
        // 존재하지 않거나 접근할 수 없다. 두 경우를 구분하지 않는 이유는 호출자가
        // 표식 파일의 유무만 알면 되기 때문이다.
        missing,
        file,
        directory,
    };

    // 실행 파일 탐색처럼 파일 존재 여부만 필요한 곳을 위한 경계다. 계층 규칙을 지키려고
    // filesystem 접근을 계약으로 분리하고 구현은 Win32 adapter에 둔다. 덕분에 탐색
    // 규칙을 실제 디스크 없이 test할 수 있다.
    class file_probe
    {
    public:
        file_probe() = default;
        file_probe(const file_probe&) = delete;
        file_probe(file_probe&&) = delete;
        file_probe& operator=(const file_probe&) = delete;
        file_probe& operator=(file_probe&&) = delete;
        virtual ~file_probe() = default;

        // `absolute_path`는 절대 경로여야 한다. 예외를 던지지 않는다.
        [[nodiscard]] virtual path_kind probe(std::u8string_view absolute_path) const noexcept = 0;
    };
} // namespace cyella
