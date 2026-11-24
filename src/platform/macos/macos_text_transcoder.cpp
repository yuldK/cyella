#include "platform/win32/win32_text_transcoder.h"

// `platform/win32/win32_text_transcoder.h`의 macOS 구현이다.
//
// Windows의 활성 code page(`CP_ACP`)에 해당하는 것이 macOS에는 없다. 시스템 로캘의 문자 집합이 늘 UTF-8이고 git·ssh도 UTF-8로
// 말한다. 그래서 UTF-8이 아닌 바이트는 옮길 수 없다고 답하고, 호출자(출력 pipeline)가 U+FFFD로 바꾼다.
namespace cyella::win32 {
    namespace {
        class utf8_only_transcoder final : public text_transcoder
        {
        public:
            [[nodiscard]] std::optional<std::u8string> to_utf8(const std::u8string_view bytes) noexcept override
            {
                static_cast<void>(bytes);
                return std::nullopt;
            }
        };
    } // namespace

    std::unique_ptr<text_transcoder> make_active_code_page_transcoder()
    {
        return std::make_unique<utf8_only_transcoder>();
    }
} // namespace cyella::win32
