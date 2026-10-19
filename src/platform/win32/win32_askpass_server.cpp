#include "platform/win32/win32_askpass_server.h"

#include "domain/credential_prompt.h"
#include "platform/win32/askpass_protocol.h"

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cyella::win32 {
    namespace {
        constexpr DWORD pipe_buffer_bytes { 64u * 1024u };
        // 요청은 연결 직후 한꺼번에 오므로 길게 기다릴 이유가 없다. 말없이 붙어 있는 연결이 스레드를
        // 붙잡지 못하게 한다.
        constexpr DWORD request_timeout_milliseconds { 10000u };
        constexpr DWORD reply_timeout_milliseconds { 5000u };
        constexpr std::size_t maximum_active_connections { 64u };
        constexpr std::size_t remembered_cancelled_operations { 256u };
        // scheduler의 종료 예산(약 3초) 안에 끝내기 위한 연결 스레드 대기 상한이다.
        constexpr std::chrono::milliseconds connection_drain_limit { 2000 };

        class scoped_handle
        {
        public:
            scoped_handle() noexcept = default;

            explicit scoped_handle(const HANDLE handle) noexcept
                : handle_ { handle }
            {}

            scoped_handle(const scoped_handle&) = delete;
            scoped_handle& operator=(const scoped_handle&) = delete;

            scoped_handle(scoped_handle&& other) noexcept
                : handle_ { std::exchange(other.handle_, INVALID_HANDLE_VALUE) }
            {}

            scoped_handle& operator=(scoped_handle&& other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
                }
                return *this;
            }

            ~scoped_handle()
            {
                reset();
            }

            [[nodiscard]] HANDLE get() const noexcept
            {
                return handle_;
            }

            // CreateEvent는 실패하면 null을, CreateNamedPipe와 CreateFile은 INVALID_HANDLE_VALUE를 돌려준다.
            [[nodiscard]] bool valid() const noexcept
            {
                return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
            }

            void reset() noexcept
            {
                if (valid())
                    CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }

        private:
            HANDLE handle_ { INVALID_HANDLE_VALUE };
        };

        scoped_handle make_manual_reset_event()
        {
            return scoped_handle { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        }

        // best-effort 지우기다. 컴파일러가 마지막 쓰기라며 없애지 못하는 SecureZeroMemory를 쓴다.
        void wipe(std::string& text) noexcept
        {
            if (text.empty() == false)
                SecureZeroMemory(text.data(), text.size());
            text.clear();
        }

        void wipe(std::u8string& text) noexcept
        {
            if (text.empty() == false)
                SecureZeroMemory(text.data(), text.size());
            text.clear();
        }

        std::string to_narrow(const std::u8string_view text)
        {
            return std::string { reinterpret_cast<const char*>(text.data()), text.size() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { reinterpret_cast<const char8_t*>(text.data()), text.size() };
        }

        // 길이가 같을 때 내용이 어디서 달라지는지 시간으로 새지 않게 비교한다.
        bool equal_constant_time(const std::string_view left, const std::string_view right) noexcept
        {
            unsigned char difference { static_cast<unsigned char>(left.size() != right.size() ? 1u : 0u) };
            const std::size_t length { std::max(left.size(), right.size()) };
            for (std::size_t index { 0 }; index < length; ++index)
            {
                const unsigned char a { index < left.size() ? static_cast<unsigned char>(left[index]) : static_cast<unsigned char>(0) };
                const unsigned char b { index < right.size() ? static_cast<unsigned char>(right[index]) : static_cast<unsigned char>(0) };
                difference = static_cast<unsigned char>(difference | (a ^ b));
            }
            return difference == 0;
        }

        bool random_hex(const std::size_t byte_count, std::string& output)
        {
            std::vector<unsigned char> bytes(byte_count);
            if (BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) == false)
                return false;

            constexpr std::string_view digits { "0123456789abcdef" };
            output.clear();
            output.reserve(byte_count * 2u);
            for (const unsigned char value : bytes)
            {
                output.push_back(digits[value >> 4u]);
                output.push_back(digits[value & 0x0fu]);
            }
            SecureZeroMemory(bytes.data(), bytes.size());
            return true;
        }

        // 현재 사용자 SID 하나에만 접근을 주는 DACL이다. `P`는 상속을 막는다.
        class pipe_security
        {
        public:
            pipe_security() = default;
            pipe_security(const pipe_security&) = delete;
            pipe_security& operator=(const pipe_security&) = delete;

            ~pipe_security()
            {
                if (descriptor_ != nullptr)
                    LocalFree(descriptor_);
            }

            [[nodiscard]] bool build(DWORD& error)
            {
                scoped_handle token {};
                HANDLE raw_token { nullptr };
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == FALSE)
                {
                    error = GetLastError();
                    return false;
                }
                token = scoped_handle { raw_token };

                DWORD needed { 0 };
                GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed);
                std::vector<unsigned char> buffer(needed);
                if (needed == 0 || GetTokenInformation(token.get(), TokenUser, buffer.data(), needed, &needed) == FALSE)
                {
                    error = GetLastError();
                    return false;
                }

                LPWSTR sid_text { nullptr };
                if (ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, &sid_text) == FALSE)
                {
                    error = GetLastError();
                    return false;
                }
                const std::wstring sddl { std::wstring { L"D:P(A;;GA;;;" } + sid_text + L")" };
                LocalFree(sid_text);

                if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr) == FALSE)
                {
                    error = GetLastError();
                    return false;
                }
                attributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
                attributes_.lpSecurityDescriptor = descriptor_;
                attributes_.bInheritHandle = FALSE;
                return true;
            }

            [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept
            {
                return &attributes_;
            }

        private:
            PSECURITY_DESCRIPTOR descriptor_ { nullptr };
            SECURITY_ATTRIBUTES attributes_ {};
        };

        // 질문 하나의 대기 상태다. 연결 스레드가 소유하고, 핸들러가 받은 답 callback은 약한 참조만
        // 쥔다. 연결이 끝난 뒤의 늦은 답은 참조가 만료되어 조용히 사라진다.
        class pending_prompt
        {
        public:
            pending_prompt(const std::uint64_t id, std::u8string operation_id)
                : id_ { id }
                , operation_id_ { std::move(operation_id) }
                , ready_ { make_manual_reset_event() }
            {}

            pending_prompt(const pending_prompt&) = delete;
            pending_prompt& operator=(const pending_prompt&) = delete;

            ~pending_prompt()
            {
                wipe(reply_.answer);
            }

            [[nodiscard]] std::uint64_t id() const noexcept
            {
                return id_;
            }

            [[nodiscard]] const std::u8string& operation_id() const noexcept
            {
                return operation_id_;
            }

            [[nodiscard]] HANDLE ready_event() const noexcept
            {
                return ready_.get();
            }

            // 처음 한 번만 받아들인다. 이미 정해졌으면 들어온 답을 지운다.
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
                SetEvent(ready_.get());
            }

            // 답을 꺼내 간다. 호출한 쪽이 쓴 뒤 지울 책임을 진다.
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
            scoped_handle ready_ {};
            std::mutex mutex_ {};
            bool fulfilled_ { false };
            askpass_reply reply_ {};
        };

        // listener와 연결 스레드, 서버 객체가 공유하는 상태다. 연결 스레드는 분리되어 있으므로
        // 서버 객체보다 오래 살 수 있고, 그래서 shared_ptr로 수명을 묶는다.
        struct server_state
        {
            askpass_prompt_handler handler {};
            std::string token {};
            std::wstring pipe_name {};
            pipe_security security {};
            scoped_handle stop_event {};
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
        };

        scoped_handle create_pipe_instance(server_state& state, const bool first, DWORD& error)
        {
            DWORD open_mode { PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED };
            // 첫 인스턴스에만 건다. 같은 이름을 이미 누가 차지했으면 여기서 실패해 알아챈다.
            // 이어서 만드는 인스턴스에 걸면 서버 자신이 ERROR_ACCESS_DENIED를 받는다.
            if (first)
                open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

            // 바이트 스트림이고, 원격 클라이언트는 받지 않는다.
            constexpr DWORD pipe_mode { PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS };
            const HANDLE created { CreateNamedPipeW(state.pipe_name.c_str(), open_mode, pipe_mode, PIPE_UNLIMITED_INSTANCES, pipe_buffer_bytes, pipe_buffer_bytes, 0, state.security.attributes()) };
            scoped_handle pipe { created };
            if (pipe.valid() == false)
                error = GetLastError();
            return pipe;
        }

        enum class transfer_outcome
        {
            completed,
            failed,
            stopped,
            timed_out,
        };

        // overlapped 입출력 하나를 끝날 때까지, 또는 중지 신호나 시간 초과까지 기다린다. 끝나지 않은
        // 채 반환하면 스택의 OVERLAPPED가 사라진 뒤 커널이 쓰므로 반드시 취소가 완료된 뒤 반환한다.
        transfer_outcome transfer(const HANDLE pipe, const bool write, void* const buffer, const DWORD size, DWORD& transferred, const HANDLE event, const HANDLE stop_event, const DWORD timeout)
        {
            transferred = 0;
            ResetEvent(event);
            OVERLAPPED overlapped {};
            overlapped.hEvent = event;

            const BOOL started { write ? WriteFile(pipe, buffer, size, nullptr, &overlapped) : ReadFile(pipe, buffer, size, nullptr, &overlapped) };
            if (started == FALSE && GetLastError() != ERROR_IO_PENDING)
                return transfer_outcome::failed;

            const HANDLE handles[] { event, stop_event };
            const DWORD waited { WaitForMultipleObjects(2, handles, FALSE, timeout) };
            if (waited == WAIT_OBJECT_0)
                return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE ? transfer_outcome::completed : transfer_outcome::failed;

            CancelIoEx(pipe, &overlapped);
            DWORD ignored { 0 };
            GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            if (waited == WAIT_OBJECT_0 + 1)
                return transfer_outcome::stopped;
            return waited == WAIT_TIMEOUT ? transfer_outcome::timed_out : transfer_outcome::failed;
        }

        // 정확히 `size` byte를 채운다. 파이프는 byte 모드라 한 번에 덜 올 수 있다.
        transfer_outcome transfer_all(const HANDLE pipe, const bool write, char* const buffer, const std::size_t size, const HANDLE event, const HANDLE stop_event, const DWORD timeout)
        {
            std::size_t done { 0 };
            while (done < size)
            {
                DWORD transferred { 0 };
                const transfer_outcome outcome { transfer(pipe, write, buffer + done, static_cast<DWORD>(size - done), transferred, event, stop_event, timeout) };
                if (outcome != transfer_outcome::completed)
                    return outcome;
                if (transferred == 0)
                    return transfer_outcome::failed;
                done += transferred;
            }
            return transfer_outcome::completed;
        }

        std::optional<askpass_request_message> read_request(const HANDLE pipe, const HANDLE event, const HANDLE stop_event)
        {
            std::string header(askpass_request_header_bytes, '\0');
            if (transfer_all(pipe, false, header.data(), header.size(), event, stop_event, request_timeout_milliseconds) != transfer_outcome::completed)
                return std::nullopt;
            const std::optional<askpass_request_lengths> lengths { decode_askpass_request_header(header) };
            if (lengths.has_value() == false)
                return std::nullopt;

            std::string body(lengths->body_bytes(), '\0');
            if (body.empty() == false && transfer_all(pipe, false, body.data(), body.size(), event, stop_event, request_timeout_milliseconds) != transfer_outcome::completed)
            {
                wipe(body);
                return std::nullopt;
            }
            std::optional<askpass_request_message> message { decode_askpass_request_body(*lengths, body) };
            wipe(body);
            return message;
        }

        void write_reply(const HANDLE pipe, const HANDLE event, const HANDLE stop_event, const bool answered, std::string& answer)
        {
            std::string frame { encode_askpass_reply(answered, answer) };
            transfer_all(pipe, true, frame.data(), frame.size(), event, stop_event, reply_timeout_milliseconds);
            // 답은 한 번 쓰이고 나면 어디에도 남기지 않는다.
            wipe(frame);
            wipe(answer);
        }

        enum class reply_wait_outcome
        {
            replied,
            stopped,
            client_gone,
        };

        // 답을 기다리는 동안 helper가 사라졌는지도 본다. helper는 요청 뒤에 아무것도 보내지 않으므로
        // 읽기 하나가 끝나는 것은 연결이 끊겼다는 뜻이다.
        reply_wait_outcome await_reply(const HANDLE pipe, const HANDLE reply_event, const HANDLE stop_event)
        {
            scoped_handle gone_event { make_manual_reset_event() };
            if (gone_event.valid() == false)
                return reply_wait_outcome::stopped;

            OVERLAPPED overlapped {};
            overlapped.hEvent = gone_event.get();
            char byte { 0 };
            if (ReadFile(pipe, &byte, 1, nullptr, &overlapped) == FALSE && GetLastError() != ERROR_IO_PENDING)
                return reply_wait_outcome::client_gone;

            const HANDLE handles[] { reply_event, stop_event, gone_event.get() };
            const DWORD waited { WaitForMultipleObjects(3, handles, FALSE, INFINITE) };
            if (waited != WAIT_OBJECT_0 + 2)
            {
                CancelIoEx(pipe, &overlapped);
                DWORD ignored { 0 };
                GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            }
            if (waited == WAIT_OBJECT_0)
                return reply_wait_outcome::replied;
            return waited == WAIT_OBJECT_0 + 2 ? reply_wait_outcome::client_gone : reply_wait_outcome::stopped;
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

        // 핸들러를 부르고 답을 기다린다. 취소로 답해야 하는 모든 경우를 `answered == false`로 돌려준다.
        askpass_reply obtain_reply(server_state& state, const std::shared_ptr<server_state>& shared, const HANDLE pipe, const askpass_request_message& request)
        {
            if (equal_constant_time(request.token, state.token) == false)
                return askpass_reply::cancelled();

            std::u8string operation_id { to_u8(request.operation_id) };
            const auto prompt { std::make_shared<pending_prompt>(state.next_prompt_id.fetch_add(1), operation_id) };
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
            askpass_reply_callback callback { std::move(deliver) };

            try
            {
                state.handler(std::move(prompt_request), std::move(callback));
            }
            catch (...)
            {
                prompt->fulfill(askpass_reply::cancelled());
            }

            const reply_wait_outcome outcome { await_reply(pipe, prompt->ready_event(), shared->stop_event.get()) };
            {
                const std::lock_guard<std::mutex> lock { state.mutex };
                std::erase(state.pending, prompt);
            }
            if (outcome != reply_wait_outcome::replied)
                return askpass_reply::cancelled();
            return prompt->take_reply();
        }

        void serve_connection(const std::shared_ptr<server_state> state, scoped_handle pipe)
        {
            const connection_guard guard { state };
            scoped_handle io_event { make_manual_reset_event() };
            if (io_event.valid() == false)
                return;

            const std::optional<askpass_request_message> request { read_request(pipe.get(), io_event.get(), state->stop_event.get()) };
            if (request.has_value() == false)
                return;

            askpass_reply reply { obtain_reply(*state, state, pipe.get(), *request) };
            std::string answer { to_narrow(reply.answer) };
            wipe(reply.answer);
            write_reply(pipe.get(), io_event.get(), state->stop_event.get(), reply.answered, answer);
            // 닫기 전에 FlushFileBuffers를 부르지 않는다. 읽지 않는 클라이언트가 스레드를 붙잡는다.
            // DisconnectNamedPipe도 쓰지 않는다. 클라이언트가 읽기 전의 데이터를 버리게 된다.
        }

        void listen(const std::shared_ptr<server_state> state, scoped_handle instance)
        {
            const scoped_handle connect_event { make_manual_reset_event() };
            if (connect_event.valid() == false)
                return;

            while (instance.valid())
            {
                ResetEvent(connect_event.get());
                OVERLAPPED overlapped {};
                overlapped.hEvent = connect_event.get();

                bool connected { false };
                if (ConnectNamedPipe(instance.get(), &overlapped) != FALSE)
                {
                    connected = true;
                }
                else
                {
                    const DWORD error { GetLastError() };
                    if (error == ERROR_PIPE_CONNECTED)
                    {
                        connected = true;
                    }
                    else if (error == ERROR_IO_PENDING)
                    {
                        const HANDLE handles[] { connect_event.get(), state->stop_event.get() };
                        const DWORD waited { WaitForMultipleObjects(2, handles, FALSE, INFINITE) };
                        if (waited != WAIT_OBJECT_0)
                        {
                            CancelIoEx(instance.get(), &overlapped);
                            DWORD ignored { 0 };
                            GetOverlappedResult(instance.get(), &overlapped, &ignored, TRUE);
                            return;
                        }
                        DWORD ignored { 0 };
                        connected = GetOverlappedResult(instance.get(), &overlapped, &ignored, FALSE) != FALSE;
                    }
                }

                if (connected == false)
                    return;

                // 다음 인스턴스를 먼저 만들어 이름이 비는 순간을 없앤다.
                DWORD error { 0 };
                scoped_handle next { create_pipe_instance(*state, false, error) };

                bool accepted { false };
                {
                    const std::lock_guard<std::mutex> lock { state->mutex };
                    if (state->stopping == false && state->active_connections < maximum_active_connections)
                    {
                        ++state->active_connections;
                        accepted = true;
                    }
                }
                if (accepted)
                {
                    try
                    {
                        std::thread { serve_connection, state, std::move(instance) }.detach();
                    }
                    catch (...)
                    {
                        // 스레드를 못 만들면 연결을 닫고(instance가 소멸한다) 계속 받는다.
                        const std::lock_guard<std::mutex> lock { state->mutex };
                        --state->active_connections;
                    }
                }
                instance = std::move(next);
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

            void start_listening(scoped_handle first_instance)
            {
                listener_ = std::thread { listen, state_, std::move(first_instance) };
            }

            [[nodiscard]] askpass_endpoint endpoint() const override
            {
                std::u8string pipe_name {};
                for (const wchar_t character : state_->pipe_name)
                    pipe_name.push_back(static_cast<char8_t>(character));
                return askpass_endpoint { std::move(pipe_name), to_u8(state_->token) };
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
                SetEvent(state_->stop_event.get());
                if (listener_.joinable())
                    listener_.join();

                // 연결 스레드는 중지 신호로 곧 풀린다. 그래도 핸들러가 늦으면 예산을 넘겨 기다리지
                // 않고 놓아 준다. 스레드는 state를 공유 소유하므로 놓아도 안전하다.
                std::unique_lock<std::mutex> lock { state_->mutex };
                state_->idle.wait_for(lock, connection_drain_limit, [this]() { return state_->active_connections == 0; });
            }

        private:
            std::shared_ptr<server_state> state_ {};
            std::thread listener_ {};
            std::mutex shutdown_mutex_ {};
        };
    } // namespace

    askpass_server_start_result start_askpass_server(askpass_prompt_handler handler)
    {
        askpass_server_start_result result {};

        const auto state { std::make_shared<server_state>() };
        state->handler = std::move(handler);
        state->stop_event = make_manual_reset_event();
        if (state->stop_event.valid() == false)
        {
            result.native_error = GetLastError();
            return result;
        }

        std::string suffix {};
        if (random_hex(16, state->token) == false || random_hex(8, suffix) == false)
        {
            result.native_error = static_cast<std::uint32_t>(ERROR_GEN_FAILURE);
            return result;
        }
        std::wstring name { L"\\\\.\\pipe\\cyella-askpass-" + std::to_wstring(GetCurrentProcessId()) + L"-" };
        for (const char character : suffix)
            name.push_back(static_cast<wchar_t>(character));
        state->pipe_name = std::move(name);

        DWORD error { 0 };
        if (state->security.build(error) == false)
        {
            result.native_error = error;
            return result;
        }
        scoped_handle first { create_pipe_instance(*state, true, error) };
        if (first.valid() == false)
        {
            result.native_error = error;
            return result;
        }

        auto server { std::make_unique<askpass_server>(state) };
        server->start_listening(std::move(first));
        result.server = std::move(server);
        return result;
    }
} // namespace cyella::win32
