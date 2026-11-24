#include "platform/win32/win32_askpass_server.h"

#include "domain/credential_prompt.h"
#include "platform/macos/posix_file.h"
#include "platform/win32/askpass_protocol.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// `platform/win32/win32_askpass_server.h`의 macOS 구현이다. Win32 판의 named pipe 자리에 Unix 소켓을 쓴다.
//
//  - 소켓은 사용자 임시 폴더(`$TMPDIR`, 사용자마다 따로다) 아래에 `mkdtemp`로 만든 0700 폴더 안에 있다. 그 폴더에 들어갈 수
//    있는 것은 이 사용자뿐이고, 받은 연결은 `getpeereid`로 같은 사용자인지 한 번 더 본다 (Win32 판의 DACL 자리).
//  - 토큰은 `arc4random_buf`로 만든 128비트 난수다. 토큰이 틀린 요청은 핸들러를 부르지 않고 바로 취소로 답한다.
//  - 질문마다 연결 thread가 하나다. 답을 기다리는 동안 도우미가 사라졌는지도 본다 (도우미는 요청 뒤에 아무것도 보내지 않으므로
//    읽을 것이 생기거나 끊기면 사라진 것이다).
//  - 소켓 경로가 `CYELLA_ASKPASS_PIPE`로 도우미에게 간다 (askpass_environment). 이름은 Windows에서 온 것이고 뜻은 "접속 주소"다.
namespace cyella::win32 {
    namespace {
        constexpr int request_timeout_milliseconds { 10000 };
        constexpr int reply_timeout_milliseconds { 5000 };
        constexpr std::size_t maximum_active_connections { 64u };
        constexpr std::size_t remembered_cancelled_operations { 256u };
        constexpr std::chrono::milliseconds connection_drain_limit { 2000 };
        constexpr int listener_retry_milliseconds { 100 };

        // best-effort 지우기다. 컴파일러가 마지막 쓰기라며 없애지 못하도록 volatile로 쓴다.
        void wipe(std::string& text) noexcept
        {
            volatile char* cursor { text.data() };
            for (std::size_t index { 0 }; index < text.size(); ++index)
                cursor[index] = '\0';
            text.clear();
        }

        void wipe(std::u8string& text) noexcept
        {
            volatile char8_t* cursor { text.data() };
            for (std::size_t index { 0 }; index < text.size(); ++index)
                cursor[index] = u8'\0';
            text.clear();
        }

