#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // ssh 원격 URL이 가리키는 서버와 경로다. 서버에 빈 저장소를 만들 때 쓴다.
    //
    // `parse_remote_url`(호스팅 연결용)과 달리 계정과 경로를 git이 `git-receive-pack`에 넘기는 모양 그대로 둔다.
    // 앞 `/`(절대 경로인가)와 끝 `.git`이 서버에서 다른 폴더를 뜻하기 때문이다.
    struct ssh_remote_target
    {
        // URL에 적힌 계정이다. 비면 ssh 설정(`~/.ssh/config`의 `User`, 없으면 로컬 계정 이름)을 따른다.
        std::string user {};
        // IPv6는 대괄호 없이 담는다.
        std::string host {};
        // URL에 적힌 포트다. scp 꼴에는 없다.
        std::optional<std::uint16_t> port {};
        // 서버에서의 경로다. `/`로 시작하면 절대 경로, `~` 또는 `~계정`으로 시작하면 그 홈 기준, 나머지는 로그인 홈 기준이다.
        std::string path {};

        // ssh에 줄 목적지다 (`user@host` 또는 `host`).
        [[nodiscard]] std::string destination() const;

        [[nodiscard]] bool operator==(const ssh_remote_target&) const noexcept = default;
    };

    // `ssh://[user@]host[:port]/path`와 scp 꼴 `[user@]host:path`를 git의 규칙대로 읽는다.
    //  - `ssh://host/srv/x.git`의 경로는 `/srv/x.git`, `ssh://host/~/x.git`과 `ssh://host/~me/x.git`은 앞 `/`를 뗀 `~/x.git`·`~me/x.git`이다.
    //  - `host:x.git`은 로그인 홈 기준 `x.git`, `host:/srv/x.git`은 절대 경로다.
    // https·로컬 경로, 계정·호스트·경로가 옵션처럼 보이거나(`-`로 시작) 셸에서 뜻이 바뀔 글자를 담은 값은 값이 없다.
    [[nodiscard]] std::optional<ssh_remote_target> parse_ssh_remote_target(std::string_view url);
} // namespace cyella
