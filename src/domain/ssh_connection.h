#pragma once

#include <string>

namespace cyella {
    // `ssh -T` 연결 시험의 분류다. 환경 설정의 SSH 화면과 operation 사건이 같은 값을 쓰도록 domain에 둔다.
    enum class ssh_connection_result
    {
        // 종료 코드 0, 또는 1이면서 "successfully authenticated"처럼 인증은 성공했다고 알리는 경우다.
        // GitHub는 셸을 주지 않아 인증에 성공해도 1로 끝난다.
        authenticated,
        permission_denied,
        host_key_rejected,
        host_key_changed,
        connection_failed,
        unknown,
    };

    struct ssh_connection_classification
    {
        ssh_connection_result result { ssh_connection_result::unknown };
        // 서버가 인사말에 적은 계정 이름이다 (`Hi alice!`). 알 수 없으면 비어 있다.
        std::u8string authenticated_user {};

        [[nodiscard]] bool operator==(const ssh_connection_classification&) const noexcept = default;
    };

    // 사용자에게 보일 한 줄이다 (결과와 다음에 할 일).
    [[nodiscard]] std::u8string describe_ssh_connection(const ssh_connection_classification& classification);
} // namespace cyella
