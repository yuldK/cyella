#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // Git LFS 포인터 파일의 내용이다. 저장소에는 큰 파일 대신 이 세 줄짜리 텍스트가 들어간다.
    //
    //   version https://git-lfs.github.com/spec/v1
    //   oid sha256:<64자리 16진수>
    //   size <바이트 수>
    struct lfs_pointer
    {
        // `sha256:` 뒤의 해시다. 로컬 객체 저장소의 경로(`lfs/objects/ab/cd/<oid>`)가 된다.
        std::string oid {};
        std::uint64_t size { 0 };

        [[nodiscard]] bool operator==(const lfs_pointer&) const noexcept = default;
    };

    // 포인터 파일 하나의 내용이다. 포인터 모양이 아니면(첫 줄이 `version https://git-lfs`가 아니거나 oid·size가
    // 없거나 너무 크면) 값이 없다. 줄 끝은 `\n`과 `\r\n` 모두 받는다.
    [[nodiscard]] std::optional<lfs_pointer> parse_lfs_pointer(std::string_view text);

    // LFS가 추적하는 파일 하나다 (`git lfs ls-files`).
    struct lfs_file
    {
        std::string path {};
        std::string oid {};
        std::uint64_t size { 0 };
        // 객체가 로컬 `lfs/objects`에 있다.
        bool downloaded { false };
        // 작업 트리에 포인터가 아니라 실제 내용이 있다.
        bool checked_out { false };

        [[nodiscard]] bool operator==(const lfs_file&) const noexcept = default;
    };

    // 저장소 하나의 LFS 상태다. refresh가 읽는다.
    struct lfs_repository_state
    {
        // 이 저장소가 LFS를 쓴다(`.gitattributes`의 `filter=lfs`). 거짓이면 아래는 비어 있다.
        bool in_use { false };
        // `git lfs`를 실행할 수 있다.
        bool available { false };
        // `git-lfs/3.7.1` 같은 버전 문구다. 모르면 비어 있다.
        std::string version {};
        // 이 저장소에 `filter.lfs.*` 설정(`git lfs install`)이 있다. 없으면 pull은 아무것도 하지 않는다.
        bool installed { false };
        std::vector<lfs_file> files {};

        [[nodiscard]] std::size_t missing_count() const noexcept
        {
            std::size_t count { 0 };
            for (const lfs_file& file : files)
                if (file.checked_out == false)
                    ++count;
            return count;
        }
    };

    // diff 한 파일에 대한 LFS 정보다. 포인터 파일의 diff일 때만 채운다.
    struct lfs_diff_info
    {
        std::optional<lfs_pointer> old_pointer {};
        std::optional<lfs_pointer> new_pointer {};
        // 각 쪽의 실제 내용이 로컬에 있다. 쪽이 없으면(추가·삭제) 참이다.
        bool old_local { true };
        bool new_local { true };

        [[nodiscard]] bool all_local() const noexcept
        {
            return old_local && new_local;
        }
    };

    // 바이트 수를 사람이 읽는 말로 바꾼다(`5.0 KB`, `3.0 MB`). git-lfs의 표기와 같이 1000 단위다.
    [[nodiscard]] std::string format_byte_size(std::uint64_t bytes);
} // namespace cyella
