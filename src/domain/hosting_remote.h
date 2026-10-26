#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // GitHub·GitLab 연동이 원격 URL에서 읽어 내는 값들이다. 네트워크도 파일도 건드리지 않는 순수 규칙이다.

    enum class hosting_provider : std::uint8_t
    {
        unknown,
        github,
        gitlab,
    };

    // 설정 파일에 적는 이름이다 (`github`, `gitlab`, `unknown`).
    [[nodiscard]] std::string_view hosting_provider_name(hosting_provider provider) noexcept;
    // 알 수 없는 이름이면 값이 없다. `unknown`도 값이 없는 쪽이다 (설정에 쓸 수 있는 종류가 아니다).
    [[nodiscard]] std::optional<hosting_provider> parse_hosting_provider(std::string_view name) noexcept;
    // 화면에 보이는 서비스 이름이다 ("GitHub", "GitLab").
    [[nodiscard]] std::string hosting_provider_display_name(hosting_provider provider);
    // "PR"(GitHub)과 "MR"(GitLab)이다. 알 수 없으면 "PR"이다.
    [[nodiscard]] std::string_view hosting_change_request_abbreviation(hosting_provider provider) noexcept;

    // 설정의 "호스팅" 표 한 줄이다. 자체 호스팅 서버의 호스트 이름이 서비스 종류와 API 주소를 정한다.
    struct hosting_host_setting
    {
        // 호스트 이름이다 (`git.example.com`). 포트가 있는 http(s) 서버는 `127.0.0.1:8123`처럼 포트까지 적는다.
        std::string host {};
        hosting_provider provider { hosting_provider::github };
        // API의 기준 주소다 (`https://git.example.com/api/v3`). 비면 종류에 맞는 기본값을 쓴다.
        std::string api_base {};

        [[nodiscard]] bool operator==(const hosting_host_setting&) const noexcept = default;
    };

    // 원격 URL을 나눈 값이다. 사용자 정보(`user:password@`)는 여기에 남기지 않는다.
    struct remote_location
    {
        // `https`, `http`, `ssh`, `git`. scp 꼴(`git@host:owner/repo.git`)은 `ssh`다.
        std::string scheme {};
        // 소문자다. IPv6는 대괄호 없이 담는다.
        std::string host {};
        // URL에 적힌 포트다. scp 꼴에는 없다.
        std::optional<std::uint16_t> port {};
        // 앞뒤 `/`와 끝의 `.git`을 뗀 경로다 (`group/sub/repo`).
        std::string path {};

        [[nodiscard]] bool operator==(const remote_location&) const noexcept = default;
    };

    // 원격 URL 하나를 나눈다. https·http·ssh·git URL과 scp 꼴을 읽고, 로컬 경로(`C:\repo`, `/srv/repo`, `../x`)는 값이 없다.
    [[nodiscard]] std::optional<remote_location> parse_remote_url(std::string_view url);

    // 저장소가 호스팅 서비스의 어느 프로젝트인지다.
    struct hosting_repository
    {
        hosting_provider provider { hosting_provider::unknown };
        // 설정 표를 찾은 열쇠다. 포트가 있는 http(s) 서버는 `host:port`, 나머지는 호스트 이름이다. 토큰도 이 열쇠로 둔다.
        std::string host_key {};
        // `git credential`에 줄 호스트다. 포트가 있으면 `host:port`다.
        std::string credential_host {};
        // `git credential`에 줄 프로토콜이다 (API 주소의 scheme).
        std::string credential_protocol { "https" };
        // 네임스페이스다. GitHub는 소유자(`owner`), GitLab은 `group/subgroup`이다.
        std::string owner {};
        std::string name {};
        std::string api_base {};
        // 브라우저에서 여는 사이트의 기준 주소다 (`https://github.com`).
        std::string web_base {};
        // 이 정보를 읽어 낸 원격 이름이다.
        std::string remote_name {};
        // 설정 표에서 온 호스트인가 (내장 github.com·gitlab.com이 아니다).
        bool configured { false };

        // `owner/name`이다.
        [[nodiscard]] std::string full_name() const;
        // 알려진 서비스(GitHub·GitLab)인가.
        [[nodiscard]] bool supported() const noexcept
        {
            return provider != hosting_provider::unknown;
        }
        // 프로젝트의 웹 주소다 (`https://github.com/owner/repo`).
        [[nodiscard]] std::string web_url() const;
    };

    // 원격 URL을 호스팅 프로젝트로 푼다. URL을 읽을 수 없으면 값이 없고, 읽었지만 서비스를 모르면 `provider == unknown`이다.
    // 설정 표가 내장 호스트(github.com·gitlab.com)보다 먼저다 — 사용자가 같은 이름을 적었다면 그것이 뜻이다.
    [[nodiscard]] std::optional<hosting_repository> resolve_hosting_repository(std::string_view url, std::string_view remote_name, const std::vector<hosting_host_setting>& hosts);

    // 원격이 아직 없는 저장소를 게시할 때 쓰는 호스트 문맥이다. 소유자·이름은 비어 있고 나머지(API·웹 주소, 자격 증명 호스트, 토큰 열쇠)는
    // 그 호스트에 만든 저장소의 원격 URL을 `resolve_hosting_repository`로 푼 값과 같다 — 게시한 뒤 PR·CI가 같은 로그인을 쓴다.
    // 서비스를 모르는 호스트면 값이 없다.
    [[nodiscard]] std::optional<hosting_repository> hosting_repository_for_host(std::string_view host, const std::vector<hosting_host_setting>& hosts);

    // 호스팅 호출이 쓰는 문맥이다: 어느 호스팅 프로젝트이고, 어느 저장소 폴더의 git 설정으로 자격 증명을 물을지.
    struct hosting_context
    {
        hosting_repository repository {};
        std::string working_directory {};
    };

    // 저장소가 가진 원격들(이름, URL) 가운데 연동할 하나를 고른다. `origin`이 먼저이고 없으면 서비스를 아는 첫 원격이다.
    struct hosting_remote_candidate
    {
        std::string name {};
        std::string url {};
    };
    [[nodiscard]] std::optional<hosting_repository> pick_hosting_repository(const std::vector<hosting_remote_candidate>& remotes, const std::vector<hosting_host_setting>& hosts);

    // 설정 표의 한 줄이 쓸 수 있는가. 이유가 있으면 한글 한 줄이다. 호스트는 호스트 이름이나 `이름:포트`여야 하고, API 주소는 http(s) 절대 주소여야 한다.
    [[nodiscard]] std::optional<std::string> validate_hosting_host_setting(const hosting_host_setting& setting);
    // 호스트 표기를 비교용으로 다듬는다 (앞뒤 공백, 소문자).
    [[nodiscard]] std::string normalize_hosting_host(std::string_view host);
} // namespace cyella
