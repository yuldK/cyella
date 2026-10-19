#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // 활성 code page byte를 UTF-8로 바꾸는 경계다. Windows Git의 일부 오류
    // 메시지는 UTF-8이 아니라 활성 code page로 나오기 때문에 필요하다.
    //
    // 입력은 UTF-8이 아닌 raw byte지만 저장소 계층과 같은 이유로 `std::u8string_view`에
    // 담아 전달한다. 실제 code page 해석은 Win32 adapter 구현만 수행한다.
    class text_transcoder
    {
    public:
        text_transcoder() = default;
        text_transcoder(const text_transcoder&) = delete;
        text_transcoder(text_transcoder&&) = delete;
        text_transcoder& operator=(const text_transcoder&) = delete;
        text_transcoder& operator=(text_transcoder&&) = delete;
        virtual ~text_transcoder() = default;

        // 변환할 수 없으면 값을 돌려주지 않는다. 호출자는 U+FFFD 대체로 되돌린다.
        [[nodiscard]] virtual std::optional<std::u8string> to_utf8(std::u8string_view bytes) noexcept = 0;

        // 레코드 강제 분할이 멀티바이트 문자 가운데를 가르지 않도록 문자 경계에 있는
        // 가장 뒤쪽 분할 위치를 돌려준다. 경계를 판정할 수 없으면 전체 길이를 돌려준다.
        [[nodiscard]] virtual std::size_t safe_split_position(const std::u8string_view bytes) const noexcept
        {
            return bytes.size();
        }
    };
} // namespace cyella
