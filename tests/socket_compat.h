#pragma once

// 시험용 가짜 서버(HTTP·GitHub API)가 Windows(Winsock)와 macOS(BSD 소켓)에서 같은 코드로 돌게 하는 얇은 다리다.
// Winsock의 이름(`SOCKET`·`INVALID_SOCKET`·`closesocket`·`SD_SEND`)을 POSIX에서도 쓰고, 시작·정리와 받기 제한 시간처럼
// 모양이 다른 곳만 함수로 감싼다.

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace cyella::test {
#if defined(_WIN32)
    using socket_length = int;
#else
    using SOCKET = int;
    inline constexpr SOCKET INVALID_SOCKET { -1 };
    inline constexpr int SD_SEND { SHUT_WR };
    using socket_length = socklen_t;

    inline int closesocket(const SOCKET socket) noexcept
    {
        return ::close(socket);
    }
#endif

    // Winsock을 쓸 준비를 한다. POSIX에는 할 일이 없다. 성공하면 true다.
    [[nodiscard]] inline bool start_sockets() noexcept
    {
#if defined(_WIN32)
        WSADATA data {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        return true;
#endif
    }

    inline void stop_sockets() noexcept
    {
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    // 받기 제한 시간을 건다. POSIX에서는 상대가 끊은 뒤 쓰기가 SIGPIPE로 시험 프로세스를 죽이지 않게도 한다.
    inline void set_receive_timeout(const SOCKET connection, const int milliseconds) noexcept
    {
#if defined(_WIN32)
        const DWORD timeout { static_cast<DWORD>(milliseconds) };
#else
        const timeval timeout { milliseconds / 1000, static_cast<int>((milliseconds % 1000) * 1000) };
        const int no_signal { 1 };
        setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &no_signal, sizeof(no_signal));
#endif
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }

    // `select`의 첫 인자다. Winsock은 보지 않고 POSIX는 가장 큰 기술자 + 1이다.
    [[nodiscard]] inline int select_width(const SOCKET socket) noexcept
    {
        return static_cast<int>(socket) + 1;
    }
} // namespace cyella::test
