#pragma once

#include "application/diff_sides.h"
#include "application/process_runner.h"
#include "infrastructure/diff_temp_directory.h"

#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // diff_git_gateway의 실제 구현이다. blob은 `git cat-file`로 꺼내 파일로 흘려 쓰고(큰 이미지를 메모리에
    // 모으지 않는다), 작업 트리 파일은 복사한다.
    //
    // runner와 temp는 이 객체보다 오래 살아야 한다. 여러 스레드에서 불러도 된다.
    class git_diff_gateway final : public diff_git_gateway
    {
    public:
        git_diff_gateway(process_runner& runner, std::u8string git_executable, diff_temp_directory& temp) noexcept
            : runner_ { runner }
            , git_executable_ { std::move(git_executable) }
            , temp_ { temp }
        {}

        [[nodiscard]] extracted_sides extract(const diff_side_plan& plan, bool write_files) override;
        [[nodiscard]] std::optional<std::string> configured_diff_tool(std::u8string_view repository_path) override;

    private:
        process_runner& runner_;
        std::u8string git_executable_;
        diff_temp_directory& temp_;
    };
} // namespace cyella
