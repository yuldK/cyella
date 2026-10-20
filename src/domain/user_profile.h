#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    struct app_settings;

    // 저장소별로 쓰는 사용자 신원이다. 이름·`user.name`·`user.email`은 [적용]이 `git config --local`로 저장소에 쓴다.
    // 서명 키는 커밋 서명 기능의 몫이라 앱은 뜻을 모르는 글 그대로 보관한다. 비밀번호·토큰은 두지 않는다.
    struct user_profile
    {
        // 목록에서 프로필을 가리키는 이름이다 (대소문자를 가리지 않고 유일하다).
        std::u8string name {};
        std::u8string user_name {};
        std::u8string user_email {};
        std::u8string signing_key {};

        [[nodiscard]] bool operator==(const user_profile&) const noexcept = default;
    };

    // 저장소를 열 때 프로필을 **제안**하는 규칙이다. 경로가 접두에 들어가면(폴더 경계 기준) 그 프로필을 권한다. 설정을 쓰지는 않는다.
    struct profile_rule
    {
        std::u8string path_prefix {};
        // `user_profile::name`이다.
        std::u8string profile {};

        [[nodiscard]] bool operator==(const profile_rule&) const noexcept = default;
    };

    // 저장소에 적용하기로 한 프로필이다. 적용한 선택을 기억해 두었다가 "이 저장소의 프로필"로 보여 준다.
    struct profile_assignment
    {
        // `normalize_repository_path`한 작업 트리 경로다. 비교는 `repository_path_key`로 한다.
        std::u8string path {};
        std::u8string profile {};

        [[nodiscard]] bool operator==(const profile_assignment&) const noexcept = default;
    };

    inline constexpr std::size_t maximum_profile_name_length { 40 };

    // 이름으로 프로필을 찾는다 (ASCII 대소문자를 가리지 않는다). 없으면 값이 없다.
    [[nodiscard]] std::optional<std::size_t> find_profile_index(const std::vector<user_profile>& profiles, std::u8string_view name);

    // 입력한 프로필의 문제다. 비어 있으면 저장할 수 있다. `editing`은 고치는 줄이다 (자기 이름과의 중복은 허용한다).
    [[nodiscard]] std::u8string check_user_profile(const user_profile& profile, const std::vector<user_profile>& existing, std::optional<std::size_t> editing);

    // 경로가 접두 안에 있는가 (폴더 경계로 맞춘다: `E:\work`는 `E:\work\a`에 맞고 `E:\workshop`에는 맞지 않는다. 대소문자·구분자 무시).
    [[nodiscard]] bool path_is_under_prefix(std::u8string_view path, std::u8string_view prefix);

    // 저장소 경로에 맞는 규칙이다. 접두가 가장 긴 규칙이 이기고 같으면 앞의 규칙이다. 빈 접두와 없는 프로필을 가리키는 규칙은 건너뛴다.
    [[nodiscard]] std::optional<std::size_t> match_profile_rule(const std::vector<profile_rule>& rules, const std::vector<user_profile>& profiles, std::u8string_view repository_path);

    // 규칙 입력의 문제다. 비어 있으면 더할 수 있다.
    [[nodiscard]] std::u8string check_profile_rule(const profile_rule& rule, const std::vector<user_profile>& profiles);

    // ---- 설정을 고친다. 모두 `app_settings_service::update` 안에서 부른다.
    // `index`에 값이 있으면 그 줄을 바꾸고 없으면 끝에 더한다. 이름을 바꾸면 그 프로필을 가리키던 규칙과 적용 기록도 새 이름으로 따라간다.
    void upsert_user_profile(app_settings& settings, std::optional<std::size_t> index, user_profile profile);
    // 프로필을 지우고 그것을 가리키던 규칙과 적용 기록도 지운다. 지웠으면 true다.
    bool remove_user_profile(app_settings& settings, std::size_t index);
    // 같은 접두의 규칙이 있으면 프로필만 바꾼다.
    void upsert_profile_rule(app_settings& settings, profile_rule rule);
    bool remove_profile_rule(app_settings& settings, std::size_t index);
    // 저장소에 적용한 프로필을 기억한다 (같은 저장소면 바꾼다).
    void assign_profile(app_settings& settings, std::u8string_view repository_path, std::u8string profile);
    // 적용 기록에서 저장소의 프로필 이름이다.
    [[nodiscard]] std::optional<std::u8string> assigned_profile(const app_settings& settings, std::u8string_view repository_path);
} // namespace cyella
