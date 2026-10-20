#pragma once

#include "application/file_probe.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cyella {
    // 저장소 작업이 git 밖에서 만지는 작은 파일 연산이다. 진행 중 rebase의 단계 파일
    // (`rebase-merge/msgnum`), `.gitignore` 덧붙이기, 남은 `index.lock` 삭제가 쓴다. 계약으로
    // 떼어 두면 실행기를 실제 디스크 없이 가짜 파일로 test할 수 있다.
    //
    // 경로는 모두 절대 경로이며 `/`와 `\`를 모두 받는다. 여러 스레드에서 동시에 불려도 안전해야 한다.
    class workspace_files : public file_probe
    {
    public:
        // 파일 전체를 읽는다. 없거나 `maximum_bytes`보다 크면 값이 없다.
        [[nodiscard]] virtual std::optional<std::string> read_text(std::u8string_view absolute_path, std::size_t maximum_bytes) const noexcept = 0;
        // 파일 끝에 덧붙인다. 없으면 만든다. 실패하면 false다.
        [[nodiscard]] virtual bool append_text(std::u8string_view absolute_path, std::string_view text) noexcept = 0;
        // 파일 하나를 지운다. 없으면 false다.
        [[nodiscard]] virtual bool remove_file(std::u8string_view absolute_path) noexcept = 0;
        // 파일들을 휴지통으로 보낸다(되돌릴 수 있다). 하나라도 실패하거나 지원하지 않으면 false다. 기본은 지원하지 않는다.
        [[nodiscard]] virtual bool move_to_recycle_bin(std::span<const std::u8string> absolute_paths) noexcept
        {
            static_cast<void>(absolute_paths);
            return false;
        }
    };
} // namespace cyella
