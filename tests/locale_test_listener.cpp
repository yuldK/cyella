// test 실행 파일도 앱처럼 실린 문구 파일로 카탈로그를 정한다. 문구를 비교하는 test가 한국어 원문을 본다.
#include "platform/win32/win32_locale.h"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

namespace {
    class korean_text_listener final : public Catch::EventListenerBase
    {
    public:
        using Catch::EventListenerBase::EventListenerBase;

        void testRunStarting(const Catch::TestRunInfo&) override
        {
            cyella::win32::install_ui_language(cyella::ui_language::korean);
        }
    };
} // namespace

CATCH_REGISTER_LISTENER(korean_text_listener)
