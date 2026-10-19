#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>

namespace cyella {
    // 이미지 비교와 외부 diff 도구가 쓰는 임시 파일의 자리다. 프로세스마다 하위 폴더 하나를 만들고, 그 안에
    // diff 작업마다 폴더를 하나씩 둔다.
    //
    // 외부 도구는 앱이 끝난 뒤에도 파일을 열어 둘 수 있고, 이미지는 웹뷰가 읽는 동안 있어야 하므로 작업이
    // 끝나는 때가 아니라 **앱이 끝날 때**(소멸자) 한꺼번에 지운다. 비정상 종료로 남은 이전 프로세스의 폴더는
    // 다음 실행의 생성자가 오래된 것부터 지운다.
    class diff_temp_directory
    {
    public:
        // root가 비어 있으면 `%TEMP%\cyella-diff`다.
        explicit diff_temp_directory(std::filesystem::path root = {});
        diff_temp_directory(const diff_temp_directory&) = delete;
        diff_temp_directory& operator=(const diff_temp_directory&) = delete;
        ~diff_temp_directory();

        // 새 작업 폴더를 만들어 절대 경로를 돌려준다. 만들지 못하면 빈 경로다. 여러 스레드에서 불러도 된다.
        [[nodiscard]] std::filesystem::path create_operation_directory();

        // 이 프로세스가 만든 폴더를 모두 지운다. 실패한 것은 무시한다.
        void remove_all() noexcept;

        // 이 프로세스의 하위 폴더다.
        [[nodiscard]] const std::filesystem::path& process_directory() const noexcept
        {
            return process_directory_;
        }

        // root 아래에서 다른 프로세스가 남긴 폴더 중 마지막 수정이 max_age보다 오래된 것을 지운다.
        static void sweep_stale(const std::filesystem::path& root, std::chrono::hours max_age) noexcept;

    private:
        std::filesystem::path root_ {};
        std::filesystem::path process_directory_ {};
        std::mutex mutex_ {};
        std::uint64_t counter_ { 0 };
    };
} // namespace cyella
