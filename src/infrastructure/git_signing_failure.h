#pragma once

#include "domain/operation_status.h"

#include <string>
#include <string_view>

namespace cyella {
    // 서명 실패(`signing_*`)의 배너 한 줄이다. gpg와 ssh-keygen은 고칠 곳이 달라서(gpg-agent·pinentry / ssh-agent·passphrase)
    // stderr를 보고 어느 쪽인지 가려 알려 준다. 서명 실패가 아니면 빈 글이다 (호출자가 기본 문구를 쓴다).
    [[nodiscard]] std::u8string signing_failure_summary(failure_kind kind, std::string_view standard_error);
} // namespace cyella
