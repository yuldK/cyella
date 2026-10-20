#pragma once

#include "domain/diff_file.h"
#include "presentation/view_snapshot.h"

#include <optional>

namespace cyella {
    // 텍스트 diff로 보일 수 없는 변경을 카드로 바꾼다. 순수 함수다.
    //
    // - submodule 포인터(gitlink, 모드 160000)는 `Subproject commit` 두 줄 대신 옛·새 커밋을 보인다.
    // - Git LFS 포인터는 포인터 세 줄 대신 옛·새 크기와 oid, 실제 내용이 로컬에 있는지를 보인다. 두 쪽의 실제 내용이 모두
    //   로컬에 있으면(`lfs->all_local()`) 카드가 아니라 이미지·바이너리 비교가 맡으므로 값이 없다.
    //
    // 둘 다 아니면 값이 없다. 조건은 `diff_file::is_special`과 같고, `diff_view_controller`가 같은 규칙으로 웹뷰 내용을 비운다.
    [[nodiscard]] std::optional<diff_special_view> build_diff_special(const diff_file& file);
} // namespace cyella
