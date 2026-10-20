#include "presentation/commands/key_chord.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace cyella {
    namespace {
        // 이름이 있는 키다. 글자·숫자는 아래에서 따로 다룬다.
        struct named_key
        {
            std::u8string_view name;
            luil::key_code key;
        };

        // Win32 가상 키 값이다 (luil이 이름을 붙이지 않은 키는 `platform_key_code`로 나른다).
        constexpr std::uint32_t vk_back { 0x08 };
        constexpr std::uint32_t vk_add { 0x6B };
        constexpr std::uint32_t vk_subtract { 0x6D };
        constexpr std::uint32_t vk_oem_1 { 0xBA };
        constexpr std::uint32_t vk_oem_plus { 0xBB };
        constexpr std::uint32_t vk_oem_comma { 0xBC };
        constexpr std::uint32_t vk_oem_minus { 0xBD };
        constexpr std::uint32_t vk_oem_period { 0xBE };
        constexpr std::uint32_t vk_oem_2 { 0xBF };
        constexpr std::uint32_t vk_oem_3 { 0xC0 };
        constexpr std::uint32_t vk_oem_4 { 0xDB };
        constexpr std::uint32_t vk_oem_5 { 0xDC };
        constexpr std::uint32_t vk_oem_6 { 0xDD };
        constexpr std::uint32_t vk_oem_7 { 0xDE };

        const std::vector<named_key>& named_keys()
        {
            static const std::vector<named_key> keys {
                { u8"Enter", luil::key_code::enter },
                { u8"Escape", luil::key_code::escape },
                { u8"Tab", luil::key_code::tab },
                { u8"Space", luil::key_code::space },
                { u8"Left", luil::key_code::arrow_left },
                { u8"Right", luil::key_code::arrow_right },
                { u8"Up", luil::key_code::arrow_up },
                { u8"Down", luil::key_code::arrow_down },
                { u8"Home", luil::key_code::home },
                { u8"End", luil::key_code::end },
                { u8"PageUp", luil::key_code::page_up },
                { u8"PageDown", luil::key_code::page_down },
                { u8"Insert", luil::key_code::insert },
                { u8"Delete", luil::key_code::delete_forward },
                { u8"Backspace", luil::platform_key_code(vk_back) },
                { u8"F1", luil::key_code::f1 },
                { u8"F2", luil::key_code::f2 },
                { u8"F3", luil::key_code::f3 },
                { u8"F4", luil::key_code::f4 },
                { u8"F5", luil::key_code::f5 },
                { u8"F6", luil::key_code::f6 },
                { u8"F7", luil::key_code::f7 },
                { u8"F8", luil::key_code::f8 },
                { u8"F9", luil::key_code::f9 },
                { u8"F10", luil::key_code::f10 },
                { u8"F11", luil::key_code::f11 },
                { u8"F12", luil::key_code::f12 },
                { u8"=", luil::platform_key_code(vk_oem_plus) },
                { u8"-", luil::platform_key_code(vk_oem_minus) },
                { u8",", luil::platform_key_code(vk_oem_comma) },
                { u8".", luil::platform_key_code(vk_oem_period) },
                { u8"/", luil::platform_key_code(vk_oem_2) },
                { u8";", luil::platform_key_code(vk_oem_1) },
                { u8"[", luil::platform_key_code(vk_oem_4) },
                { u8"]", luil::platform_key_code(vk_oem_6) },
                { u8"\\", luil::platform_key_code(vk_oem_5) },
                { u8"'", luil::platform_key_code(vk_oem_7) },
                { u8"`", luil::platform_key_code(vk_oem_3) },
                { u8"NumpadAdd", luil::platform_key_code(vk_add) },
                { u8"NumpadSubtract", luil::platform_key_code(vk_subtract) },
            };
            return keys;
        }

        char8_t ascii_lower(const char8_t value) noexcept
        {
            return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value - u8'A' + u8'a') : value;
        }

        bool equals_ignoring_case(const std::u8string_view left, const std::u8string_view right) noexcept
        {
            if (left.size() != right.size())
                return false;
            for (std::size_t index { 0 }; index < left.size(); ++index)
                if (ascii_lower(left[index]) != ascii_lower(right[index]))
                    return false;
            return true;
        }

        std::u8string trimmed(std::u8string_view text)
        {
            while (text.empty() == false && (text.front() == u8' ' || text.front() == u8'\t'))
                text.remove_prefix(1);
            while (text.empty() == false && (text.back() == u8' ' || text.back() == u8'\t'))
                text.remove_suffix(1);
            return std::u8string { text };
        }

        // 읽은 글에서 수정키 접두를 하나 뗀다.
        bool take_modifier(std::u8string_view& text, key_chord& chord)
        {
            const std::size_t plus { text.find(u8'+') };
            // 마지막 조각(키 이름)은 `+`가 없다. `Ctrl++`처럼 키 이름 자체가 `+`인 경우는 받지 않는다 (`=`가 같은 키다).
            if (plus == std::u8string_view::npos || plus == 0)
                return false;
            const std::u8string_view word { text.substr(0, plus) };
            if (equals_ignoring_case(word, u8"ctrl") || equals_ignoring_case(word, u8"control"))
                chord.control = true;
            else if (equals_ignoring_case(word, u8"alt"))
                chord.alt = true;
            else if (equals_ignoring_case(word, u8"shift"))
                chord.shift = true;
            else
                return false;
            text.remove_prefix(plus + 1);
            return true;
        }

        std::optional<luil::key_code> key_of_name(const std::u8string_view name)
        {
            if (name.size() == 1)
            {
                const char8_t value { ascii_lower(name.front()) };
                if (value >= u8'a' && value <= u8'z')
                    return luil::alphanumeric_key_code(static_cast<std::uint32_t>(value - u8'a' + u8'A'));
                if (value >= u8'0' && value <= u8'9')
                    return luil::alphanumeric_key_code(static_cast<std::uint32_t>(value));
            }
            for (const named_key& key : named_keys())
                if (equals_ignoring_case(name, key.name))
                    return key.key;
            return std::nullopt;
        }

        std::u8string name_of_key(const luil::key_code key)
        {
            if (const std::uint32_t virtual_key { luil::platform_key_of(key) }; virtual_key != 0 && luil::alphanumeric_key_code(virtual_key) == key)
                return std::u8string(1, static_cast<char8_t>(virtual_key));
            for (const named_key& named : named_keys())
                if (named.key == key)
                    return std::u8string { named.name };
            return {};
        }

        // 이름 키의 번호가 F5만 떨어져 있어(f5는 방향 키 옆에 있다) 범위가 아니라 목록으로 본다.
        bool is_function_key(const luil::key_code key) noexcept
        {
            switch (key)
            {
            case luil::key_code::f1:
            case luil::key_code::f2:
            case luil::key_code::f3:
            case luil::key_code::f4:
            case luil::key_code::f5:
            case luil::key_code::f6:
            case luil::key_code::f7:
            case luil::key_code::f8:
            case luil::key_code::f9:
            case luil::key_code::f10:
            case luil::key_code::f11:
            case luil::key_code::f12:
                return true;
            default:
                return false;
            }
        }
    } // namespace

    std::optional<key_chord> parse_key_chord(const std::u8string_view text)
    {
        std::u8string source { trimmed(text) };
        std::u8string_view rest { source };
        key_chord chord {};
        while (take_modifier(rest, chord))
        {
        }
        const std::optional<luil::key_code> key { key_of_name(rest) };
        if (key.has_value() == false)
            return std::nullopt;
        chord.key = *key;
        return chord;
    }

    std::u8string format_key_chord(const key_chord& chord)
    {
        if (chord.valid() == false)
            return {};
        const std::u8string name { name_of_key(chord.key) };
        if (name.empty())
            return {};
        std::u8string text {};
        if (chord.control)
            text += u8"Ctrl+";
        if (chord.alt)
            text += u8"Alt+";
        if (chord.shift)
            text += u8"Shift+";
        return text + name;
    }

    std::u8string normalize_key_chord_text(const std::u8string_view text)
    {
        const std::optional<key_chord> chord { parse_key_chord(text) };
        return chord.has_value() ? format_key_chord(*chord) : std::u8string {};
    }

    key_chord chord_of(const luil::key_pressed_event& event) noexcept
    {
        return key_chord { .key = event.key, .control = event.control, .alt = event.alt, .shift = event.shift };
    }

    std::u8string check_key_chord(const key_chord& chord)
    {
        if (chord.valid() == false || format_key_chord(chord).empty())
            return u8"알 수 없는 키입니다.";
        const bool modified { chord.control || chord.alt };
        if (modified == false && is_function_key(chord.key) == false && chord.key != luil::key_code::escape)
            return u8"Ctrl이나 Alt와 함께 눌러야 합니다 (수정키 없이 쓸 수 있는 것은 F1~F12와 Esc뿐입니다).";
        if (chord.control && chord.alt == false && chord.shift == false)
            if (chord.key == luil::key_code::key_a || chord.key == luil::key_code::key_c || chord.key == luil::key_code::key_v || chord.key == luil::key_code::key_x
                || chord.key == luil::key_code::key_y || chord.key == luil::key_code::key_z)
                return u8"텍스트 칸의 편집 키라 쓸 수 없습니다.";
        if (chord.alt && chord.control == false && chord.shift == false && chord.key == luil::key_code::f4)
            return u8"Alt+F4는 창을 닫는 키라 쓸 수 없습니다.";
        return {};
    }

    chord_list_parse parse_key_chord_list(const std::u8string_view text)
    {
        chord_list_parse result {};
        std::u8string_view rest { text };
        while (true)
        {
            const std::size_t separator { rest.find(u8" / ") };
            const std::u8string piece { trimmed(rest.substr(0, separator)) };
            if (piece.empty() == false)
            {
                const std::optional<key_chord> chord { parse_key_chord(piece) };
                if (chord.has_value() == false)
                {
                    result.failed = piece;
                    result.chords.clear();
                    return result;
                }
                if (std::ranges::find(result.chords, *chord) == result.chords.end())
                    result.chords.push_back(*chord);
            }
            if (separator == std::u8string_view::npos)
                break;
            rest.remove_prefix(separator + 3);
        }
        return result;
    }

    std::u8string format_key_chord_list(const std::vector<key_chord>& chords)
    {
        std::u8string text {};
        for (const key_chord& chord : chords)
        {
            const std::u8string one { format_key_chord(chord) };
            if (one.empty())
                continue;
            if (text.empty() == false)
                text += u8" / ";
            text += one;
        }
        return text;
    }
} // namespace cyella
