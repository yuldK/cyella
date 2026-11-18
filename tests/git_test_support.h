#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// write_file·read_file은 temporary_directory_support.h가 정의한다.
#include "temporary_directory_support.h"

namespace cyella {
    // git 해석기 test가 함께 쓰는 도구다. 앱 코드가 아니므로 앱의 process runner에
    // 기대지 않고 CreateProcess로 직접 git을 실행한다.
    namespace test {
        // tests/data/git/<name>의 바이트를 그대로 읽는다. 없으면 예외를 던진다.
        [[nodiscard]] std::string read_fixture(std::string_view name);

        struct git_run_result
        {
            int exit_code { -1 };
            std::string standard_output {};
            std::string standard_error {};
        };

        // 프로세스 환경을 사용자 전역·시스템 git 설정과 지역화에서 끊고 작성자·날짜를 고정한다(GIT_CONFIG_GLOBAL=NUL 등).
        // 자식 프로세스가 이 환경을 상속한다. 처음 한 번만 적용하고 `run_git`이 먼저 부르지만, 앱의 process runner로 git을
        // 직접 띄우는 test는 사용자 설정(credential.username 등)에 노출되지 않도록 이것을 먼저 불러야 한다.
        void isolate_git_environment();

        // 사용자 전역 설정과 지역화를 끊은 환경으로 git을 실행한다. 작성자·날짜가 고정되어
        // 같은 입력에서 같은 SHA가 나온다. 120초 안에 끝나지 않으면 git을 끝내고 `exit_code`를 -1로 돌려준다.
        git_run_result run_git(const std::filesystem::path& working_directory, const std::vector<std::u8string>& arguments, std::string_view standard_input = {});

        // 같은 함수의 편의 판이다. 인자는 ASCII 문자열 목록이다.
        git_run_result run_git(const std::filesystem::path& working_directory, const std::vector<std::string>& arguments, std::string_view standard_input = {});

        [[nodiscard]] bool git_is_available();

        // u8 인자 목록을 Catch2가 출력할 수 있는 문자열 목록으로 바꾼다.
        [[nodiscard]] std::vector<std::string> to_text(const std::vector<std::u8string>& arguments);

        // 임시 폴더를 만들고 소멸할 때 지운다. git 객체 파일은 읽기 전용이라 속성을
        // 풀고 지운다.
        class temp_directory
        {
        public:
            temp_directory();
            temp_directory(const temp_directory&) = delete;
            temp_directory& operator=(const temp_directory&) = delete;
            ~temp_directory();

            [[nodiscard]] const std::filesystem::path& path() const noexcept
            {
                return path_;
            }

        private:
            std::filesystem::path path_ {};
        };

    } // namespace test
} // namespace cyella
