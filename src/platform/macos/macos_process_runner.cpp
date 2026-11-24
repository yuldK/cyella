#include "platform/win32/win32_process_runner.h"

#include "domain/text_catalog.h"
#include "infrastructure/process_output_pipeline.h"
#include "infrastructure/secret_masking.h"
#include "platform/macos/posix_file.h"
#include "platform/win32/win32_text_transcoder.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

// `platform/win32/win32_process_runner.h`의 macOS 구현이다. Win32 판과 같은 계약을 POSIX로 세운다.
//
//  - 자식은 `posix_spawn`으로 띄운다. `POSIX_SPAWN_CLOEXEC_DEFAULT`(Apple 확장)가 파일 동작에 적은 stdin·stdout·stderr 말고는
//    어떤 기술자도 자식에게 넘기지 않는다 — 다른 thread가 동시에 연 pipe가 새지 않는다 (Win32 판의 상속 목록 자리).
//  - 자식은 새 프로세스 그룹의 우두머리다 (`POSIX_SPAWN_SETPGROUP`). 취소·시간 초과는 그룹 전체에 SIGKILL을 보내 손자까지
//    정리한다 (Win32 판의 kill-on-close job 자리). 콘솔 없는 자식에게 보낼 안전한 graceful signal이 없다는 판단도 같다.
//  - 기다림은 kqueue 하나다: 자식의 종료(`EVFILT_PROC`/`NOTE_EXIT`), 취소(`EVFILT_USER`), 시간 초과(kevent의 제한 시간).
//  - pipe마다 I/O thread가 하나다. 각 thread는 `poll`로 자료와 깨우기 pipe를 함께 기다려, 손자가 쓰기 끝을 쥐고 남아도
//    유예가 지나면 수집을 끝낼 수 있다 (Win32 판의 CancelSynchronousIo 자리).
//  - 쓰기 끝에 `F_SETNOSIGPIPE`를 건다. 입력을 다 읽지 않고 끝난 자식에게 쓰면 프로세스가 SIGPIPE로 죽지 않고 EPIPE가 된다.
namespace cyella::win32 {
    namespace {
        // 자식이 끝난 뒤 손자가 쥔 쓰기 끝 때문에 EOF가 오지 않을 때 기다리는 유예다. Win32 판과 같다.
        constexpr std::chrono::milliseconds reader_drain_grace { 2'000 };
        constexpr int maximum_io_threads { 3 };

        diagnostic make_process_diagnostic(
            const diagnostic_code code, std::u8string message, const std::optional<std::uint32_t> native_error = std::nullopt, const diagnostic_severity severity = diagnostic_severity::error)
        {
            diagnostic value {};
            value.code = code;
            value.severity = severity;
            value.message = std::move(message);
            value.native_error = native_error;
            return value;
        }

        std::uint32_t errno_or(const int fallback) noexcept
        {
            return static_cast<std::uint32_t>(errno != 0 ? errno : fallback);
        }

        void finish_result(process_result& result, const std::chrono::steady_clock::time_point started)
        {
            result.finished_at = std::chrono::system_clock::now();
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        }

        // 기록용 명령줄이다. 셸이 읽는 꼴(작은따옴표)로 적어 사람이 그대로 붙여 넣을 수 있게 한다. 실행에는 쓰지 않는다.
        std::u8string display_command_line(const std::u8string& executable, const std::vector<std::u8string>& arguments)
        {
            const auto quoted = [](const std::u8string_view text) {
                constexpr std::u8string_view plain { u8"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./=:,+@%" };
                if (text.empty() == false && text.find_first_not_of(plain) == std::u8string_view::npos)
                    return std::u8string { text };
                std::u8string value { u8"'" };
                for (const char8_t character : text)
                    if (character == u8'\'')
                        value.append(u8"'\\''");
                    else
                        value.push_back(character);
                value.push_back(u8'\'');
                return value;
            };
            std::u8string line { quoted(executable) };
            for (const std::u8string& argument : arguments)
            {
                line.push_back(u8' ');
                line += quoted(argument);
            }
            return line;
        }

        // 부모 환경 위에 override를 얹은 `NAME=value` 목록이다. POSIX의 이름은 대소문자를 가린다.
        std::vector<std::string> make_environment(const std::vector<process_environment_override>& overrides)
        {
            std::vector<std::string> entries {};
            for (char** cursor { environ }; cursor != nullptr && *cursor != nullptr; ++cursor)
                entries.emplace_back(*cursor);
            for (const process_environment_override& entry : overrides)
            {
                const std::string name { macos::to_narrow(entry.name) };
                const auto existing { std::ranges::find_if(entries, [&name](const std::string& value) { return value.size() > name.size() && value.compare(0, name.size(), name) == 0 && value[name.size()] == '='; }) };
                if (entry.value.has_value() == false)
                {
                    if (existing != entries.end())
                        entries.erase(existing);
                    continue;
                }
                std::string assignment { name + "=" + macos::to_narrow(*entry.value) };
                if (existing != entries.end())
                    *existing = std::move(assignment);
                else
                    entries.push_back(std::move(assignment));
            }
            return entries;
        }

        struct pipe_pair
        {
            macos::unique_fd read {};
            macos::unique_fd write {};
        };

        // 두 끝 모두 CLOEXEC다. 자식에게는 posix_spawn의 dup2가 0·1·2로 넘긴다 (dup2된 기술자는 CLOEXEC가 풀린다).
        bool make_pipe(pipe_pair& pair) noexcept
        {
            int ends[2] { -1, -1 };
            if (::pipe(ends) != 0)
                return false;
            pair.read.reset(ends[0]);
            pair.write.reset(ends[1]);
            return ::fcntl(ends[0], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(ends[1], F_SETFD, FD_CLOEXEC) == 0;
        }

        // I/O thread를 멈추라고 알리는 pipe다. 읽기 끝이 읽을 수 있게 되면 poll이 깬다.
        class wake_signal
        {
        public:
            [[nodiscard]] bool open() noexcept
            {
                return make_pipe(pipe_) && ::fcntl(pipe_.write.get(), F_SETFL, O_NONBLOCK) == 0;
            }

            void raise() noexcept
            {
                if (raised_.exchange(true))
                    return;
                const char byte { 1 };
                static_cast<void>(::write(pipe_.write.get(), &byte, 1));
            }

            [[nodiscard]] int fd() const noexcept
            {
                return pipe_.read.get();
            }

        private:
            pipe_pair pipe_ {};
            std::atomic<bool> raised_ { false };
        };

        // 레코드에 실행 단위 sequence를 부여하고 sink 호출을 직렬화한다 (Win32 판과 같다).
        class output_collector
        {
        public:
            explicit output_collector(process_output_sink* const sink) noexcept
                : sink_ { sink }
            {}

            void submit(process_output_record& record) noexcept
            {
                const std::lock_guard<std::mutex> guard { mutex_ };
                record.sequence = next_sequence_++;
                ++count_;
                if (sink_ == nullptr)
                    return;
                try
                {
                    sink_->on_record(record);
                }
                catch (...)
                {
                    // sink 예외가 reader thread를 끝내면 자식이 pipe에서 막힌다.
                    sink_failed_ = true;
                }
            }

            void submit_chunk(const process_stream stream, const std::string_view bytes) noexcept
            {
                const std::lock_guard<std::mutex> guard { mutex_ };
                const process_output_chunk chunk { next_sequence_++, stream, bytes };
                ++count_;
                if (sink_ == nullptr)
                    return;
                try
                {
                    sink_->on_chunk(chunk);
                }
                catch (...)
                {
                    sink_failed_ = true;
                }
            }

            [[nodiscard]] std::uint64_t count() const noexcept
            {
                const std::lock_guard<std::mutex> guard { mutex_ };
                return count_;
            }

            [[nodiscard]] bool sink_failed() const noexcept
            {
                const std::lock_guard<std::mutex> guard { mutex_ };
                return sink_failed_;
            }

        private:
            mutable std::mutex mutex_ {};
            std::uint64_t next_sequence_ { 1 };
            std::uint64_t count_ { 0 };
            bool sink_failed_ { false };
            process_output_sink* sink_ { nullptr };
        };

        // 끝난 I/O thread를 센다. 유예 안에 모두 끝났는지 기다리는 데 쓴다.
        class io_completion
        {
        public:
            void finished() noexcept
            {
                const std::lock_guard<std::mutex> guard { mutex_ };
                ++finished_;
                changed_.notify_all();
            }

            [[nodiscard]] bool wait_all(const int count, const std::chrono::milliseconds timeout)
            {
                std::unique_lock<std::mutex> lock { mutex_ };
                return changed_.wait_for(lock, timeout, [this, count] { return finished_ >= count; });
            }

        private:
            std::mutex mutex_ {};
            std::condition_variable changed_ {};
            int finished_ { 0 };
        };

        enum class read_step
        {
            data,
            end,
            stopped,
            failed,
        };

        // 자료나 깨우기를 기다려 한 번 읽는다.
        read_step read_some(const int fd, const int wake, char* const buffer, const std::size_t capacity, std::size_t& received) noexcept
        {
            while (true)
            {
                pollfd watched[2] { { fd, POLLIN, 0 }, { wake, POLLIN, 0 } };
                const int ready { ::poll(watched, 2, -1) };
                if (ready < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return read_step::failed;
                }
                // 자료가 먼저다. 깨우기와 자료가 함께 왔으면 남은 자료를 다 읽은 뒤 멈춘다.
                if ((watched[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                {
                    const ssize_t count { ::read(fd, buffer, capacity) };
                    if (count < 0)
                    {
                        if (errno == EINTR || errno == EAGAIN)
                            continue;
                        return read_step::failed;
                    }
                    if (count == 0)
                        return read_step::end;
                    received = static_cast<std::size_t>(count);
                    return read_step::data;
                }
                if ((watched[1].revents & POLLIN) != 0)
                    return read_step::stopped;
            }
        }

        void read_output_stream(const int fd, const int wake, process_output_pipeline& pipeline, output_collector& collector, std::atomic<bool>& read_failed) noexcept
        {
            const process_output_pipeline::record_handler handler { [&collector](process_output_record& record) { collector.submit(record); } };
            try
            {
                std::u8string buffer(process_read_block_byte_size, u8'\0');
                while (true)
                {
                    std::size_t received { 0 };
                    const read_step step { read_some(fd, wake, reinterpret_cast<char*>(buffer.data()), buffer.size(), received) };
                    if (step == read_step::failed)
                    {
                        read_failed.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (step != read_step::data)
                        break;
                    pipeline.append(std::u8string_view { buffer.data(), received }, handler);
                }
                pipeline.flush(handler);
            }
            catch (...)
            {
                read_failed.store(true, std::memory_order_relaxed);
            }
        }

        struct raw_stream_state
        {
            std::size_t captured_bytes { 0 };
            bool truncated { false };
        };

        void read_raw_stream(const int fd, const int wake, const process_stream stream, const std::size_t maximum_captured_bytes, raw_stream_state& state, output_collector& collector,
            std::atomic<bool>& read_failed) noexcept
        {
            try
            {
                std::string buffer(process_read_block_byte_size, '\0');
                while (true)
                {
                    std::size_t received { 0 };
                    const read_step step { read_some(fd, wake, buffer.data(), buffer.size(), received) };
                    if (step == read_step::failed)
                    {
                        read_failed.store(true, std::memory_order_relaxed);
                        break;
                    }
                    if (step != read_step::data)
                        break;
                    const std::size_t usable { std::min(maximum_captured_bytes - state.captured_bytes, received) };
                    if (usable != 0)
                    {
                        state.captured_bytes += usable;
                        collector.submit_chunk(stream, std::string_view { buffer.data(), usable });
                    }
                    if (usable < received)
                        state.truncated = true;
                }
            }
            catch (...)
            {
                read_failed.store(true, std::memory_order_relaxed);
            }
        }

        // 표준 입력을 쓰고 닫아 자식이 EOF를 보게 한다. 자식이 다 읽기 전에 끝나 EPIPE가 되는 것은 정상 경로다.
        void write_standard_input(macos::unique_fd& pipe, const int wake, const std::string& data, std::atomic<std::uint32_t>& unexpected_error) noexcept
        {
            std::size_t offset { 0 };
            while (offset < data.size())
            {
                pollfd watched[2] { { pipe.get(), POLLOUT, 0 }, { wake, POLLIN, 0 } };
                const int ready { ::poll(watched, 2, -1) };
                if (ready < 0)
                {
                    if (errno == EINTR)
                        continue;
                    unexpected_error.store(static_cast<std::uint32_t>(errno), std::memory_order_relaxed);
                    break;
                }
                if ((watched[1].revents & POLLIN) != 0)
                    break;
                if ((watched[0].revents & (POLLERR | POLLHUP)) != 0 && (watched[0].revents & POLLOUT) == 0)
                    break;
                const ssize_t written { ::write(pipe.get(), data.data() + offset, std::min(data.size() - offset, process_read_block_byte_size)) };
                if (written < 0)
                {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    if (errno != EPIPE)
                        unexpected_error.store(static_cast<std::uint32_t>(errno), std::memory_order_relaxed);
                    break;
                }
                offset += static_cast<std::size_t>(written);
            }
            pipe.reset();
        }

        // 자식의 프로세스 그룹 전체를 끝낸다. 아직 거두지 않은 자식이면 그룹이 없을 때(자식이 그룹을 떠났다) 자식 하나에도 보낸다.
        //  - 거둔 뒤에는 그룹에만 보낸다. 거둔 pid는 다른 프로세스가 다시 받을 수 있지만, 남은 구성원이 있는 그룹의 번호는
        //    다시 쓰이지 않는다.
        void terminate_tree(const pid_t child, const bool reaped = false) noexcept
        {
            if (::kill(-child, SIGKILL) != 0 && reaped == false)
                static_cast<void>(::kill(child, SIGKILL));
        }

        // 자식을 거둔다. 신호에 끊기면 다시 기다린다.
        std::optional<int> reap(const pid_t child) noexcept
        {
            int status { 0 };
            while (true)
            {
                const pid_t waited { ::waitpid(child, &status, 0) };
                if (waited == child)
                    return status;
                if (waited < 0 && errno == EINTR)
                    continue;
                return std::nullopt;
            }
        }

        class spawn_resources
        {
        public:
            spawn_resources() noexcept
            {
                actions_ready_ = ::posix_spawn_file_actions_init(&actions_) == 0;
                attributes_ready_ = ::posix_spawnattr_init(&attributes_) == 0;
            }

            spawn_resources(const spawn_resources&) = delete;
            spawn_resources& operator=(const spawn_resources&) = delete;

            ~spawn_resources()
            {
                if (actions_ready_)
                    static_cast<void>(::posix_spawn_file_actions_destroy(&actions_));
                if (attributes_ready_)
                    static_cast<void>(::posix_spawnattr_destroy(&attributes_));
            }

            [[nodiscard]] bool ready() const noexcept
            {
                return actions_ready_ && attributes_ready_;
            }

            posix_spawn_file_actions_t actions_ {};
            posix_spawnattr_t attributes_ {};

        private:
            bool actions_ready_ { false };
            bool attributes_ready_ { false };
        };

        process_result make_unexpected_failure_result() noexcept
        {
            process_result result {};
            result.completion = process_completion::start_failed;
            result.native_error = static_cast<std::uint32_t>(ENOMEM);
            try
            {
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.unexpected_failure"), result.native_error));
            }
            catch (...)
            {
                // 진단 문자열조차 만들 수 없는 상황이므로 구조화된 결과만 반환한다.
            }
            return result;
        }

        process_result run_impl(const process_request& request, process_output_sink* const sink, const process_cancellation_token& token)
        {
            process_result result {};
            std::vector<diagnostic> validation { validate_process_request(request) };
            if (validation.empty() == false)
            {
                result.completion = process_completion::invalid_request;
                result.diagnostics = std::move(validation);
                return result;
            }

            if (token.cancelled())
            {
                result.completion = process_completion::cancelled;
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_cancelled, tr(u8"process.cancelled_before_start")));
                return result;
            }

            result.masked_command_line = mask_secrets(display_command_line(request.executable, request.arguments));

            const std::string executable { macos::to_narrow(request.executable) };
            const std::string working_directory { macos::to_narrow(request.working_directory) };
            std::vector<std::string> argument_storage {};
            argument_storage.reserve(request.arguments.size() + 1);
            argument_storage.push_back(executable);
            for (const std::u8string& argument : request.arguments)
                argument_storage.push_back(macos::to_narrow(argument));
            // 인자 가운데의 NUL은 C 문자열에서 잘려 다른 인자가 된다. 요청 검사는 실행 파일·폴더만 본다.
            if (std::ranges::any_of(argument_storage, [](const std::string& value) { return value.find('\0') != std::string::npos; }))
            {
                result.completion = process_completion::start_failed;
                result.native_error = static_cast<std::uint32_t>(EINVAL);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.request_conversion_failed"), result.native_error));
                return result;
            }
            std::vector<std::string> environment_storage { make_environment(request.environment_overrides) };
            if (std::ranges::any_of(environment_storage, [](const std::string& value) { return value.find('\0') != std::string::npos; }))
            {
                result.completion = process_completion::start_failed;
                result.native_error = static_cast<std::uint32_t>(EINVAL);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.environment_conversion_failed"), result.native_error));
                return result;
            }
            std::vector<char*> argv {};
            for (std::string& value : argument_storage)
                argv.push_back(value.data());
            argv.push_back(nullptr);
            std::vector<char*> envp {};
            for (std::string& value : environment_storage)
                envp.push_back(value.data());
            envp.push_back(nullptr);

            // 입력 내용이 없으면 자식이 프롬프트를 읽어도 곧바로 EOF가 되게 stdin은 /dev/null이다.
            const bool has_standard_input { request.standard_input.empty() == false };
            pipe_pair standard_input {};
            pipe_pair standard_output {};
            pipe_pair standard_error {};
            wake_signal wake {};
            errno = 0;
            if ((has_standard_input && make_pipe(standard_input) == false) || make_pipe(standard_output) == false || make_pipe(standard_error) == false || wake.open() == false)
            {
                result.completion = process_completion::start_failed;
                result.native_error = errno_or(EMFILE);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.standard_handles_failed"), result.native_error));
                return result;
            }
            if (has_standard_input)
            {
                static_cast<void>(::fcntl(standard_input.write.get(), F_SETNOSIGPIPE, 1));
                static_cast<void>(::fcntl(standard_input.write.get(), F_SETFL, O_NONBLOCK));
            }

            // 취소를 받을 kqueue다. 콜백은 사용자 사건 하나를 울리기만 한다 (Win32 판의 event 자리).
            const macos::unique_fd queue { ::kqueue() };
            if (queue.valid() == false)
            {
                result.completion = process_completion::start_failed;
                result.native_error = errno_or(EMFILE);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.cancel_event_failed"), result.native_error));
                return result;
            }
            constexpr std::uintptr_t cancel_identifier { 1 };
            {
                struct kevent change {};
                EV_SET(&change, cancel_identifier, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
                if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) != 0)
                {
                    result.completion = process_completion::start_failed;
                    result.native_error = errno_or(EINVAL);
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.cancel_event_failed"), result.native_error));
                    return result;
                }
            }

            spawn_resources spawn {};
            bool prepared { spawn.ready() };
            prepared = prepared && ::posix_spawnattr_setflags(&spawn.attributes_, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK) == 0;
            prepared = prepared && ::posix_spawnattr_setpgroup(&spawn.attributes_, 0) == 0;
            // 앱이 막아 둔 신호(SIGPIPE 등)를 자식이 물려받지 않게 기본 동작으로 되돌리고 가림을 푼다.
            sigset_t defaults {};
            sigemptyset(&defaults);
            sigaddset(&defaults, SIGPIPE);
            sigaddset(&defaults, SIGINT);
            sigaddset(&defaults, SIGTERM);
            sigaddset(&defaults, SIGCHLD);
            sigset_t empty_mask {};
            sigemptyset(&empty_mask);
            prepared = prepared && ::posix_spawnattr_setsigdefault(&spawn.attributes_, &defaults) == 0;
            prepared = prepared && ::posix_spawnattr_setsigmask(&spawn.attributes_, &empty_mask) == 0;
            if (has_standard_input)
                prepared = prepared && ::posix_spawn_file_actions_adddup2(&spawn.actions_, standard_input.read.get(), STDIN_FILENO) == 0;
            else
                prepared = prepared && ::posix_spawn_file_actions_addopen(&spawn.actions_, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0;
            prepared = prepared && ::posix_spawn_file_actions_adddup2(&spawn.actions_, standard_output.write.get(), STDOUT_FILENO) == 0;
            prepared = prepared && ::posix_spawn_file_actions_adddup2(&spawn.actions_, standard_error.write.get(), STDERR_FILENO) == 0;
            prepared = prepared && ::posix_spawn_file_actions_addchdir_np(&spawn.actions_, working_directory.c_str()) == 0;
            if (prepared == false)
            {
                result.completion = process_completion::start_failed;
                result.native_error = errno_or(EINVAL);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.inherited_handles_failed"), result.native_error));
                return result;
            }

            const std::chrono::steady_clock::time_point started { std::chrono::steady_clock::now() };
            result.started_at = std::chrono::system_clock::now();
            pid_t child { -1 };
            const int spawned { ::posix_spawn(&child, executable.c_str(), &spawn.actions_, &spawn.attributes_, argv.data(), envp.data()) };
            if (spawned != 0)
            {
                result.completion = process_completion::start_failed;
                result.native_error = static_cast<std::uint32_t>(spawned);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.start_failed"), result.native_error));
                finish_result(result, started);
                return result;
            }

            // 자식의 종료를 kqueue에 건다. 이미 끝났으면(ESRCH) 기다림 없이 거둔다.
            bool exit_watched { false };
            {
                struct kevent change {};
                EV_SET(&change, static_cast<std::uintptr_t>(child), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
                exit_watched = ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) == 0;
            }

            // 자식만 써야 하는 끝을 닫는다. 닫지 않으면 자식이 끝나도 부모가 쥔 쓰기 끝 때문에 EOF가 오지 않는다.
            standard_input.read.reset();
            standard_output.write.reset();
            standard_error.write.reset();

            std::unique_ptr<text_transcoder> transcoder {};
            if (request.text_encoding == process_text_encoding::active_code_page_fallback)
                transcoder = make_active_code_page_transcoder();
            process_output_pipeline output_pipeline {
                process_stream::standard_output, request.maximum_record_bytes, request.maximum_captured_bytes_per_stream, request.text_encoding, transcoder.get(),
            };
            process_output_pipeline error_pipeline {
                process_stream::standard_error, request.maximum_record_bytes, request.maximum_captured_bytes_per_stream, request.text_encoding, transcoder.get(),
            };
            raw_stream_state raw_output {};
            raw_stream_state raw_error {};
            output_collector collector { sink };
            std::atomic<bool> read_failed { false };
            std::atomic<std::uint32_t> input_error { 0 };
            io_completion completion {};

            const auto start_reader = [&](std::vector<std::thread>& threads, pipe_pair& channel, const process_stream stream, const process_output_mode mode, process_output_pipeline& pipeline,
                                          raw_stream_state& raw) {
                const int fd { channel.read.get() };
                const int wake_fd { wake.fd() };
                if (mode == process_output_mode::raw)
                    threads.emplace_back([fd, wake_fd, stream, &request, &raw, &collector, &read_failed, &completion]() {
                        read_raw_stream(fd, wake_fd, stream, request.maximum_captured_bytes_per_stream, raw, collector, read_failed);
                        completion.finished();
                    });
                else
                    threads.emplace_back([fd, wake_fd, &pipeline, &collector, &read_failed, &completion]() {
                        read_output_stream(fd, wake_fd, pipeline, collector, read_failed);
                        completion.finished();
                    });
            };

            // I/O thread를 모두 끝내고 거둔다. 유예 안에 끝났으면 true다. 어떤 경로로든 join은 반드시 끝난다.
            const auto join_io_threads = [&](std::vector<std::thread>& threads) noexcept {
                bool drained_promptly { true };
                const int count { static_cast<int>(threads.size()) };
                bool all_finished { false };
                try
                {
                    all_finished = completion.wait_all(count, reader_drain_grace);
                }
                catch (...)
                {
                    all_finished = false;
                }
                if (all_finished == false)
                {
                    // 손자가 쓰기 끝을 쥐고 있다. 그룹을 정리하고, 그래도 남으면 thread를 깨워 수집을 끝낸다.
                    drained_promptly = false;
                    terminate_tree(child, true);
                    bool finished_after_kill { false };
                    try
                    {
                        finished_after_kill = completion.wait_all(count, reader_drain_grace);
                    }
                    catch (...)
                    {
                        finished_after_kill = false;
                    }
                    if (finished_after_kill == false)
                        wake.raise();
                }
                for (std::thread& thread : threads)
                    if (thread.joinable())
                        thread.join();
                return drained_promptly;
            };

            std::vector<std::thread> io_threads {};
            try
            {
                io_threads.reserve(maximum_io_threads);
                start_reader(io_threads, standard_output, process_stream::standard_output, request.standard_output_mode, output_pipeline, raw_output);
                start_reader(io_threads, standard_error, process_stream::standard_error, request.standard_error_mode, error_pipeline, raw_error);
                if (has_standard_input)
                    io_threads.emplace_back([&standard_input, &wake, &request, &input_error, &completion]() {
                        write_standard_input(standard_input.write, wake.fd(), request.standard_input, input_error);
                        completion.finished();
                    });
            }
            catch (...)
            {
                terminate_tree(child);
                wake.raise();
                for (std::thread& thread : io_threads)
                    if (thread.joinable())
                        thread.join();
                static_cast<void>(reap(child));
                result.completion = process_completion::internal_error;
                result.native_error = static_cast<std::uint32_t>(ENOMEM);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.io_thread_failed"), result.native_error));
                finish_result(result, started);
                return result;
            }

            bool child_reaped { false };
            try
            {
                enum class wait_outcome
                {
                    exited,
                    cancelled,
                    timed_out,
                    failed,
                };
                wait_outcome outcome { wait_outcome::exited };
                std::uint32_t wait_error { 0 };
                {
                    // 취소 콜백은 사용자 사건 하나만 울린다. registration은 이 블록에서 풀린다.
                    const int queue_fd { queue.get() };
                    const auto trigger_cancel = [queue_fd]() {
                        struct kevent trigger {};
                        EV_SET(&trigger, cancel_identifier, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
                        static_cast<void>(::kevent(queue_fd, &trigger, 1, nullptr, 0, nullptr));
                    };
                    const process_cancellation_registration registration { token.register_callback(trigger_cancel) };

                    if (exit_watched == false)
                        outcome = wait_outcome::exited;
                    else
                    {
                        const std::optional<std::chrono::steady_clock::time_point> deadline {
                            request.timeout.has_value() ? std::optional { started + *request.timeout } : std::nullopt,
                        };
                        while (true)
                        {
                            timespec remaining {};
                            const timespec* limit { nullptr };
                            if (deadline.has_value())
                            {
                                const auto left { std::max(std::chrono::nanoseconds { 0 }, std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - std::chrono::steady_clock::now())) };
                                remaining.tv_sec = static_cast<time_t>(left.count() / 1'000'000'000);
                                remaining.tv_nsec = static_cast<long>(left.count() % 1'000'000'000);
                                limit = &remaining;
                            }
                            struct kevent fired {};
                            const int count { ::kevent(queue.get(), nullptr, 0, &fired, 1, limit) };
                            if (count < 0)
                            {
                                if (errno == EINTR)
                                    continue;
                                outcome = wait_outcome::failed;
                                wait_error = errno_or(EINVAL);
                                break;
                            }
                            if (count == 0)
                            {
                                outcome = wait_outcome::timed_out;
                                break;
                            }
                            if (fired.filter == EVFILT_PROC)
                            {
                                outcome = wait_outcome::exited;
                                break;
                            }
                            if (fired.filter == EVFILT_USER)
                            {
                                outcome = wait_outcome::cancelled;
                                break;
                            }
                        }
                    }
                }

                // 자식이 스스로 끝난 경우가 아니면 그룹을 정리한다. 그러면 pipe가 닫혀 reader가 EOF를 본다.
                if (outcome != wait_outcome::exited)
                    terminate_tree(child);
                const std::optional<int> status { reap(child) };
                child_reaped = true;
                const bool drained_promptly { join_io_threads(io_threads) };

                result.captured_bytes = output_pipeline.captured_bytes() + error_pipeline.captured_bytes() + raw_output.captured_bytes + raw_error.captured_bytes;
                result.output_truncated = output_pipeline.truncated() || error_pipeline.truncated() || raw_output.truncated || raw_error.truncated;
                result.standard_output_truncated = output_pipeline.truncated() || raw_output.truncated;
                result.record_count = collector.count();
                if (result.output_truncated)
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_output_truncated, tr(u8"process.output_truncated"), std::nullopt, diagnostic_severity::warning));
                if (collector.sink_failed())
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.output_sink_threw")));
                if (drained_promptly == false)
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.inherited_pipe_lingered"), std::nullopt, diagnostic_severity::warning));
                if (input_error.load(std::memory_order_relaxed) != 0)
                    result.diagnostics.push_back(
                        make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.stdin_write_failed"), input_error.load(std::memory_order_relaxed), diagnostic_severity::warning));
                if (read_failed.load(std::memory_order_relaxed))
                {
                    result.output_truncated = true;
                    result.standard_output_truncated = true;
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.output_thread_failed")));
                }

                if (outcome == wait_outcome::timed_out)
                {
                    result.completion = process_completion::timed_out;
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_timed_out, tr(u8"process.timed_out")));
                    finish_result(result, started);
                    return result;
                }
                if (outcome == wait_outcome::cancelled)
                {
                    result.completion = process_completion::cancelled;
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_cancelled, tr(u8"process.cancelled")));
                    finish_result(result, started);
                    return result;
                }
                if (outcome == wait_outcome::failed || status.has_value() == false)
                {
                    result.completion = process_completion::internal_error;
                    result.native_error = outcome == wait_outcome::failed ? wait_error : static_cast<std::uint32_t>(ECHILD);
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.wait_failed"), result.native_error));
                    finish_result(result, started);
                    return result;
                }

                result.completion = process_completion::exited;
                // 신호로 끝났으면 셸처럼 128 + 신호 번호로 적는다. 0이 아니므로 실패로 읽힌다.
                if (WIFEXITED(*status))
                    result.exit_code = WEXITSTATUS(*status);
                else if (WIFSIGNALED(*status))
                    result.exit_code = 128 + WTERMSIG(*status);
                else
                    result.exit_code = -1;
                finish_result(result, started);
                return result;
            }
            catch (...)
            {
                terminate_tree(child, child_reaped);
                if (child_reaped == false)
                    static_cast<void>(reap(child));
                wake.raise();
                for (std::thread& thread : io_threads)
                    if (thread.joinable())
                        thread.join();
                result.completion = process_completion::internal_error;
                result.native_error = static_cast<std::uint32_t>(ENOMEM);
                try
                {
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.monitor_exception"), result.native_error));
                }
                catch (...)
                {
                    // 진단 문자열조차 만들 수 없는 상황이므로 구조화된 결과만 반환한다.
                }
                finish_result(result, started);
                return result;
            }
        }

        class posix_process_runner final : public process_runner
        {
        public:
            [[nodiscard]] process_result run(const process_request& request, process_output_sink* const sink, const process_cancellation_token& token) noexcept override
            {
                try
                {
                    return run_impl(request, sink, token);
                }
                catch (...)
                {
                    return make_unexpected_failure_result();
                }
            }
        };
    } // namespace

    std::unique_ptr<process_runner> make_process_runner()
    {
        return std::make_unique<posix_process_runner>();
    }
} // namespace cyella::win32
