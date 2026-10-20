// 통합 테스트의 임시 폴더(`temp_directory`)가 이전 실행의 잔해를 만나도 깨끗한 새 폴더를 주는지 본다.
//
// 배경: 폴더 이름이 `cyella_git_<프로세스 id>_<번호>`뿐이었다. ctest는 test마다 새 프로세스를 띄우고 Windows는 프로세스 id를 빨리
// 다시 쓰므로, 이전 프로세스가 지우지 못하고 죽은 폴더(타임아웃, 강제 종료)와 같은 이름을 새 프로세스가 받을 수 있다. 그러면
// `git clone ... one`이 "이미 있고 비어 있지 않다"로 실패한다. 병렬(-j 8)에서만 가끔 실패하던 `remote commands run against a local
// bare repository`가 이 경우다 (그 test가 `one`·`origin.git`·`two`를 만든다).
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

TEST_CASE("a fresh temp directory is empty even when a crashed process left folders with the same process id", "[git][support]")
{
    const std::filesystem::path root { std::filesystem::temp_directory_path() };
    const std::string prefix { "cyella_git_" + std::to_string(::GetCurrentProcessId()) + "_" };

    // 이전 프로세스가 같은 id로 남긴 잔해를 흉내 낸다: 번호가 겹칠 만한 이름마다 내용이 든 폴더를 둔다.
    std::vector<std::filesystem::path> leftovers {};
    for (int number { 0 }; number < 300; ++number)
    {
        const std::filesystem::path leftover { root / (prefix + std::to_string(number)) };
        if (std::filesystem::exists(leftover))
            continue;
        std::filesystem::create_directories(leftover / "one");
        cyella::test::write_file(leftover / "origin.git" / "HEAD", "ref: refs/heads/main\n");
        leftovers.push_back(leftover);
    }

    {
        const cyella::test::temp_directory fresh {};
        INFO("path: " << fresh.path().string());
        CHECK(std::filesystem::is_directory(fresh.path()));
        CHECK(std::filesystem::is_empty(fresh.path()));
        CHECK(std::ranges::find(leftovers, fresh.path()) == leftovers.end());
        // 같은 프로세스에서 연달아 만든 폴더도 서로 다르다.
        const cyella::test::temp_directory second {};
        CHECK(second.path() != fresh.path());
        CHECK(std::filesystem::is_empty(second.path()));
    }

    std::error_code ignored {};
    for (const std::filesystem::path& leftover : leftovers)
        std::filesystem::remove_all(leftover, ignored);
}