        // 길이가 같을 때 내용이 어디서 달라지는지 시간으로 새지 않게 비교한다.
        bool equal_constant_time(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
                return false;
            unsigned char difference { 0 };
            for (std::size_t index { 0 }; index < left.size(); ++index)
                difference = static_cast<unsigned char>(difference | (static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index])));
            return difference == 0;
        }

        std::string random_hex(const std::size_t byte_count)
        {
            std::vector<unsigned char> bytes(byte_count);
            ::arc4random_buf(bytes.data(), bytes.size());
            constexpr std::string_view digits { "0123456789abcdef" };
            std::string text {};
            text.reserve(byte_count * 2);
            for (const unsigned char value : bytes)
            {
                text.push_back(digits[value >> 4]);
                text.push_back(digits[value & 0x0F]);
            }
            return text;
        }

        // 한 번 울리면 계속 울려 있는 신호다. 읽기 끝을 poll에 넣는다.
        class latch
        {
        public:
            [[nodiscard]] bool open() noexcept
            {
                int ends[2] { -1, -1 };
                if (::pipe(ends) != 0)
                    return false;
                read_.reset(ends[0]);
                write_.reset(ends[1]);
                return ::fcntl(ends[0], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(ends[1], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(ends[1], F_SETFL, O_NONBLOCK) == 0;
            }

            void raise() noexcept
            {
                if (raised_.exchange(true))
                    return;
                const char byte { 1 };
                static_cast<void>(::write(write_.get(), &byte, 1));
            }

            [[nodiscard]] int fd() const noexcept
            {
                return read_.get();
            }

        private:
            macos::unique_fd read_ {};
            macos::unique_fd write_ {};
            std::atomic<bool> raised_ { false };
        };

        // 질문 하나의 대기 상태다. 연결 thread가 소유하고, 핸들러가 받은 답 callback은 약한 참조만 쥔다 (Win32 판과 같다).
        class pending_prompt
        {
        public:
            pending_prompt(const std::uint64_t id, std::u8string operation_id)
                : id_ { id }
                , operation_id_ { std::move(operation_id) }
            {
                opened_ = ready_.open();
            }

            pending_prompt(const pending_prompt&) = delete;
            pending_prompt& operator=(const pending_prompt&) = delete;

            ~pending_prompt()
            {
                wipe(reply_.answer);
            }

            [[nodiscard]] bool usable() const noexcept
            {
                return opened_;
            }

            [[nodiscard]] std::uint64_t id() const noexcept
            {
                return id_;
            }

            [[nodiscard]] const std::u8string& operation_id() const noexcept
            {
                return operation_id_;
            }

            [[nodiscard]] int ready_fd() const noexcept
            {
                return ready_.fd();
            }

            void fulfill(askpass_reply reply)
            {
                const std::lock_guard<std::mutex> lock { mutex_ };
                if (fulfilled_)
                {
                    wipe(reply.answer);
                    return;
                }
                fulfilled_ = true;
                reply_ = std::move(reply);
                ready_.raise();
            }

            [[nodiscard]] askpass_reply take_reply()
            {
                const std::lock_guard<std::mutex> lock { mutex_ };
                askpass_reply reply { std::move(reply_) };
                reply_ = askpass_reply {};
                return reply;
            }

        private:
            std::uint64_t id_ {};
            std::u8string operation_id_ {};
            latch ready_ {};
            bool opened_ { false };
            std::mutex mutex_ {};
            bool fulfilled_ { false };
            askpass_reply reply_ {};
        };

        struct server_state
        {
            askpass_prompt_handler handler {};
            std::string token {};
            std::string directory {};
            std::string socket_path {};
            macos::unique_fd listener {};
            latch stop {};
            std::atomic<std::uint64_t> next_prompt_id { 1 };

            std::mutex mutex {};
            std::condition_variable idle {};
            bool stopping { false };
            std::size_t active_connections { 0 };
            std::vector<std::shared_ptr<pending_prompt>> pending {};
            std::deque<std::u8string> cancelled_operations {};

            [[nodiscard]] bool operation_cancelled_locked(const std::u8string& operation_id) const
            {
                return std::ranges::find(cancelled_operations, operation_id) != cancelled_operations.end();
            }

            ~server_state()
            {
                // 소켓 파일과 폴더를 지운다. 비정상 종료로 남은 것은 OS가 임시 폴더를 비울 때 사라진다.
                listener.reset();
                if (socket_path.empty() == false)
                    static_cast<void>(::unlink(socket_path.c_str()));
                if (directory.empty() == false)
                    static_cast<void>(::rmdir(directory.c_str()));
            }
        };

        enum class transfer_outcome
        {
            completed,
            failed,
            stopped,
            timed_out,
        };

        // 정확히 `size` 바이트를 읽거나 쓴다. 중지 신호나 시간 초과에서 멈춘다.
        transfer_outcome transfer_all(const int socket, const bool write, char* const buffer, const std::size_t size, const int stop_fd, const int timeout_milliseconds)
        {
            const auto deadline { std::chrono::steady_clock::now() + std::chrono::milliseconds { timeout_milliseconds } };
            std::size_t done { 0 };
            while (done < size)
            {
                const auto left { std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count() };
                if (left <= 0)
                    return transfer_outcome::timed_out;
                pollfd watched[2] { { socket, static_cast<short>(write ? POLLOUT : POLLIN), 0 }, { stop_fd, POLLIN, 0 } };
                const int ready { ::poll(watched, 2, static_cast<int>(left)) };
                if (ready < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return transfer_outcome::failed;
                }
                if (ready == 0)
                    return transfer_outcome::timed_out;
                if ((watched[1].revents & POLLIN) != 0)
                    return transfer_outcome::stopped;
                const ssize_t moved { write ? ::send(socket, buffer + done, size - done, 0) : ::recv(socket, buffer + done, size - done, 0) };
                if (moved < 0)
                {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    return transfer_outcome::failed;
                }
                if (moved == 0)
                    return transfer_outcome::failed;
                done += static_cast<std::size_t>(moved);
            }
            return transfer_outcome::completed;
        }

        std::optional<askpass_request_message> read_request(const int socket, const int stop_fd)
        {
            std::string header(askpass_request_header_bytes, '\0');
            if (transfer_all(socket, false, header.data(), header.size(), stop_fd, request_timeout_milliseconds) != transfer_outcome::completed)
                return std::nullopt;
            const std::optional<askpass_request_lengths> lengths { decode_askpass_request_header(header) };
            if (lengths.has_value() == false)
                return std::nullopt;
            std::string body(lengths->body_bytes(), '\0');
            if (body.empty() == false && transfer_all(socket, false, body.data(), body.size(), stop_fd, request_timeout_milliseconds) != transfer_outcome::completed)
            {
                wipe(body);
                return std::nullopt;
            }
            std::optional<askpass_request_message> message { decode_askpass_request_body(*lengths, body) };
            wipe(body);
            return message;
        }

        void write_reply(const int socket, const int stop_fd, const bool answered, std::string& answer)
        {
            std::string frame { encode_askpass_reply(answered, answer) };
            static_cast<void>(transfer_all(socket, true, frame.data(), frame.size(), stop_fd, reply_timeout_milliseconds));
            wipe(frame);
            wipe(answer);
        }

        enum class reply_wait_outcome
        {
            replied,
            stopped,
            client_gone,
        };

        reply_wait_outcome await_reply(const int socket, const int reply_fd, const int stop_fd)
        {
            while (true)
            {
                pollfd watched[3] { { reply_fd, POLLIN, 0 }, { stop_fd, POLLIN, 0 }, { socket, POLLIN, 0 } };
                const int ready { ::poll(watched, 3, -1) };
                if (ready < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return reply_wait_outcome::stopped;
                }
                if ((watched[0].revents & POLLIN) != 0)
                    return reply_wait_outcome::replied;
                if ((watched[1].revents & POLLIN) != 0)
                    return reply_wait_outcome::stopped;
                if ((watched[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                    return reply_wait_outcome::client_gone;
            }
        }

        class connection_guard
        {
        public:
            explicit connection_guard(std::shared_ptr<server_state> state) noexcept
                : state_ { std::move(state) }
            {}

            connection_guard(const connection_guard&) = delete;
            connection_guard& operator=(const connection_guard&) = delete;

            ~connection_guard()
            {
                {
                    const std::lock_guard<std::mutex> lock { state_->mutex };
                    --state_->active_connections;
                }
                state_->idle.notify_all();
            }

        private:
            std::shared_ptr<server_state> state_ {};
        };

        std::u8string to_u8(const std::string_view text)
        {
            return macos::to_u8(text);
        }

        askpass_reply obtain_reply(server_state& state, const int socket, const askpass_request_message& request)
        {
            if (equal_constant_time(request.token, state.token) == false)
                return askpass_reply::cancelled();

            std::u8string operation_id { to_u8(request.operation_id) };
            const auto prompt { std::make_shared<pending_prompt>(state.next_prompt_id.fetch_add(1), operation_id) };
            if (prompt->usable() == false)
                return askpass_reply::cancelled();
            {
                const std::lock_guard<std::mutex> lock { state.mutex };
                if (state.stopping || state.operation_cancelled_locked(operation_id))
                    return askpass_reply::cancelled();
                state.pending.push_back(prompt);
            }

            askpass_prompt_request prompt_request {};
            prompt_request.prompt_id = prompt->id();
            prompt_request.operation_id = std::move(operation_id);
            prompt_request.prompt = classify_credential_prompt(to_u8(request.prompt));

            const std::weak_ptr<pending_prompt> weak { prompt };
            auto deliver = [weak](askpass_reply reply) {
                if (const std::shared_ptr<pending_prompt> alive { weak.lock() })
                    alive->fulfill(std::move(reply));
                else
                    wipe(reply.answer);
            };
            try
            {
                state.handler(std::move(prompt_request), askpass_reply_callback { std::move(deliver) });
            }
            catch (...)
            {
                prompt->fulfill(askpass_reply::cancelled());
            }

            const reply_wait_outcome outcome { await_reply(socket, prompt->ready_fd(), state.stop.fd()) };
            {
                const std::lock_guard<std::mutex> lock { state.mutex };
                std::erase(state.pending, prompt);
            }
            if (outcome != reply_wait_outcome::replied)
                return askpass_reply::cancelled();
            return prompt->take_reply();
        }

        void serve_connection(const std::shared_ptr<server_state> state, macos::unique_fd socket)
        {
            const connection_guard guard { state };
            // 같은 사용자의 프로세스만 받는다. 폴더 권한이 이미 막지만 두 번째 울타리다.
            uid_t user { 0 };
            gid_t group { 0 };
            if (::getpeereid(socket.get(), &user, &group) != 0 || user != ::geteuid())
                return;
            const std::optional<askpass_request_message> request { read_request(socket.get(), state->stop.fd()) };
            if (request.has_value() == false)
                return;
            askpass_reply reply { obtain_reply(*state, socket.get(), *request) };
            std::string answer { macos::to_narrow(reply.answer) };
            wipe(reply.answer);
            write_reply(socket.get(), state->stop.fd(), reply.answered, answer);
        }

        void listen_loop(const std::shared_ptr<server_state> state)
        {
            while (true)
            {
                pollfd watched[2] { { state->listener.get(), POLLIN, 0 }, { state->stop.fd(), POLLIN, 0 } };
                const int ready { ::poll(watched, 2, -1) };
                if (ready < 0)
                {
                    if (errno == EINTR)
                        continue;
                    std::this_thread::sleep_for(std::chrono::milliseconds { listener_retry_milliseconds });
                    continue;
                }
                if ((watched[1].revents & POLLIN) != 0)
                    return;
                if ((watched[0].revents & POLLIN) == 0)
                    continue;
                macos::unique_fd connection { ::accept(state->listener.get(), nullptr, nullptr) };
                if (connection.valid() == false)
                {
                    if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED)
                        std::this_thread::sleep_for(std::chrono::milliseconds { listener_retry_milliseconds });
                    continue;
                }
                static_cast<void>(::fcntl(connection.get(), F_SETFD, FD_CLOEXEC));
                // 답을 쓰는 사이 도우미가 끊으면 SIGPIPE 대신 EPIPE가 되게 한다.
                const int on { 1 };
                static_cast<void>(::setsockopt(connection.get(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)));
                {
                    const std::lock_guard<std::mutex> lock { state->mutex };
                    if (state->stopping)
                        return;
                    if (state->active_connections >= maximum_active_connections)
                        continue;
                    ++state->active_connections;
                }
                try
                {
                    std::thread { serve_connection, state, std::move(connection) }.detach();
                }
                catch (...)
                {
                    // thread를 못 만들면 연결을 닫고(connection이 소멸한다) 계속 받는다.
                    {
                        const std::lock_guard<std::mutex> lock { state->mutex };
                        --state->active_connections;
                    }
                    state->idle.notify_all();
                }
            }
        }

        class askpass_server final : public askpass_service
        {
        public:
            explicit askpass_server(std::shared_ptr<server_state> state)
                : state_ { std::move(state) }
            {}

            askpass_server(const askpass_server&) = delete;
            askpass_server& operator=(const askpass_server&) = delete;

            ~askpass_server() override
            {
                shutdown();
            }

            void start_listening()
            {
                listener_ = std::thread { listen_loop, state_ };
            }

            [[nodiscard]] askpass_endpoint endpoint() const override
            {
                return askpass_endpoint { macos::to_u8(state_->socket_path), macos::to_u8(state_->token) };
            }

            void cancel_operation(const std::u8string_view operation_id) override
            {
                const std::lock_guard<std::mutex> lock { state_->mutex };
                if (state_->operation_cancelled_locked(std::u8string { operation_id }) == false)
                {
                    state_->cancelled_operations.emplace_back(operation_id);
                    if (state_->cancelled_operations.size() > remembered_cancelled_operations)
                        state_->cancelled_operations.pop_front();
                }
                for (const std::shared_ptr<pending_prompt>& prompt : state_->pending)
                    if (prompt->operation_id() == operation_id)
                        prompt->fulfill(askpass_reply::cancelled());
            }

            void shutdown() override
            {
                const std::lock_guard<std::mutex> shutdown_lock { shutdown_mutex_ };
                {
                    const std::lock_guard<std::mutex> lock { state_->mutex };
                    state_->stopping = true;
                    for (const std::shared_ptr<pending_prompt>& prompt : state_->pending)
                        prompt->fulfill(askpass_reply::cancelled());
                }
                state_->stop.raise();
                if (listener_.joinable())
                    listener_.join();
                std::unique_lock<std::mutex> lock { state_->mutex };
                state_->idle.wait_for(lock, connection_drain_limit, [this]() { return state_->active_connections == 0; });
            }

        private:
            std::shared_ptr<server_state> state_ {};
            std::thread listener_ {};
            std::mutex shutdown_mutex_ {};
        };

        // 사용자 임시 폴더다. `$TMPDIR`(사용자마다 따로 있는 `/var/folders/…/T/`)가 없으면 `/tmp`다.
        std::string temporary_root()
        {
            std::string root { macos::to_narrow(macos::environment_value(u8"TMPDIR").value_or(u8"/tmp")) };
            while (root.size() > 1 && root.back() == '/')
                root.pop_back();
            return root.empty() ? std::string { "/tmp" } : root;
        }
    } // namespace

    askpass_server_start_result start_askpass_server(askpass_prompt_handler handler)
    {
        askpass_server_start_result result {};
        try
        {
            const auto state { std::make_shared<server_state>() };
            state->handler = std::move(handler);
            if (state->stop.open() == false)
            {
                result.native_error = static_cast<std::uint32_t>(errno);
                return result;
            }
            state->token = random_hex(16);

            std::string pattern { temporary_root() + "/cyella-askpass-XXXXXX" };
            if (::mkdtemp(pattern.data()) == nullptr)
            {
                result.native_error = static_cast<std::uint32_t>(errno);
                return result;
            }
            state->directory = pattern;
            static_cast<void>(::chmod(state->directory.c_str(), 0700));
            state->socket_path = state->directory + "/s";

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            if (state->socket_path.size() >= sizeof(address.sun_path))
            {
                result.native_error = static_cast<std::uint32_t>(ENAMETOOLONG);
                return result;
            }
            std::memcpy(address.sun_path, state->socket_path.c_str(), state->socket_path.size() + 1);

            state->listener.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
            if (state->listener.valid() == false)
            {
                result.native_error = static_cast<std::uint32_t>(errno);
                return result;
            }
            static_cast<void>(::fcntl(state->listener.get(), F_SETFD, FD_CLOEXEC));
            if (::bind(state->listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || ::listen(state->listener.get(), 16) != 0)
            {
                result.native_error = static_cast<std::uint32_t>(errno);
                return result;
            }

            auto server { std::make_unique<askpass_server>(state) };
            server->start_listening();
            result.server = std::move(server);
            return result;
        }
        catch (...)
        {
            result.server.reset();
            result.native_error = static_cast<std::uint32_t>(ENOMEM);
            return result;
        }
    }
} // namespace cyella::win32
