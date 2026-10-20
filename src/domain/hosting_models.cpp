#include "domain/hosting_models.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace cyella {
    namespace {
        // 1970-01-01부터 (year, month, day)까지의 날 수다 (Howard Hinnant의 days_from_civil).
        std::int64_t days_from_civil(std::int64_t year, const unsigned month, const unsigned day) noexcept
        {
            year -= month <= 2 ? 1 : 0;
            const std::int64_t era { (year >= 0 ? year : year - 399) / 400 };
            const auto year_of_era { static_cast<unsigned>(year - era * 400) };
            const unsigned day_of_year { (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1 };
            const unsigned day_of_era { year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year };
            return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
        }

        bool read_number(std::string_view& text, const std::size_t digits, int& value)
        {
            if (text.size() < digits)
                return false;
            int parsed { 0 };
            const auto [end, error] { std::from_chars(text.data(), text.data() + digits, parsed) };
            if (error != std::errc {} || end != text.data() + digits)
                return false;
            value = parsed;
            text.remove_prefix(digits);
            return true;
        }

        bool consume(std::string_view& text, const char expected)
        {
            if (text.empty() || text.front() != expected)
                return false;
            text.remove_prefix(1);
            return true;
        }
    } // namespace

    std::string describe_remaining_time(const std::int64_t seconds)
    {
        if (seconds <= 0)
            return "곧";
        if (seconds < 60)
            return std::to_string(seconds) + "초 후";
        if (seconds < 3600)
            return std::to_string((seconds + 59) / 60) + "분 후";
        return std::to_string((seconds + 3599) / 3600) + "시간 후";
    }

    std::string describe_hosting_error(const hosting_error& error, const std::int64_t now)
    {
        const std::string detail { error.message.empty() ? std::string {} : " (" + error.message + ")" };
        switch (error.kind)
        {
        case hosting_error_kind::none:
            return {};
        case hosting_error_kind::not_signed_in:
            return "로그인하지 않았습니다. 사이드바나 환경 설정의 '로그인'을 누르세요.";
        case hosting_error_kind::credential_unavailable:
            return "자격 증명 도우미가 토큰을 주지 않았습니다. Git Credential Manager가 설정되어 있는지 확인하세요." + detail;
        case hosting_error_kind::unauthorized:
            return "인증이 거절되었습니다. 저장해 둔 자격 증명을 지웠으니 다시 로그인하세요.";
        case hosting_error_kind::forbidden:
            return "이 작업을 할 권한이 없습니다." + detail;
        case hosting_error_kind::rate_limited: {
            std::string text { "API 호출 한도에 도달했습니다" };
            if (error.rate_limit.remaining.has_value())
                text += " (남은 횟수 " + std::to_string(*error.rate_limit.remaining) + ")";
            if (error.rate_limit.reset_at.has_value())
                text += ", " + describe_remaining_time(*error.rate_limit.reset_at - now) + " 초기화됩니다";
            else if (error.rate_limit.retry_after_seconds.has_value())
                text += ", " + describe_remaining_time(*error.rate_limit.retry_after_seconds) + " 다시 시도할 수 있습니다";
            return text + ".";
        }
        case hosting_error_kind::not_found:
            return "저장소나 대상을 찾을 수 없습니다. 비공개 저장소라면 토큰의 권한을 확인하세요." + detail;
        case hosting_error_kind::rejected:
            return "서버가 요청을 받지 않았습니다." + detail;
        case hosting_error_kind::server_error:
            return "서버 오류입니다 (" + std::to_string(error.status_code) + ")." + detail;
        case hosting_error_kind::network:
            return "서버에 연결하지 못했습니다." + detail;
        case hosting_error_kind::invalid_response:
            return "서버의 응답을 읽지 못했습니다.";
        case hosting_error_kind::cancelled:
            return "취소되었습니다.";
        case hosting_error_kind::unsupported:
            return "이 원격은 GitHub·GitLab 연동을 쓸 수 없습니다. 환경 설정의 '호스팅'에서 호스트를 추가하세요.";
        }
        return {};
    }

    std::int64_t parse_iso8601_seconds(std::string_view text) noexcept
    {
        int year { 0 };
        int month { 0 };
        int day { 0 };
        int hour { 0 };
        int minute { 0 };
        int second { 0 };
        if (read_number(text, 4, year) == false || consume(text, '-') == false || read_number(text, 2, month) == false || consume(text, '-') == false || read_number(text, 2, day) == false)
            return 0;
        if (month < 1 || month > 12 || day < 1 || day > 31)
            return 0;
        if (text.empty() == false && (text.front() == 'T' || text.front() == ' '))
        {
            text.remove_prefix(1);
            if (read_number(text, 2, hour) == false || consume(text, ':') == false || read_number(text, 2, minute) == false)
                return 0;
            if (consume(text, ':') && read_number(text, 2, second) == false)
                return 0;
            if (consume(text, '.'))
                while (text.empty() == false && std::isdigit(static_cast<unsigned char>(text.front())) != 0)
                    text.remove_prefix(1);
        }
        std::int64_t offset { 0 };
        if (text.empty() == false && (text.front() == '+' || text.front() == '-'))
        {
            const int sign { text.front() == '-' ? -1 : 1 };
            text.remove_prefix(1);
            int offset_hour { 0 };
            int offset_minute { 0 };
            if (read_number(text, 2, offset_hour) == false)
                return 0;
            consume(text, ':');
            if (text.empty() == false && read_number(text, 2, offset_minute) == false)
                return 0;
            offset = sign * (offset_hour * 3600 + offset_minute * 60);
        }
        else if (text.empty() == false && text.front() != 'Z')
            return 0;
        return days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 + hour * 3600 + minute * 60 + second - offset;
    }

    std::string_view ci_state_name(const ci_state state) noexcept
    {
        switch (state)
        {
        case ci_state::none:
            return "none";
        case ci_state::pending:
            return "pending";
        case ci_state::success:
            return "success";
        case ci_state::failure:
            return "failure";
        case ci_state::error:
            return "error";
        }
        return "none";
    }

    ci_state combine_ci_states(const std::vector<ci_state>& states) noexcept
    {
        const auto has = [&states](const ci_state wanted) { return std::ranges::find(states, wanted) != states.end(); };
        if (has(ci_state::failure))
            return ci_state::failure;
        if (has(ci_state::error))
            return ci_state::error;
        if (has(ci_state::pending))
            return ci_state::pending;
        if (has(ci_state::success))
            return ci_state::success;
        return ci_state::none;
    }
} // namespace cyella
