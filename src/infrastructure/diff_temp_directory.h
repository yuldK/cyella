#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace cyella {
    // 이미지 비교와 외부 diff 도구가 쓰는 임시 파일의 자리다. 프로세스마다 하위 폴더 하나를 만들고, 그 안에
    // diff 작업마다 폴더를 하나씩 둔다.
    //
    // 외부 도구는 앱이 끝난 뒤에도 파일을 열어 둘 수 있으므로 그 폴더는 작업이 끝나는 때가 아니라 **앱이 끝날 때**
    // (소멸자) 한꺼번에 지운다. 이미지 미리보기의 폴더는 웹뷰가 보이는 동안만 있으면 되므로 다음 미리보기를 준비할 때
    // `remove_operation_directory`로 지운다(오래 훑어도 세션 동안 쌓이지 않게). 비정상 종료로 남은 이전 프로세스의
    // 폴더는 다음 실행의 생성자가 오래된 것부터 지운다.
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

        // `create_operation_directory`가 만든 작업 폴더 하나를 지운다. 이 프로세스 폴더의 바로 아래가 아니면
        // 건드리지 않는다. 열려 있어 지우지 못한 파일은 앱이 끝날 때의 `remove_all`이 다시 지운다.
        void remove_operation_directory(const std::filesystem::path& directory) noexcept;

        // 이 프로세스의 하위 폴더다.
        [[nodiscard]] const std::filesystem::path& process_directory() const noexcept
        {
            return process_directory_;
        }

        // root 아래에서 다른 프로세스가 남긴 폴더 중 마지막 수정이 max_age보다 오래된 것을 지운다. 아직 살아 있는
        // 프로세스의 폴더는 그 프로세스가 열어 둔 잠금 파일을 지울 수 없으므로 오래돼도 남긴다.
        static void sweep_stale(const std::filesystem::path& root, std::chrono::hours max_age) noexcept;

    private:
        std::filesystem::path root_ {};
        std::filesystem::path process_directory_ {};
        std::mutex mutex_ {};
        std::uint64_t counter_ { 0 };
        // 프로세스 폴더 안의 잠금 파일이다. 열려 있는 동안 다른 프로세스가 지우지 못한다 (지우기 공유 없이 연다).
        std::ofstream lock_ {};
    };
} // namespace cyella
