#include "domain/hosting_models.h"

#include "domain/text_catalog.h"

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
            return tr_narrow(u8"hosting_model.time_soon");
        if (seconds < 60)
            return seconds == 1 ? tr_narrow(u8"hosting_model.time_seconds_one", { { u8"count", seconds } }) : tr_narrow(u8"hosting_model.time_seconds", { { u8"count", seconds } });
        if (seconds < 3600)
        {
            const std::int64_t minutes { (seconds + 59) / 60 };
            return minutes == 1 ? tr_narrow(u8"hosting_model.time_minutes_one", { { u8"count", minutes } }) : tr_narrow(u8"hosting_model.time_minutes", { { u8"count", minutes } });
        }
        const std::int64_t hours { (seconds + 3599) / 3600 };
        return hours == 1 ? tr_narrow(u8"hosting_model.time_hours_one", { { u8"count", hours } }) : tr_narrow(u8"hosting_model.time_hours", { { u8"count", hours } });
    }

    std::string describe_hosting_error(const hosting_error& error, const std::int64_t now)
    {
        const std::string detail { error.message.empty() ? std::string {} : " (" + error.message + ")" };
        switch (error.kind)
        {
        case hosting_error_kind::none:
            return {};
        case hosting_error_kind::not_signed_in:
            return tr_narrow(u8"hosting_model.error.not_signed_in");
        case hosting_error_kind::credential_unavailable:
            return tr_narrow(u8"hosting_model.error.credential_unavailable", { { u8"detail", detail } });
        case hosting_error_kind::unauthorized:
            return tr_narrow(u8"hosting_model.error.unauthorized");
        case hosting_error_kind::forbidden:
            return tr_narrow(u8"hosting_model.error.forbidden", { { u8"detail", detail } });
        case hosting_error_kind::rate_limited: {
            // 남은 횟수는 첫 절 바로 뒤에 붙는 괄호 덧말이다.
            std::string remaining {};
            if (error.rate_limit.remaining.has_value())
                remaining = tr_narrow(u8"hosting_model.error.rate_limit_remaining", { { u8"count", *error.rate_limit.remaining } });
            if (error.rate_limit.reset_at.has_value())
                return tr_narrow(u8"hosting_model.error.rate_limited_reset", { { u8"remaining", remaining }, { u8"when", describe_remaining_time(*error.rate_limit.reset_at - now) } });
            if (error.rate_limit.retry_after_seconds.has_value())
                return tr_narrow(u8"hosting_model.error.rate_limited_retry", { { u8"remaining", remaining }, { u8"when", describe_remaining_time(*error.rate_limit.retry_after_seconds) } });
            return tr_narrow(u8"hosting_model.error.rate_limited", { { u8"remaining", remaining } });
        }
        case hosting_error_kind::not_found:
            return tr_narrow(u8"hosting_model.error.not_found", { { u8"detail", detail } });
        case hosting_error_kind::rejected:
            return tr_narrow(u8"hosting_model.error.rejected", { { u8"detail", detail } });
        case hosting_error_kind::server_error:
            return tr_narrow(u8"hosting_model.error.server_error", { { u8"status", error.status_code }, { u8"detail", detail } });
        case hosting_error_kind::network:
            return tr_narrow(u8"hosting_model.error.network", { { u8"detail", detail } });
        case hosting_error_kind::invalid_response:
            return tr_narrow(u8"hosting_model.error.invalid_response");
        case hosting_error_kind::cancelled:
            return tr_narrow(u8"hosting_model.error.cancelled");
        case hosting_error_kind::unsupported:
            return tr_narrow(u8"hosting_model.error.unsupported");
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
