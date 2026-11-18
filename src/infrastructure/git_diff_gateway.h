#pragma once

#include "application/diff_sides.h"
#include "application/process_runner.h"
#include "infrastructure/diff_temp_directory.h"

#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // 저장소 경로(UTF-8)를 임시 폴더에 둘 파일 하나의 이름으로 바꾼다. 마지막 이름만 남기고(`/`와 `\` 둘 다로
    // 자른다), Windows가 경로나 장치로 읽는 글자와 이름을 바꿔 임시 폴더 밖이나 장치를 가리키지 못하게 한다.
    // 커밋 안의 이름은 다른 OS에서 만든 남의 것일 수 있어서다. 확장자는 남긴다 (도구와 이미지 판단이 쓴다).
    [[nodiscard]] std::string safe_temp_file_name(std::string_view path);

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
        [[nodiscard]] extracted_sides extract_cancellable(const diff_side_plan& plan, bool write_files, const process_cancellation_token& cancellation) override;
        [[nodiscard]] std::optional<std::string> configured_diff_tool(std::u8string_view repository_path) override;
        [[nodiscard]] std::optional<std::string> empty_tree(std::u8string_view repository_path) override;
        // 이 프로세스의 임시 폴더 바로 아래 작업 폴더만 지운다. 다른 곳을 가리키는 경로는 건드리지 않는다.
        void release(const extracted_sides& sides) noexcept override;

    private:
        process_runner& runner_;
        std::u8string git_executable_;
        diff_temp_directory& temp_;
    };
} // namespace cyella
