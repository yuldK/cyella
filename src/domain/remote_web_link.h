#pragma once

#include "domain/hosting_remote.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 원격 URL에서 브라우저로 여는 저장소 페이지 주소를 짓는 규칙이다. API 연동(`hosting_host_setting`)과 달리 주소만 만들므로 GitHub·GitLab
    // 밖의 서비스(Gitea·Bitbucket·사내 서버)도 등록할 수 있다. 네트워크도 파일도 건드리지 않는 순수 규칙이다.

    // 링크의 서비스 종류다. 기본 아이콘과 화면에 보일 이름을 정한다.
    enum class web_link_service : std::uint8_t
    {
        github,
        gitlab,
        gitea,
        bitbucket,
        other,
    };

    // 설정 파일에 적는 이름이다 (`github`, `gitlab`, `gitea`, `bitbucket`, `other`).
    [[nodiscard]] std::string_view web_link_service_name(web_link_service service) noexcept;
    [[nodiscard]] std::optional<web_link_service> parse_web_link_service(std::string_view name) noexcept;
    // "GitHub"처럼 화면에 보이는 이름이다. `other`는 빈 값이라 호스트 이름을 대신 보인다.
    [[nodiscard]] std::string web_link_service_display_name(web_link_service service);
    // 아이콘을 비워 둔 규칙이 쓰는 codicon 이름이다. codicon에 GitHub 말고는 서비스 아이콘이 없어 나머지는 일반 아이콘이다.
    [[nodiscard]] std::string_view default_web_link_icon(web_link_service service) noexcept;

    // 설정의 "웹 링크" 표 한 줄이다.
    struct web_link_setting
    {
        // 원격 URL의 호스트다 (`git.example.com`, 포트가 있는 http(s) 서버는 `git.example.com:3000`).
        std::string host {};
        web_link_service service { web_link_service::other };
        // 저장소 경로 앞에 붙일 웹 주소다 (`https://git.example.com`). 비우면 `https://<호스트>`다. ssh 호스트와 웹 주소가 다른 서버에 쓴다.
        std::string web_base {};
        // codicon 이름이다 (`github`, `globe`, `link-external`). 비우면 서비스 종류의 기본 아이콘이다.
        std::string icon {};

        [[nodiscard]] bool operator==(const web_link_setting&) const noexcept = default;
    };

    struct remote_web_link
    {
        // `https://github.com/owner/repo`다. 원격 URL의 사용자 정보와 끝의 `.git`은 없다.
        std::string url {};
        web_link_service service { web_link_service::other };
        // 규칙을 찾은 호스트 열쇠다.
        std::string host {};
        // codicon 이름이다.
        std::string icon {};
    };

    // 원격 URL 하나의 웹 주소다. 규칙은 웹 링크 표 → 호스팅 표(GitHub·GitLab API 서버) → 내장(github.com, gitlab.com, gitea.com,
    // codeberg.org, bitbucket.org) 차례로 찾는다. 어느 규칙에도 없는 호스트는 원격이 http(s)면 그 출처, 아니면 `https://<호스트>`에 경로를
    // 붙이고 서비스는 호스트 이름으로 짐작한다. 로컬 경로면 값이 없다.
    [[nodiscard]] std::optional<remote_web_link> resolve_remote_web_link(std::string_view remote_url, const std::vector<web_link_setting>& links, const std::vector<hosting_host_setting>& hosts);

    // 내장 규칙(github.com, gitlab.com, gitea.com, codeberg.org, bitbucket.org)이다. 설정 화면이 표 위에 보인다.
    [[nodiscard]] std::vector<web_link_setting> builtin_web_links();

    // 표의 한 줄이 쓸 수 있는가. 이유가 있으면 한 줄이다. 호스트는 호스팅 표와 같은 규칙이고, 웹 주소는 비었거나 http(s) 절대 주소여야 한다.
    [[nodiscard]] std::optional<std::string> validate_web_link_setting(const web_link_setting& setting);
} // namespace cyella
