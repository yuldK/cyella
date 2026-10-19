#pragma once

#include <filesystem>
#include <optional>

namespace cyella::win32 {
    inline constexpr const wchar_t* askpass_helper_file_name { L"cyella-askpass.exe" };

    // 앱 실행 파일이 있는 디렉터리 옆의 도우미 경로다. 설치 규칙이 둘을 같은 디렉터리(`bin`)에 둔다.
    // 파일 존재는 확인하지 않는다.
    [[nodiscard]] std::filesystem::path askpass_helper_path_beside(const std::filesystem::path& executable_directory);

    // 실행 중인 프로세스의 실행 파일을 기준으로 도우미를 찾는다. 파일이 없거나 경로를 알 수 없으면
    // 값이 없다. 이때 호출자는 askpass 없이 git을 실행해 인증이 필요한 작업이 즉시 실패하게 둔다.
    [[nodiscard]] std::optional<std::filesystem::path> locate_askpass_helper();
} // namespace cyella::win32
