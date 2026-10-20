#pragma once

#include "application/process_cancellation.h"

#include <string>
#include <string_view>
#include <utility>

namespace cyella {
    // 메모리에서 내용을 지운다. 컴파일러가 "쓰이지 않는 쓰기"로 보고 없애지 못하게 volatile로 쓴다. 최선 노력이다 —
    // 복사본이 이미 다른 곳에 있었다면 거기까지는 닿지 않는다.
    void wipe_secret(std::string& text) noexcept;

    // 토큰·비밀번호를 쥐는 문자열이다. 복사할 수 없고 사라질 때 내용을 지운다. 로그·진단에 실을 일이 없으므로
    // 글로 바꾸는 길도 `value()` 하나뿐이다 (호출 지점이 곧 비밀이 나가는 지점이라 눈에 띈다).
    class secret_text
    {
    public:
        secret_text() noexcept = default;
        explicit secret_text(std::string value) noexcept
            : value_ { std::move(value) }
        {}
        secret_text(const secret_text&) = delete;
        secret_text& operator=(const secret_text&) = delete;
        secret_text(secret_text&& other) noexcept
            : value_ { std::move(other.value_) }
        {
            other.wipe();
        }
        secret_text& operator=(secret_text&& other) noexcept
        {
            if (this != &other)
            {
                wipe();
                value_ = std::move(other.value_);
                other.wipe();
            }
            return *this;
        }
        ~secret_text()
        {
            wipe();
        }

        [[nodiscard]] const std::string& value() const noexcept
        {
            return value_;
        }
        [[nodiscard]] bool empty() const noexcept
        {
            return value_.empty();
        }
        void wipe() noexcept
        {
            wipe_secret(value_);
        }

    private:
        std::string value_ {};
    };

    // `git credential`에 줄 질문이다. 프로토콜과 호스트(포트가 있으면 `host:port`)만 쓴다 — 경로는 주지 않는다
    // (`credential.useHttpPath`를 켜지 않은 흔한 설정에서 호스트 하나에 자격 증명이 하나다).
    struct credential_query
    {
        std::string protocol { "https" };
        std::string host {};
        // 저장소 폴더다. 비어 있지 않으면 그 저장소의 git 설정(`credential.helper`)이 적용된다.
        std::string working_directory {};
    };

    struct credential_fill_result
    {
        bool ok { false };
        std::string username {};
        secret_text token {};
        // 실패 이유다 (토큰을 담지 않는다).
        std::string error {};
        bool cancelled { false };
    };

    // 토큰을 구하는 길이다. 구현은 `git credential fill`로 사용자의 자격 증명 도우미(Git Credential Manager 등)에 묻는다.
    // 호출은 워커에서 하며 도우미의 창이 뜨는 동안 막힌다. cyella는 받은 토큰을 메모리에만 두고 파일에 쓰지 않는다 (ADR-007).
    class credential_source
    {
    public:
        credential_source() = default;
        credential_source(const credential_source&) = delete;
        credential_source& operator=(const credential_source&) = delete;
        virtual ~credential_source() = default;

        [[nodiscard]] virtual credential_fill_result fill(const credential_query& query, const process_cancellation_token& cancellation) = 0;
        // 거절된 토큰을 도우미가 지우게 한다 (`git credential reject`). 실패해도 알릴 것이 없다.
        virtual void reject(const credential_query& query, std::string_view username, const secret_text& token, const process_cancellation_token& cancellation) = 0;
    };
} // namespace cyella
