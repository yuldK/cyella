#include "infrastructure/command_line_builder.h"
#include "infrastructure/secret_masking.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cyella;

TEST_CASE("masking hides the password of a URL in a command line", "[masking]")
{
    CHECK(mask_secrets(u8"git clone https://user:token@host/repo.git target") == u8"git clone https://user:***@host/repo.git target");
}

TEST_CASE("masking hides a token used as the whole user info", "[masking]")
{
    CHECK(mask_secrets(u8"fetch https://ghp_abcdef123456@github.com/o/r.git") == u8"fetch https://***@github.com/o/r.git");
}

TEST_CASE("masking handles an at sign inside the password", "[masking]")
{
    CHECK(mask_secrets(u8"https://user:p@ss@host/path") == u8"https://user:***@host/path");
}

TEST_CASE("masking leaves a URL without credentials unchanged", "[masking]")
{
    CHECK(mask_secrets(u8"https://github.com/o/r.git") == u8"https://github.com/o/r.git");
    // ssh의 계정 이름은 비밀이 아니다. 비밀번호가 붙은 경우만 가린다.
    CHECK(mask_secrets(u8"ssh://git@github.com/o/r.git") == u8"ssh://git@github.com/o/r.git");
    CHECK(mask_secrets(u8"ssh://git:secret@github.com/o/r.git") == u8"ssh://git:***@github.com/o/r.git");
}

TEST_CASE("masking hides option values and headers", "[masking]")
{
    CHECK(mask_secrets(u8"tool --password hunter2 --verbose") == u8"tool --password *** --verbose");
    CHECK(mask_secrets(u8"tool --token=abc123") == u8"tool --token=***");
    CHECK(mask_secrets(u8"Authorization: Bearer abc.def.ghi") == u8"Authorization: ***");
}

TEST_CASE("masking is idempotent", "[masking]")
{
    const std::u8string once { mask_secrets(u8"https://user:token@host/x --password secret") };

    CHECK(mask_secrets(once) == once);
}

TEST_CASE("a quoted command line keeps arguments intact", "[masking][command_line]")
{
    const std::vector<std::u8string> arguments { u8"clone", u8"path with space", u8"", u8"a\"b", u8"trail \\" };

    CHECK(build_windows_command_line(u8"C:\\Git\\git.exe", arguments) == u8"\"C:\\Git\\git.exe\" clone \"path with space\" \"\" \"a\\\"b\" \"trail \\\\\"");
}
