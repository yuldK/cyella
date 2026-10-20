#pragma once

// 시험용 작은 HTTP 서버다. 127.0.0.1의 임시 포트에서 요청을 받아 핸들러가 정한 응답을 돌려준다. 실제 github.com·gitlab.com에는 절대 닿지
// 않게, 호스팅 연동 시험은 이 서버만 API 주소로 쓴다. 요청마다 연결을 닫는다(`Connection: close`).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cyella::test {
    struct fake_request
    {
        std::string method {};
        // 경로와 질의다 (`/repos/o/r/pulls?state=open`).
        std::string target {};
        // 이름은 소문자다.
        std::map<std::string, std::string> headers {};
        std::string body {};

        [[nodiscard]] std::string header(const std::string& name) const
        {
            const auto found { headers.find(name) };
            return found == headers.end() ? std::string {} : found->second;
        }

        [[nodiscard]] std::string path() const
        {
            return target.substr(0, target.find('?'));
        }

        // 질의 문자열의 값이다 (퍼센트 부호는 풀지 않는다).
        [[nodiscard]] std::string query(const std::string& key) const
        {
            const std::size_t mark { target.find('?') };
            if (mark == std::string::npos)
                return {};
            std::size_t start { mark + 1 };
            while (start < target.size())
            {
                std::size_t end { target.find('&', start) };
                if (end == std::string::npos)
                    end = target.size();
                const std::string pair { target.substr(start, end - start) };
                const std::size_t equals { pair.find('=') };
                if (pair.substr(0, equals) == key)
                    return equals == std::string::npos ? std::string {} : pair.substr(equals + 1);
                start = end + 1;
            }
            return {};
        }
    };

    struct fake_response
    {
        int status { 200 };
        std::vector<std::pair<std::string, std::string>> headers {};
        std::string body {};
        std::string content_type { "application/json" };
    };

    class fake_http_server
    {
    public:
        using handler = std::function<fake_response(const fake_request&)>;

        // `requested_port`가 0이면 임시 포트다.
        explicit fake_http_server(handler on_request, const std::uint16_t requested_port = 0)
            : handler_ { std::move(on_request) }
        {
            WSADATA data {};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error { "WSAStartup failed" };
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == INVALID_SOCKET)
                throw std::runtime_error { "socket failed" };
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_port = htons(requested_port);
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(listener_, 16) != 0)
                throw std::runtime_error { "bind/listen failed" };
            int length { sizeof(address) };
            getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
            port_ = ntohs(address.sin_port);
            thread_ = std::thread { [this] { serve(); } };
        }

        fake_http_server(const fake_http_server&) = delete;
        fake_http_server& operator=(const fake_http_server&) = delete;

        ~fake_http_server()
        {
            stopping_.store(true);
            if (thread_.joinable())
                thread_.join();
            while (active_.load() > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
            closesocket(listener_);
            WSACleanup();
        }

        [[nodiscard]] std::uint16_t port() const noexcept
        {
            return port_;
        }

        [[nodiscard]] std::string origin() const
        {
            return "http://127.0.0.1:" + std::to_string(port_);
        }

    private:
        void serve()
        {
            while (stopping_.load() == false)
            {
                fd_set readable {};
                FD_ZERO(&readable);
                FD_SET(listener_, &readable);
                timeval wait { 0, 50 * 1000 };
                if (select(0, &readable, nullptr, nullptr, &wait) <= 0)
                    continue;
                const SOCKET connection { accept(listener_, nullptr, nullptr) };
                if (connection == INVALID_SOCKET)
                    continue;
                // 연결마다 따로 처리해 한 연결이 막혀도 다른 요청이 선다 (luil client가 연결을 미리 열어 둘 수 있다).
                ++active_;
                auto work = [this, connection] { serve_connection(connection); };
                std::thread { work }.detach();
            }
        }

        void serve_connection(const SOCKET connection)
        {
            answer(connection);
            closesocket(connection);
            --active_;
        }

        static std::string lower(std::string text)
        {
            for (char& letter : text)
                letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
            return text;
        }

        void answer(const SOCKET connection)
        {
            const DWORD receive_timeout { 3000 };
            setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout));
            std::string received {};
            char buffer[4096] {};
            std::size_t head_end { std::string::npos };
            while ((head_end = received.find("\r\n\r\n")) == std::string::npos)
            {
                const int count { recv(connection, buffer, sizeof(buffer), 0) };
                if (count <= 0 || received.size() > 256 * 1024)
                    return;
                received.append(buffer, static_cast<std::size_t>(count));
            }
            fake_request request {};
            const std::string head { received.substr(0, head_end) };
            std::size_t line_end { head.find("\r\n") };
            const std::string first_line { head.substr(0, line_end) };
            const std::size_t space1 { first_line.find(' ') };
            const std::size_t space2 { first_line.find(' ', space1 + 1) };
            request.method = first_line.substr(0, space1);
            request.target = first_line.substr(space1 + 1, space2 - space1 - 1);
            while (line_end != std::string::npos)
            {
                const std::size_t start { line_end + 2 };
                line_end = head.find("\r\n", start);
                const std::string line { head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start) };
                const std::size_t colon { line.find(':') };
                if (colon == std::string::npos)
                    continue;
                std::string value { line.substr(colon + 1) };
                const std::size_t first { value.find_first_not_of(' ') };
                request.headers[lower(line.substr(0, colon))] = first == std::string::npos ? std::string {} : value.substr(first);
            }
            std::size_t expected { 0 };
            if (const std::string length { request.header("content-length") }; length.empty() == false)
                expected = static_cast<std::size_t>(std::stoul(length));
            request.body = received.substr(head_end + 4);
            while (request.body.size() < expected)
            {
                const int count { recv(connection, buffer, sizeof(buffer), 0) };
                if (count <= 0)
                    return;
                request.body.append(buffer, static_cast<std::size_t>(count));
            }

            const fake_response reply { handler_(request) };
            std::string text = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Type: " + reply.content_type;
            text += "\r\nContent-Length: " + std::to_string(reply.body.size()) + "\r\nConnection: close\r\n";
            for (const auto& [name, value] : reply.headers)
                text += name + ": " + value + "\r\n";
            text += "\r\n" + reply.body;
            std::size_t sent { 0 };
            while (sent < text.size())
            {
                const int count { send(connection, text.data() + sent, static_cast<int>(text.size() - sent), 0) };
                if (count <= 0)
                    break;
                sent += static_cast<std::size_t>(count);
            }
            shutdown(connection, SD_SEND);
        }

        handler handler_ {};
        SOCKET listener_ { INVALID_SOCKET };
        std::uint16_t port_ { 0 };
        std::atomic<bool> stopping_ { false };
        std::atomic<int> active_ { 0 };
        std::thread thread_ {};
    };
} // namespace cyella::test
