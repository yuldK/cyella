#include "platform/win32/askpass_protocol.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <string_view>

// git과 OpenSSH가 비밀이나 예/아니요 답이 필요할 때 실행하는 작은 도우미의 macOS 판이다 (Windows 판은
// platform/win32/askpass_helper_main.cpp). 프롬프트 문구를 `argv[1]`로 받아 앱의 Unix 소켓에 묻고, 답을 표준 출력에 한 줄로
// 쓴 뒤 0으로 끝난다. 취소, 오류, 환경 변수 누락, 앱이 없는 경우는 아무것도 쓰지 않고 1로 끝난다. luil과 Skia에는 링크하지 않는다.
namespace {
    constexpr int exit_answered { 0 };
    constexpr int exit_failed { 1 };

    std::optional<std::string> read_environment(const char* const name)
    {
        const char* const value { std::getenv(name) };
        if (value == nullptr)
            return std::nullopt;
        return std::string { value };
    }

    bool write_all(const int fd, const char* const data, const std::size_t size)
    {
        std::size_t done { 0 };
        while (done < size)
        {
            const ssize_t written { ::write(fd, data + done, size - done) };
            if (written < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            done += static_cast<std::size_t>(written);
        }
        return true;
    }

    bool read_all(const int fd, char* const data, const std::size_t size)
    {
        std::size_t done { 0 };
        while (done < size)
        {
            const ssize_t read { ::read(fd, data + done, size - done) };
            if (read < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (read == 0)
                return false;
            done += static_cast<std::size_t>(read);
        }
        return true;
    }

    void wipe(std::string& text) noexcept
    {
        volatile char* cursor { text.data() };
        for (std::size_t index { 0 }; index < text.size(); ++index)
            cursor[index] = '\0';
        text.clear();
    }

    int run(const int argc, const char* const* const argv)
    {
        const std::optional<std::string> socket_path { read_environment("CYELLA_ASKPASS_PIPE") };
        const std::optional<std::string> token { read_environment("CYELLA_ASKPASS_TOKEN") };
        const std::optional<std::string> operation { read_environment("CYELLA_ASKPASS_OPERATION") };
        if (socket_path.has_value() == false || token.has_value() == false || operation.has_value() == false)
            return exit_failed;
        // 앱이 만든 소켓은 절대 경로다. 상대 경로면 지금 폴더(저장소)의 엉뚱한 소켓에 토큰을 보낸다.
        if (socket_path->empty() || socket_path->front() != '/')
            return exit_failed;

        // 첫 인자만 필요하다. git과 ssh는 프롬프트 하나만 넘긴다.
        const std::string prompt { argc >= 2 ? std::string { argv[1] } : std::string {} };
        const std::optional<std::string> request { cyella::win32::encode_askpass_request({ *token, *operation, prompt }) };
        if (request.has_value() == false)
            return exit_failed;

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (socket_path->size() >= sizeof(address.sun_path))
            return exit_failed;
        std::memcpy(address.sun_path, socket_path->c_str(), socket_path->size() + 1);
        const int connection { ::socket(AF_UNIX, SOCK_STREAM, 0) };
        if (connection < 0)
            return exit_failed;
        const int on { 1 };
        static_cast<void>(::setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)));
        if (::connect(connection, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            static_cast<void>(::close(connection));
            return exit_failed;
        }

        int exit_code { exit_failed };
        std::string header(cyella::win32::askpass_reply_header_bytes, '\0');
        if (write_all(connection, request->data(), request->size()) && read_all(connection, header.data(), header.size()))
        {
            const std::optional<cyella::win32::askpass_reply_header> reply { cyella::win32::decode_askpass_reply_header(header) };
            if (reply.has_value() && reply->answered)
            {
                // 답과 줄 바꿈을 한 번에 써서 중간에 끊긴 답이 나가지 않게 한다. 줄 바꿈 자리까지 처음부터 잡아 둔다 (Windows 판과 같다).
                std::string answer(reply->answer_bytes + 1u, '\0');
                if (read_all(connection, answer.data(), reply->answer_bytes))
                {
                    answer.back() = '\n';
                    if (write_all(STDOUT_FILENO, answer.data(), answer.size()))
                        exit_code = exit_answered;
                }
                wipe(answer);
            }
        }
        static_cast<void>(::close(connection));
        return exit_code;
    }
} // namespace

int main(const int argc, const char* const* const argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (...)
    {
        return exit_failed;
    }
}
