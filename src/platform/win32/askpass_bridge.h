#pragma once

#include "application/askpass_service.h"
#include "application/operation_events.h"
#include "domain/diagnostic.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace cyella::win32 {
    // 앱 실행마다 하나인 askpass 서버와, 그것을 operation 사건으로 잇는 다리다. 구동기가 소유한다.
    //
    // - `start`가 서버를 한 번 열고 도우미 경로를 보관한다. 도우미나 서버가 없으면 `available()`이
    //   false이고 진단이 하나 남는다. 호출한 쪽은 askpass 없이 git을 실행한다(인증이 필요하면 즉시 실패).
    // - 서버가 질문을 받으면 연결 스레드가 `operation_input_requested` 사건을 `post`로 보낸다.
    //   답 callback이 사건에 실려 logic thread로 가고, 답은 `git_workspace::answer_input`이 부른다.
    // - `shutdown`은 대기 질문을 모두 취소로 답하고 서버를 닫는다. scheduler를 멈추기 **전에** 불러야
    //   프롬프트를 기다리는 git과 도우미가 종료를 붙잡지 않는다. 반복 호출해도 안전하다.
    class askpass_bridge
    {
    public:
        // `post`는 서버의 연결 스레드에서 불린다 (relay의 `post`). 어느 스레드에서나 안전해야 한다.
        explicit askpass_bridge(operation_event_sink post);
        askpass_bridge(const askpass_bridge&) = delete;
        askpass_bridge& operator=(const askpass_bridge&) = delete;
        ~askpass_bridge();

        // `helper`는 `locate_askpass_helper()`의 결과다. 값이 없으면 서버를 열지 않는다. 한 번만 효과가
        // 있다.
        void start(std::optional<std::filesystem::path> helper);

        // 도우미와 서버가 모두 준비됐다.
        [[nodiscard]] bool available() const noexcept
        {
            return server_ != nullptr && helper_path_.empty() == false;
        }

        // 준비되지 않았으면 null이다.
        [[nodiscard]] askpass_service* service() noexcept
        {
            return server_.get();
        }

        // 도우미 실행 파일의 절대 경로(`/` 구분)다. 준비되지 않았으면 비어 있다.
        [[nodiscard]] const std::u8string& helper_path() const noexcept
        {
            return helper_path_;
        }

        // pipe 이름과 토큰이다. 준비되지 않았으면 비어 있다.
        [[nodiscard]] askpass_endpoint endpoint() const;

        // 시작하지 못한 이유다. 준비됐으면 값이 없다.
        [[nodiscard]] const std::optional<diagnostic>& startup_diagnostic() const noexcept
        {
            return startup_diagnostic_;
        }

        void shutdown() noexcept;

    private:
        operation_event_sink post_ {};
        std::unique_ptr<askpass_service> server_ {};
        std::u8string helper_path_ {};
        std::optional<diagnostic> startup_diagnostic_ {};
        bool started_ { false };
    };
} // namespace cyella::win32
