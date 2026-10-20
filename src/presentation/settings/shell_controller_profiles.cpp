// 저장소별 사용자 프로필의 controller 쪽이다. shell_controller의 멤버 함수를 파일 하나로 나눠 둔다.
//
// 신원 읽기와 `git config --local` 쓰기는 worker가 하고(driver), 이 파일은 결과를 받아 화면 상태로 옮긴다. 전역 git 설정은 어디서도 건드리지 않는다.
#include "presentation/shell_controller.h"

#include "domain/repository_path.h"
#include "domain/user_profile.h"
#include "presentation/settings/profiles_intents.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace cyella {
    namespace {
        // 신원을 다시 읽는 간격이다. 터미널에서 `git config`를 고친 것이 따라오게 한다.
        constexpr std::chrono::seconds identity_refresh_interval { 15 };

        constexpr std::u8string_view apply_profile_key_prefix { u8"apply_profile:" };
        constexpr std::u8string_view commit_apply_key_prefix { u8"commit_apply:" };
        constexpr std::u8string_view commit_anyway_key { u8"commit_anyway" };
        // 커밋 전 경고가 권하는 프로필의 최대 수다 (규칙이 없을 때 처음 몇 개만 단추로 둔다).
        constexpr std::size_t offered_profile_limit { 3 };
    } // namespace

    const git_identity* shell_controller::selected_identity() const
    {
        const auto found { identities_.find(selected_repository_id()) };
        return found != identities_.end() && found->second.loaded ? &found->second.identity : nullptr;
    }

    std::u8string shell_controller::suggested_profile_name() const
    {
        const std::string id { selected_repository_id() };
        if (id.empty() || dependencies_.workspace == nullptr)
            return {};
        const repository_session* const session { std::as_const(*dependencies_.workspace).find_session(id) };
        if (session == nullptr)
            return {};
        const app_settings& value { settings() };
        const std::optional<std::size_t> rule { match_profile_rule(value.profile_rules, value.profiles, session->path()) };
        return rule.has_value() ? value.profile_rules[*rule].profile : std::u8string {};
    }

    std::u8string shell_controller::applied_profile_name() const
    {
        const git_identity* const identity { selected_identity() };
        if (identity == nullptr || dependencies_.workspace == nullptr)
            return {};
        const repository_session* const session { std::as_const(*dependencies_.workspace).find_session(selected_repository_id()) };
        const app_settings& value { settings() };
        // 적용 기록이 있고 신원이 아직 그 프로필과 같으면 그 프로필이다. 아니면 같은 이름·이메일을 가진 첫 프로필이다.
        if (session != nullptr)
            if (const std::optional<std::u8string> assigned { assigned_profile(value, session->path()) }; assigned.has_value())
                if (const std::optional<std::size_t> index { find_profile_index(value.profiles, *assigned) }; index.has_value() && identity_matches_profile(*identity, value.profiles[*index]))
                    return value.profiles[*index].name;
        for (const user_profile& profile : value.profiles)
            if (identity_matches_profile(*identity, profile))
                return profile.name;
        return {};
    }

    void shell_controller::sync_identity()
    {
        if (dependencies_.request_identity == nullptr || dependencies_.workspace == nullptr)
            return;
        const std::string id { selected_repository_id() };
        const repository_session* const session { id.empty() ? nullptr : std::as_const(*dependencies_.workspace).find_session(id) };
        if (session == nullptr)
            return;
        identity_entry& entry { identities_[id] };
        const auto now { std::chrono::steady_clock::now() };
        if (entry.requested == false || now - entry.read_at > identity_refresh_interval)
        {
            entry.requested = true;
            entry.read_at = now;
            dependencies_.request_identity(id, session->path());
        }
        if (entry.loaded == false || profile_proposed_.contains(id))
            return;
        // 규칙에 맞는 프로필이 있고 지금 신원이 그것과 다르면 한 번 제안한다. 설정을 쓰지는 않는다 ([적용]을 눌러야 쓴다).
        profile_proposed_.insert(id);
        const app_settings& value { settings() };
        const std::optional<std::size_t> rule { match_profile_rule(value.profile_rules, value.profiles, session->path()) };
        if (rule.has_value() == false)
            return;
        const std::optional<std::size_t> wanted { find_profile_index(value.profiles, value.profile_rules[*rule].profile) };
        if (wanted.has_value() == false || identity_matches_profile(entry.identity, value.profiles[*wanted]))
            return;
        // 사용자가 이 저장소에 다른 프로필을 적용해 두었고 신원이 그대로 그것이면 규칙을 따르지 않기로 한 선택이다.
        if (const std::optional<std::u8string> assigned { assigned_profile(value, session->path()) }; assigned.has_value())
            if (const std::optional<std::size_t> index { find_profile_index(value.profiles, *assigned) }; index.has_value() && identity_matches_profile(entry.identity, value.profiles[*index]))
                return;
        const user_profile& profile { value.profiles[*wanted] };
        std::u8string text = u8"규칙 '" + value.profile_rules[*rule].path_prefix + u8"'이 이 저장소에 프로필 '" + profile.name + u8"' 권장 · 지금 ";
        text += describe_identity_with_source(entry.identity);
        notice_state notice { .text = std::move(text), .warning = false, .actions = {} };
        notice.actions.push_back(notice_action { .key = std::u8string { apply_profile_key_prefix } + profile.name, .label = u8"'" + profile.name + u8"' 적용" });
        ui_.notice = std::move(notice);
    }

    void shell_controller::apply_profile(const std::u8string& name)
    {
        const app_settings& value { settings() };
        const std::optional<std::size_t> index { find_profile_index(value.profiles, name) };
        if (index.has_value() == false)
        {
            show_notice(u8"프로필을 찾을 수 없습니다: " + name, true);
            return;
        }
        const std::string id { selected_repository_id() };
        const repository_session* const session { id.empty() || dependencies_.workspace == nullptr ? nullptr : std::as_const(*dependencies_.workspace).find_session(id) };
        if (session == nullptr)
        {
            show_notice(u8"프로필을 적용할 저장소가 없습니다. 저장소 탭을 연 뒤 다시 시도하세요.", true);
            return;
        }
        if (dependencies_.apply_identity == nullptr)
        {
            show_notice(u8"이 환경에서는 프로필을 적용할 수 없습니다.", true);
            return;
        }
        const user_profile& profile { value.profiles[*index] };
        applying_profiles_[id] = profile.name;
        dependencies_.apply_identity(id, session->path(), profile.user_name, profile.user_email);
        show_notice(u8"이 저장소에 프로필을 적용하는 중입니다: " + profile.name, false);
    }

    bool shell_controller::warn_before_commit()
    {
        const std::string id { selected_repository_id() };
        if (id.empty() || commit_warned_.contains(id) || dependencies_.workspace == nullptr)
            return false;
        const auto found { identities_.find(id) };
        // 아직 읽지 못했으면 판단할 근거가 없다. 커밋을 막지 않는다.
        if (found == identities_.end() || found->second.loaded == false)
            return false;
        const repository_session* const session { std::as_const(*dependencies_.workspace).find_session(id) };
        if (session == nullptr)
            return false;
        const git_identity& identity { found->second.identity };
        const app_settings& value { settings() };
        const std::optional<std::size_t> rule { match_profile_rule(value.profile_rules, value.profiles, session->path()) };
        const user_profile* wanted { nullptr };
        if (rule.has_value())
            if (const std::optional<std::size_t> index { find_profile_index(value.profiles, value.profile_rules[*rule].profile) }; index.has_value())
                wanted = &value.profiles[*index];
        const bool missing { identity.complete() == false };
        bool differs { wanted != nullptr && identity_matches_profile(identity, *wanted) == false };
        // 이 저장소에 다른 프로필을 적용해 두었고 신원이 그대로 그것이면 규칙을 따르지 않기로 한 선택이다.
        if (differs)
            if (const std::optional<std::u8string> assigned { assigned_profile(value, session->path()) }; assigned.has_value())
                if (const std::optional<std::size_t> index { find_profile_index(value.profiles, *assigned) }; index.has_value() && identity_matches_profile(identity, value.profiles[*index]))
                    differs = false;
        if (missing == false && differs == false)
            return false;

        commit_warned_.insert(id);
        notice_state notice {};
        notice.warning = true;
        if (missing)
            notice.text = u8"작성자 설정이 없어 git이 커밋을 거절할 수 있습니다 · 지금 " + describe_identity_with_source(identity);
        else
            notice.text = u8"규칙 '" + value.profile_rules[*rule].path_prefix + u8"'은 프로필 '" + wanted->name + u8"' 권장이지만 지금 " + describe_identity_with_source(identity);
        if (wanted != nullptr)
            notice.actions.push_back(notice_action { .key = std::u8string { commit_apply_key_prefix } + wanted->name, .label = u8"'" + wanted->name + u8"' 적용 후 커밋" });
        else
            for (std::size_t index { 0 }; index < value.profiles.size() && index < offered_profile_limit; ++index)
                notice.actions.push_back(
                    notice_action { .key = std::u8string { commit_apply_key_prefix } + value.profiles[index].name, .label = u8"'" + value.profiles[index].name + u8"' 적용 후 커밋" });
        notice.actions.push_back(notice_action { .key = std::u8string { commit_anyway_key }, .label = u8"그대로 커밋" });
        ui_.notice = std::move(notice);
        return true;
    }

    bool shell_controller::handle_profile_banner_action(const std::u8string& key)
    {
        if (key.starts_with(apply_profile_key_prefix))
        {
            ui_.notice.reset();
            apply_profile(key.substr(apply_profile_key_prefix.size()));
            return true;
        }
        if (key.starts_with(commit_apply_key_prefix))
        {
            ui_.notice.reset();
            commit_after_apply_.insert(selected_repository_id());
            apply_profile(key.substr(commit_apply_key_prefix.size()));
            return true;
        }
        if (key == commit_anyway_key)
        {
            ui_.notice.reset();
            commit_now();
            return true;
        }
        return false;
    }

    bool shell_controller::handle_profile_message(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<apply_profile_intent>() }; intent != nullptr)
        {
            apply_profile(intent->name);
            return true;
        }
        if (const auto* const intent { message.get<identity_loaded_intent>() }; intent != nullptr)
        {
            identity_entry& entry { identities_[intent->repository_id] };
            entry.requested = true;
            entry.loaded = intent->ok;
            entry.identity = intent->identity;
            entry.error = intent->error;
            entry.read_at = std::chrono::steady_clock::now();
            return true;
        }
        if (const auto* const intent { message.get<identity_applied_intent>() }; intent != nullptr)
        {
            const std::string id { intent->repository_id };
            std::u8string profile {};
            if (const auto pending { applying_profiles_.find(id) }; pending != applying_profiles_.end())
            {
                profile = pending->second;
                applying_profiles_.erase(pending);
            }
            const bool commit_after { commit_after_apply_.erase(id) > 0 };
            if (intent->ok == false)
            {
                show_notice(u8"프로필을 적용하지 못했습니다 (" + profile + u8"): " + intent->message, true);
                return true;
            }
            // 어느 프로필을 골랐는지 기억한다. 그 저장소의 "이 저장소의 프로필"이고, 신원이 그대로인 동안은 규칙을 거스르는 선택으로 존중한다.
            if (dependencies_.workspace != nullptr)
                if (const repository_session* const session { std::as_const(*dependencies_.workspace).find_session(id) }; session != nullptr && profile.empty() == false)
                {
                    const std::u8string path { session->path() };
                    update_settings([&path, &profile](app_settings& value) { assign_profile(value, path, profile); });
                }
            show_notice(u8"프로필을 적용했습니다: " + profile + u8" · " + intent->message, false);
            // 새 신원을 다시 읽는다.
            identities_[id].requested = false;
            identities_[id].loaded = false;
            profile_proposed_.insert(id);
            // 새 신원은 방금 적용한 프로필이다. 같은 경고를 다시 보이지 않게 한 번 경고한 것으로 친다.
            commit_warned_.insert(id);
            if (commit_after && selected_repository_id() == id)
                commit_now();
            return true;
        }
        return false;
    }
} // namespace cyella
