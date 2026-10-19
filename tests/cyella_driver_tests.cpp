#include "platform/win32/cyella_driver.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("cyella_driver completes shutdown after the close message", "[driver]")
{
    cyella::win32::cyella_driver driver {};
    REQUIRE_FALSE(driver.shutdown_completed());

    driver.handle(driver.make_close_message());

    CHECK(driver.shutdown_completed());
}
