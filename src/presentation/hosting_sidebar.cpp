#include "presentation/hosting_sidebar.h"

#include "presentation/hosting_intents.h"
#include "presentation/start_page_view.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        void join(std::u8string& text, const std::u8string_view part)
        {
            if (text.empty() == false)
                text += u8" · ";
            text += part;
        }

        badge_tone tone_of(const ci_state state) noexcept
        {
            switch (state)
            {
            case ci_state::success:
                return badge_tone::accent;
            case ci_state::pending:
                return badge_tone::warning;
            case ci_state::failure:
            case ci_state::error:
                return badge_tone::danger;
            case ci_state::none:
                break;
            }
            return badge_tone::none;
        }

        sidebar_row_view action_row(
            std::u8string key, std::string path, std::u8string label, std::u8string detail = {}, std::u8string badge = {}, std::u8string tooltip = {}, const bool dimmed = false)
        {
            sidebar_row_view row {};
            row.key = std::move(key);
            row.section = sidebar_section::pull_requests;
            row.item = sidebar_item_kind::pull_request;
            row.path = std::move(path);
            row.label = std::move(label);
            row.detail = std::move(detail);
            row.badge = std::move(badge);
            row.tooltip = std::move(tooltip);
            row.dimmed = dimmed;
            return row;
        }

        // 안내만 하는 행이다. 눌러도 아무 일이 없다 (`path`가 비어 있다).
        sidebar_row_view info_row(std::u8string key, std::u8string label, std::u8string tooltip, const badge_tone tone = badge_tone::none)
        {
            sidebar_row_view row { action_row(std::move(key), {}, std::move(label), {}, {}, std::move(tooltip), tone == badge_tone::none) };
            row.tone = tone;
            return row;
        }
    } // namespace

    std::u8string ci_glyph_text(const ci_state state)
    {
        switch (state)
        {
        case ci_state::success:
            return u8"✓";
        case ci_state::failure:
            return u8"✗";
        case ci_state::pending:
            return u8"…";
        case ci_state::error:
            return u8"!";
        case ci_state::none:
            break;
        }
        return {};
    }

    void append_hosting_sidebar_rows(sidebar_rows& result, const hosting_repository_view& view, const std::u8string_view filter, const std::uint8_t collapsed_sections)
    {
        const bool collapsed { (collapsed_sections & (1u << static_cast<unsigned>(sidebar_section::pull_requests))) != 0 };
        const std::size_t header_row { result.rows.size() };
        result.rows.push_back({ .key = u8"h:PULL REQUESTS", .header = true, .section = sidebar_section::pull_requests, .label = u8"PULL REQUESTS", .collapsed = collapsed });
        std::uint32_t count { 0 };
        const auto finish = [&]() {
            result.counts[static_cast<std::size_t>(sidebar_section::pull_requests)] = count;
            result.rows[header_row].count = count;
        };
        const std::u8string abbreviation { view.abbreviation.empty() ? std::u8string { u8"PR" } : view.abbreviation };
        const auto add = [&](sidebar_row_view row) {
            if (collapsed == false)
                result.rows.push_back(std::move(row));
        };

        switch (view.support)
        {
        case hosting_support::no_remote:
            add(info_row(u8"pr:info", u8"원격이 없습니다", u8"GitHub·GitLab 연동은 원격 저장소가 있어야 합니다. 원격을 추가한 뒤 다시 보세요."));
            finish();
            return;
        case hosting_support::unsupported:
            add(info_row(u8"pr:info", u8"GitHub·GitLab 원격이 아닙니다", u8"이 원격의 호스트를 알아보지 못했습니다. 자체 호스팅 서버라면 환경 설정의 '호스팅'에서 호스트를 추가하세요."));
            finish();
            return;
        case hosting_support::supported:
            break;
        }

        if (view.signed_in == false)
        {
            const std::u8string where { view.provider_name + u8" (" + to_u8(view.host_key) + u8")" };
            std::u8string tooltip = where + u8"에 로그인합니다. Git Credential Manager 같은 자격 증명 도우미에서 토큰을 받으며 cyella는 저장하지 않습니다.";
            add(action_row(u8"pr:login", hosting_row_login, view.signing_in ? u8"로그인 중…" : u8"로그인…", view.provider_name + u8" · " + to_u8(view.host_key), {}, std::move(tooltip)));
            if (view.error_text.empty() == false)
                add(info_row(u8"pr:error", view.error_text, view.error_text, badge_tone::danger));
            finish();
            return;
        }

        for (const pull_request_info& info : *view.pulls)
        {
            const std::u8string title { to_u8(info.title) };
            const std::u8string branches { to_u8(info.source_branch) + u8" → " + to_u8(info.target_branch) };
            if (matches_search_text(title + u8" " + branches + u8" #" + to_u8(std::to_string(info.number)) + u8" " + to_u8(info.author), filter) == false)
                continue;
            ++count;
            if (collapsed)
                continue;
            sidebar_row_view row {};
            row.key = u8"pr:n:" + to_u8(std::to_string(info.number));
            row.section = sidebar_section::pull_requests;
            row.item = sidebar_item_kind::pull_request;
            row.path = std::to_string(info.number);
            row.label = u8"#" + to_u8(std::to_string(info.number)) + u8" " + title;
            row.detail = branches;
            std::u8string badge {};
            if (info.draft && info.state == pull_request_state::open)
                join(badge, u8"초안");
            else if (info.state == pull_request_state::merged)
                join(badge, u8"병합됨");
            else if (info.state == pull_request_state::closed)
                join(badge, u8"닫힘");
            row.tone = info.state == pull_request_state::open ? badge_tone::none : (info.state == pull_request_state::merged ? badge_tone::accent : badge_tone::warning);
            const auto ci { std::ranges::find_if(view.list_ci, [&info](const auto& entry) { return entry.first == info.number; }) };
            if (ci != view.list_ci.end() && ci->second != ci_state::none)
            {
                join(badge, u8"CI " + ci_glyph_text(ci->second));
                row.tone = tone_of(ci->second);
            }
            row.badge = std::move(badge);
            row.dimmed = info.state != pull_request_state::open;
            row.target_sha = info.head_sha;
            row.tooltip = u8"#" + to_u8(std::to_string(info.number)) + u8" " + title + u8"\n" + to_u8(info.author) + u8" · " + branches;
            result.rows.push_back(std::move(row));
        }

        if (collapsed)
        {
            finish();
            return;
        }
        if (view.loading)
            add(info_row(u8"pr:loading", u8"불러오는 중…", u8"서버에서 목록을 읽고 있습니다."));
        else if (view.error_text.empty() == false)
            add(info_row(u8"pr:error", view.error_text, view.error_text, badge_tone::danger));
        else if (view.loaded && view.pulls->empty())
            add(info_row(u8"pr:empty", view.include_closed ? u8"항목이 없습니다" : u8"열린 " + abbreviation + u8"이 없습니다", {}));
        else if (view.loaded == false)
            add(info_row(u8"pr:unloaded", u8"목록을 아직 읽지 않았습니다", u8"'새로 고침'을 누르면 읽습니다."));
        if (view.has_more && view.loading == false)
            add(action_row(u8"pr:more", hosting_row_more, u8"더 보기…", {}, {}, u8"다음 쪽을 읽습니다."));
        add(action_row(u8"pr:closed", hosting_row_toggle_closed, u8"닫힌·병합된 " + abbreviation + u8" 포함", {}, view.include_closed ? u8"켬" : u8"끔",
            u8"눌러서 닫힌 것과 병합된 " + abbreviation + u8"까지 볼지 바꿉니다."));
        add(action_row(u8"pr:refresh", hosting_row_refresh, u8"새로 고침", view.user.empty() ? std::u8string {} : view.user, {},
            view.rate_limit_text.empty() ? u8"서버에서 목록을 다시 읽습니다." : u8"서버에서 목록을 다시 읽습니다.\n" + view.rate_limit_text));
        add(action_row(u8"pr:create", hosting_row_create, u8"현재 브랜치로 " + abbreviation + u8" 만들기…", {}, {},
            view.can_create ? u8"현재 브랜치에서 " + abbreviation + u8"을 만드는 대화 상자를 엽니다." : view.create_blocked_reason, view.can_create == false));
        finish();
    }
} // namespace cyella
