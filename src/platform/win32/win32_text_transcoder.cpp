#include "platform/win32/win32_text_transcoder.h"

#include "platform/win32/utf8.h"

#include <windows.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace cyella::win32 {
    namespace {
        class active_code_page_transcoder final : public text_transcoder
        {
        public:
            [[nodiscard]] std::optional<std::u8string> to_utf8(const std::u8string_view bytes) noexcept override
            {
                if (bytes.empty())
                    return std::u8string {};
                if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                    return std::nullopt;

                try
                {
                    return convert(bytes);
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }

            [[nodiscard]] std::size_t safe_split_position(const std::u8string_view bytes) const noexcept override
            {
                // DBCS가 아닌 code page는 모든 byte가 문자 경계이므로 그대로 자른다.
                CPINFO info {};
                if (GetCPInfo(CP_ACP, &info) == FALSE || info.MaxCharSize <= 1)
                    return bytes.size();

                // lead와 trail byte 값 범위가 겹치므로 앞에서부터 걸어야 경계가 정확하다.
                // 마지막 문자의 trail이 아직 도착하지 않았으면 lead 앞에서 끊는다.
                std::size_t index { 0 };
                while (index < bytes.size())
                {
                    const std::size_t length { IsDBCSLeadByteEx(CP_ACP, static_cast<BYTE>(bytes[index])) != FALSE ? std::size_t { 2 } : std::size_t { 1 } };
                    if (index + length > bytes.size())
                        return index;
                    index += length;
                }
                return bytes.size();
            }

        private:
            [[nodiscard]] static std::optional<std::u8string> convert(const std::u8string_view bytes)
            {
                const auto* const data { reinterpret_cast<const char*>(bytes.data()) };
                const int length { static_cast<int>(bytes.size()) };
                // 활성 code page에서도 해석할 수 없는 byte가 있으면 실패로 처리해야
                // 호출자가 U+FFFD 대체로 되돌릴 수 있다.
                const int wide_length { MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, data, length, nullptr, 0) };
                if (wide_length <= 0)
                    return std::nullopt;

                std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
                if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, data, length, wide.data(), wide_length) != wide_length)
                    return std::nullopt;

                auto converted { utf16_to_utf8(wide) };
                if (converted.value.has_value() == false)
                    return std::nullopt;
                return std::move(*converted.value);
            }
        };
    } // namespace

    std::unique_ptr<text_transcoder> make_active_code_page_transcoder()
    {
        return std::make_unique<active_code_page_transcoder>();
    }
} // namespace cyella::win32
