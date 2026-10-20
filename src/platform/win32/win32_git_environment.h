#pragma once

#include <string>
#include <vector>

namespace cyella::win32 {
    // git이 사용자 전역 설정으로 읽는 파일의 후보 경로다. `GIT_CONFIG_GLOBAL`이 있으면 그것 하나이고, 없으면 `$HOME/.gitconfig`와
    // `$XDG_CONFIG_HOME/git/config`(없으면 `$HOME/.config/git/config`)다. `HOME`이 없으면 `USERPROFILE`을 쓴다.
    // 작성자 신원이 전역 파일에서 왔는지 `includeIf`로 끌어온 다른 파일에서 왔는지 가르는 데 쓴다. 읽기만 하고 파일을 건드리지 않는다.
    [[nodiscard]] std::vector<std::u8string> global_git_config_files();
} // namespace cyella::win32
