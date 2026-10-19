#include "domain/ssh_connection.h"

namespace cyella {
    std::u8string describe_ssh_connection(const ssh_connection_classification& classification)
    {
        switch (classification.result)
        {
        case ssh_connection_result::authenticated:
            if (classification.authenticated_user.empty())
                return u8"인증에 성공했습니다.";
            return u8"인증에 성공했습니다. 서버가 확인한 계정: " + classification.authenticated_user;
        case ssh_connection_result::permission_denied:
            return u8"접근이 거부되었습니다 (Permission denied). 이 키의 공개 키를 서버에 등록했는지 확인하세요.";
        case ssh_connection_result::host_key_rejected:
            return u8"호스트 키를 확인하지 않아 연결하지 않았습니다.";
        case ssh_connection_result::host_key_changed:
            return u8"서버의 호스트 키가 바뀌었습니다. known_hosts의 해당 줄을 확인하세요.";
        case ssh_connection_result::connection_failed:
            return u8"서버에 연결하지 못했습니다. 호스트 이름과 네트워크를 확인하세요.";
        case ssh_connection_result::unknown:
            break;
        }
        return u8"연결 결과를 분류하지 못했습니다. 작업 로그의 명령 결과를 확인하세요.";
    }
} // namespace cyella
