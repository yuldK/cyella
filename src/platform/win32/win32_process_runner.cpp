#include "platform/win32/win32_process_runner.h"

#include "domain/text_catalog.h"
#include "infrastructure/command_line_builder.h"
#include "infrastructure/process_output_pipeline.h"
#include "infrastructure/secret_masking.h"
#include "platform/win32/utf8.h"
#include "platform/win32/win32_text_transcoder.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cyella::win32 {
    namespace {
        class unique_handle
        {
        public:
            unique_handle() noexcept = default;

            explicit unique_handle(const HANDLE value) noexcept
                : value_ { value }
            {}

            unique_handle(const unique_handle&) = delete;
            unique_handle& operator=(const unique_handle&) = delete;

            unique_handle(unique_handle&& other) noexcept
                : value_ { other.release() }
            {}

            unique_handle& operator=(unique_handle&& other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    value_ = other.release();
                }
                return *this;
            }

            ~unique_handle()
            {
                reset();
            }

            [[nodiscard]] HANDLE get() const noexcept
            {
                return value_;
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
            }

            [[nodiscard]] HANDLE release() noexcept
            {
                const HANDLE value { value_ };
                value_ = INVALID_HANDLE_VALUE;
                return value;
            }

            void reset(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
            {
                if (valid())
                    CloseHandle(value_);
                value_ = value;
            }

        private:
            HANDLE value_ { INVALID_HANDLE_VALUE };
        };

        // 상속할 handle을 명시하지 않으면 부모의 다른 상속 가능 handle까지 자식에게
        // 넘어간다. 그래서 attribute list로 stdin, stdout, stderr만 상속시킨다.
        class unique_attribute_list
        {
        public:
            unique_attribute_list() noexcept = default;
            unique_attribute_list(const unique_attribute_list&) = delete;
            unique_attribute_list(unique_attribute_list&&) = delete;
            unique_attribute_list& operator=(const unique_attribute_list&) = delete;
            unique_attribute_list& operator=(unique_attribute_list&&) = delete;

            ~unique_attribute_list()
            {
                if (initialized_)
                    DeleteProcThreadAttributeList(get());
            }

            [[nodiscard]] bool initialize(const DWORD count)
            {
                SIZE_T required { 0 };
                InitializeProcThreadAttributeList(nullptr, count, 0, &required);
                if (required == 0)
                    return false;

                storage_.assign(static_cast<std::size_t>(required), std::byte {});
                if (InitializeProcThreadAttributeList(get(), count, 0, &required) == FALSE)
                    return false;
                initialized_ = true;
                return true;
            }

            [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept
            {
                return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
            }

        private:
            std::vector<std::byte> storage_ {};
            bool initialized_ { false };
        };

        class environment_strings
        {
        public:
            environment_strings() noexcept
                : value_ { GetEnvironmentStringsW() }
            {}

            environment_strings(const environment_strings&) = delete;
            environment_strings(environment_strings&&) = delete;
            environment_strings& operator=(const environment_strings&) = delete;
            environment_strings& operator=(environment_strings&&) = delete;

            ~environment_strings()
            {
                if (value_ != nullptr)
                    FreeEnvironmentStringsW(value_);
            }

            [[nodiscard]] const wchar_t* get() const noexcept
            {
                return value_;
            }

        private:
            wchar_t* value_ { nullptr };
        };

        struct environment_entry
        {
            std::wstring name {};
            std::wstring value {};
        };

        struct environment_block
        {
            // 값이 없고 실패도 아니면 부모 환경을 그대로 상속한다.
            std::optional<std::wstring> block {};
            std::uint32_t native_error { ERROR_SUCCESS };
            bool failed { false };
        };

        std::uint32_t last_error_or(const std::uint32_t fallback) noexcept
        {
            const DWORD error { GetLastError() };
            return error == ERROR_SUCCESS ? fallback : static_cast<std::uint32_t>(error);
        }

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

        // 자식이 끝난 뒤에도 pipe 쓰기 handle을 상속한 손자가 남아 있으면 reader가
        // EOF를 보지 못한다. 이 유예를 넘기면 트리를 종료해 수집을 강제로 마감한다.
        constexpr DWORD reader_drain_grace_milliseconds { 2'000 };
        constexpr DWORD reader_cancel_interval_milliseconds { 100 };
        constexpr DWORD maximum_io_threads { 3 };

        // 콘솔 없는 자식에게 보낼 안전한 graceful signal이 없으므로 즉시 종료한다.
        // job이 있으면 자식이 만든 손자 프로세스까지 함께 정리된다.
        void terminate_execution(const HANDLE job, const HANDLE process) noexcept
        {
            if (job != nullptr && job != INVALID_HANDLE_VALUE)
            {
                TerminateJobObject(job, static_cast<UINT>(ERROR_PROCESS_ABORTED));
                return;
            }
            TerminateProcess(process, static_cast<UINT>(ERROR_PROCESS_ABORTED));
        }

        // I/O 스레드가 유예 안에 끝났으면 true다. 유예를 넘기면 트리를 종료해
        // pipe를 닫고, job이 없어 손자가 살아남는 경우까지 대비해 마지막 수단으로
        // 동기 ReadFile·WriteFile을 직접 취소한다. 어떤 경로로든 join은 반드시 끝난다.
        bool join_io_threads(std::vector<std::thread>& io_threads, const HANDLE job, const HANDLE process) noexcept
        {
            // 출력 reader 둘과 stdin writer 하나다. writer도 같은 경로로 정리해야 자식이
            // 입력을 읽지 않고 멈춘 채 WriteFile에 막힌 경우에 join이 끝난다.
            HANDLE handles[maximum_io_threads] {};
            DWORD count { 0 };
            for (std::thread& io_thread : io_threads)
                if (io_thread.joinable() && count < maximum_io_threads)
                    handles[count++] = io_thread.native_handle();

            bool drained_promptly { true };
            if (count != 0)
            {
                DWORD wait_result { WaitForMultipleObjects(count, handles, TRUE, reader_drain_grace_milliseconds) };
                if (wait_result == WAIT_TIMEOUT)
                {
                    drained_promptly = false;
                    terminate_execution(job, process);
                    wait_result = WaitForMultipleObjects(count, handles, TRUE, reader_drain_grace_milliseconds);
                }
                while (wait_result == WAIT_TIMEOUT)
                {
                    // ReadFile 사이의 좁은 틈에 취소가 빗나갈 수 있으므로 끝날 때까지
                    // 짧은 간격으로 반복한다. 취소된 ReadFile·WriteFile은 실패로 반환되어
                    // 스레드가 스스로 끝난다.
                    for (DWORD index = 0; index < count; ++index)
                        CancelSynchronousIo(handles[index]);
                    wait_result = WaitForMultipleObjects(count, handles, TRUE, reader_cancel_interval_milliseconds);
                }
            }

            for (std::thread& io_thread : io_threads)
                if (io_thread.joinable())
                    io_thread.join();
            return drained_promptly;
        }

        // 자식과 손자를 함께 종료할 수 있도록 kill-on-close job을 만든다. 만들지 못하면
        // 비어 있는 handle을 돌려주고 호출자가 단일 프로세스 종료로 물러선다.
        unique_handle create_kill_on_close_job() noexcept
        {
            unique_handle job { CreateJobObjectW(nullptr, nullptr) };
            if (job.valid() == false)
                return {};

            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == FALSE)
                return {};
            return job;
        }

        DWORD wait_timeout_milliseconds(const std::optional<std::chrono::milliseconds> timeout) noexcept
        {
            if (timeout.has_value() == false)
                return INFINITE;

            const std::int64_t value { timeout->count() };
            // `INFINITE`와 겹치지 않는 최대값으로 제한한다. 실용적인 상한을 넘는 요청은
            // 사실상 무제한과 같다.
            if (value >= static_cast<std::int64_t>(INFINITE))
                return INFINITE - 1;
            return static_cast<DWORD>(value);
        }

        void normalize_separators(std::wstring& path) noexcept
        {
            for (wchar_t& value : path)
                if (value == L'/')
                    value = L'\\';
        }

        int compare_environment_names(const std::wstring_view left, const std::wstring_view right) noexcept
        {
            if (left.empty() || right.empty())
                return left.empty() && right.empty() ? 0 : (left.empty() ? -1 : 1);

            const int result { CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE) };
            // API가 실패하면 대소문자를 구분하는 비교로 대체한다. 순서만 안정적이면 된다.
            if (result == 0)
                return left.compare(right);
            return result - CSTR_EQUAL;
        }

        std::vector<environment_entry> parent_environment_entries()
        {
            std::vector<environment_entry> entries {};
            const environment_strings strings {};
            if (strings.get() == nullptr)
                return entries;

            for (const wchar_t* cursor = strings.get(); *cursor != L'\0';)
            {
                const std::wstring_view text { cursor };
                cursor += text.size() + 1;
                // `=C:` 형태의 drive 현재 디렉터리 항목은 이름이 `=`로 시작하므로 1부터 찾는다.
                const std::size_t separator { text.find(L'=', 1) };
                if (separator == std::wstring_view::npos)
                    continue;
                entries.push_back(environment_entry { std::wstring { text.substr(0, separator) }, std::wstring { text.substr(separator + 1) } });
            }
            return entries;
        }

        environment_block make_environment_block(const std::vector<process_environment_override>& overrides)
        {
            if (overrides.empty())
                return {};

            std::vector<environment_entry> entries { parent_environment_entries() };
            for (const process_environment_override& entry : overrides)
            {
                const auto name { utf8_to_utf16(entry.name) };
                if (name.value.has_value() == false)
                    return { std::nullopt, static_cast<std::uint32_t>(name.error->native_error), true };

                const auto existing { std::ranges::find_if(entries, [&name](const environment_entry& value) { return compare_environment_names(value.name, *name.value) == 0; }) };
                if (entry.value.has_value() == false)
                {
                    if (existing != entries.end())
                        entries.erase(existing);
                    continue;
                }

                const auto value { utf8_to_utf16(*entry.value) };
                if (value.value.has_value() == false)
                    return { std::nullopt, static_cast<std::uint32_t>(value.error->native_error), true };
                if (existing != entries.end())
                    existing->value = *value.value;
                else
                    entries.push_back(environment_entry { *name.value, *value.value });
            }

            // CreateProcess에 넘기는 block은 이름 기준 대소문자 무시 정렬을 요구한다.
            std::ranges::sort(entries, [](const environment_entry& left, const environment_entry& right) { return compare_environment_names(left.name, right.name) < 0; });

            std::wstring block {};
            for (const environment_entry& entry : entries)
            {
                block.append(entry.name);
                block.push_back(L'=');
                block.append(entry.value);
                block.push_back(L'\0');
            }
            // std::wstring이 마지막에 암시적 null을 하나 더 두므로 block은 이중 null로 끝난다.
            return { std::move(block), ERROR_SUCCESS, false };
        }

        unique_handle open_null_device(const DWORD access)
        {
            SECURITY_ATTRIBUTES inheritable {};
            inheritable.nLength = sizeof(inheritable);
            inheritable.lpSecurityDescriptor = nullptr;
            inheritable.bInheritHandle = TRUE;

            SetLastError(ERROR_SUCCESS);
            return unique_handle { CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr) };
        }

        struct output_channel
        {
            unique_handle read {};
            unique_handle write {};
        };

        // 자식에게 넘길 쓰기 end만 상속 가능하게 만든다. 읽기 end가 자식에게 남으면
        // 자식이 살아 있는 동안 부모가 EOF를 볼 수 없다.
        bool create_output_channel(output_channel& channel) noexcept
        {
            SECURITY_ATTRIBUTES inheritable {};
            inheritable.nLength = sizeof(inheritable);
            inheritable.lpSecurityDescriptor = nullptr;
            inheritable.bInheritHandle = TRUE;

            HANDLE read_end { INVALID_HANDLE_VALUE };
            HANDLE write_end { INVALID_HANDLE_VALUE };
            SetLastError(ERROR_SUCCESS);
            if (CreatePipe(&read_end, &write_end, &inheritable, 0) == FALSE)
                return false;

            channel.read.reset(read_end);
            channel.write.reset(write_end);
            return SetHandleInformation(channel.read.get(), HANDLE_FLAG_INHERIT, 0) != FALSE;
        }

        // stdout·stderr와 방향이 반대다. 자식이 읽을 end만 상속 가능하게 만든다. 쓰기
        // end가 자식에게 남으면 부모가 pipe를 닫아도 자식이 EOF를 볼 수 없다.
        bool create_input_channel(output_channel& channel) noexcept
        {
            SECURITY_ATTRIBUTES inheritable {};
            inheritable.nLength = sizeof(inheritable);
            inheritable.lpSecurityDescriptor = nullptr;
            inheritable.bInheritHandle = TRUE;

            HANDLE read_end { INVALID_HANDLE_VALUE };
            HANDLE write_end { INVALID_HANDLE_VALUE };
            SetLastError(ERROR_SUCCESS);
            if (CreatePipe(&read_end, &write_end, &inheritable, 0) == FALSE)
                return false;

            channel.read.reset(read_end);
            channel.write.reset(write_end);
            return SetHandleInformation(channel.write.get(), HANDLE_FLAG_INHERIT, 0) != FALSE;
        }

        // 레코드에 실행 단위 sequence를 부여하고 sink 호출을 직렬화한다. 두 reader
        // 스레드가 동시에 도착해도 한 실행 안의 순서는 고정된다.
        class output_collector
        {
        public:
            explicit output_collector(process_output_sink* const sink) noexcept
                : sink_ { sink }
            {}

            output_collector(const output_collector&) = delete;
            output_collector(output_collector&&) = delete;
            output_collector& operator=(const output_collector&) = delete;
            output_collector& operator=(output_collector&&) = delete;
            ~output_collector() = default;

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
                    // sink 예외가 reader 스레드를 종료시키면 자식이 pipe에서 막힌다.
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
                    // 레코드 경로와 같은 이유로 예외는 삼키고 실패만 표시한다.
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

        // 수집이 실패해도 pipe는 계속 비워 자식이 backpressure로 멈추지 않게 한다. 할당 없는
        // buffer로 남은 byte를 버린다.
        void drain_pipe(const HANDLE pipe) noexcept
        {
            std::byte discard[4096];
            DWORD read_count { 0 };
            while (ReadFile(pipe, discard, static_cast<DWORD>(sizeof(discard)), &read_count, nullptr) != FALSE && read_count != 0)
            {
            }
        }

        void read_output_stream(const HANDLE pipe, process_output_pipeline& pipeline, output_collector& collector, std::atomic<bool>& read_failed) noexcept
        {
            const process_output_pipeline::record_handler handler { [&collector](process_output_record& record) { collector.submit(record); } };
            try
            {
                std::u8string buffer(process_read_block_byte_size, u8'\0');
                while (true)
                {
                    DWORD read_count { 0 };
                    // 자식이 끝나고 쓰기 end가 모두 닫히면 ERROR_BROKEN_PIPE로 반환된다.
                    if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read_count, nullptr) == FALSE)
                        break;
                    if (read_count == 0)
                        break;
                    pipeline.append(std::u8string_view { buffer.data(), static_cast<std::size_t>(read_count) }, handler);
                }
                pipeline.flush(handler);
            }
            catch (...)
            {
                // 남은 byte를 버리고 결과에는 실패만 표시한다.
                read_failed.store(true, std::memory_order_relaxed);
                drain_pipe(pipe);
            }
        }

        // `raw` 스트림의 캡처 통계다. reader 스레드 하나가 쓰고 join 뒤에 읽으므로 잠금이 없다.
        struct raw_stream_state
        {
            std::size_t captured_bytes { 0 };
            bool truncated { false };
        };

        // pipe에서 읽은 byte를 가공 없이 sink로 넘긴다. 상한을 넘은 뒤에도 pipe는 끝까지
        // 비워 자식이 backpressure로 멈추지 않게 한다.
        void read_raw_stream(
            const HANDLE pipe, const process_stream stream, const std::size_t maximum_captured_bytes, raw_stream_state& state, output_collector& collector, std::atomic<bool>& read_failed) noexcept
        {
            try
            {
                std::string buffer(process_read_block_byte_size, '\0');
                while (true)
                {
                    DWORD read_count { 0 };
                    if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read_count, nullptr) == FALSE)
                        break;
                    if (read_count == 0)
                        break;

                    const std::size_t received { static_cast<std::size_t>(read_count) };
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
                drain_pipe(pipe);
            }
        }

        // 표준 입력을 쓰고 pipe를 닫아 자식이 EOF를 보게 한다. 자식이 입력을 다 읽기 전에
        // 끝나거나 취소로 죽으면 WriteFile이 실패하는데, 이는 정상 경로이므로 오류로
        // 세지 않는다.
        void write_standard_input(unique_handle& pipe, const std::string& data, std::atomic<std::uint32_t>& unexpected_error) noexcept
        {
            std::size_t offset { 0 };
            while (offset < data.size())
            {
                const DWORD length { static_cast<DWORD>(std::min(data.size() - offset, process_read_block_byte_size)) };
                DWORD written { 0 };
                if (WriteFile(pipe.get(), data.data() + offset, length, &written, nullptr) == FALSE)
                {
                    const DWORD error { GetLastError() };
                    if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA && error != ERROR_OPERATION_ABORTED)
                        unexpected_error.store(static_cast<std::uint32_t>(error), std::memory_order_relaxed);
                    break;
                }
                offset += static_cast<std::size_t>(written);
            }
            pipe.reset();
        }

        void finish_result(process_result& result, const std::chrono::steady_clock::time_point started)
        {
            result.finished_at = std::chrono::system_clock::now();
            result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        }

        process_result make_unexpected_failure_result() noexcept
        {
            process_result result {};
            result.completion = process_completion::start_failed;
            result.native_error = ERROR_NOT_ENOUGH_MEMORY;
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

        process_result make_reader_failure_result(process_result result, const std::chrono::steady_clock::time_point started)
        {
            result.completion = process_completion::internal_error;
            result.native_error = ERROR_NOT_ENOUGH_MEMORY;
            result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.io_thread_failed"), result.native_error));
            finish_result(result, started);
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

            // 이미 취소된 요청은 프로세스를 만들지 않는다.
            if (token.cancelled())
            {
                result.completion = process_completion::cancelled;
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_cancelled, tr(u8"process.cancelled_before_start")));
                return result;
            }

            // 실제 실행에 쓰는 명령줄과 기록용 값을 분리한다. 마스킹은 기록용 값에만
            // 적용하므로 자식에게 전달하는 인자는 바뀌지 않는다.
            const std::u8string command_line_text { build_windows_command_line(request.executable, request.arguments) };
            result.masked_command_line = mask_secrets(command_line_text);

            const auto executable { utf8_to_utf16(request.executable) };
            const auto command_line { utf8_to_utf16(command_line_text) };
            const auto working_directory { utf8_to_utf16(request.working_directory) };
            if (executable.value.has_value() == false || command_line.value.has_value() == false || working_directory.value.has_value() == false)
            {
                result.completion = process_completion::start_failed;
                result.native_error = ERROR_NO_UNICODE_TRANSLATION;
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.request_conversion_failed"), result.native_error));
                return result;
            }

            const environment_block environment { make_environment_block(request.environment_overrides) };
            if (environment.failed)
            {
                result.completion = process_completion::start_failed;
                result.native_error = environment.native_error;
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.environment_conversion_failed"), result.native_error));
                return result;
            }

            // 입력 내용이 없으면 자식이 프롬프트를 읽어도 즉시 EOF가 되도록 stdin은 NUL이다.
            // 내용이 있으면 pipe를 만들고 writer 스레드가 채운 뒤 닫는다.
            const bool has_standard_input { request.standard_input.empty() == false };
            unique_handle null_input {};
            output_channel standard_input {};
            bool input_ready { false };
            if (has_standard_input)
                input_ready = create_input_channel(standard_input);
            else
            {
                null_input = open_null_device(GENERIC_READ);
                input_ready = null_input.valid();
            }
            const HANDLE child_input { has_standard_input ? standard_input.read.get() : null_input.get() };
            output_channel standard_output {};
            output_channel standard_error {};
            if (input_ready == false || create_output_channel(standard_output) == false || create_output_channel(standard_error) == false)
            {
                result.completion = process_completion::start_failed;
                result.native_error = last_error_or(ERROR_INVALID_HANDLE);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.standard_handles_failed"), result.native_error));
                return result;
            }

            unique_attribute_list attributes {};
            HANDLE inherited[] { child_input, standard_output.write.get(), standard_error.write.get() };
            STARTUPINFOEXW startup {};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = child_input;
            startup.StartupInfo.hStdOutput = standard_output.write.get();
            startup.StartupInfo.hStdError = standard_error.write.get();

            SetLastError(ERROR_SUCCESS);
            if (attributes.initialize(1) == false || UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) == FALSE)
            {
                result.completion = process_completion::start_failed;
                result.native_error = last_error_or(ERROR_INVALID_PARAMETER);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.inherited_handles_failed"), result.native_error));
                return result;
            }
            startup.lpAttributeList = attributes.get();

            std::wstring application_path { *executable.value };
            std::wstring current_directory { *working_directory.value };
            normalize_separators(application_path);
            normalize_separators(current_directory);
            // `CreateProcessW`는 명령줄 buffer를 수정할 수 있으므로 쓰기 가능한 사본을 넘긴다.
            std::wstring mutable_command_line { *command_line.value };

            void* environment_pointer { nullptr };
            if (environment.block.has_value())
                environment_pointer = const_cast<wchar_t*>(environment.block->c_str());

            // 취소 event는 registration보다 먼저 선언해 registration이 먼저 해제되게 한다.
            // 그래야 콜백이 이미 닫힌 event를 신호하는 일이 없다.
            unique_handle cancel_event { CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            if (cancel_event.valid() == false)
            {
                result.completion = process_completion::start_failed;
                result.native_error = last_error_or(ERROR_INVALID_HANDLE);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.cancel_event_failed"), result.native_error));
                return result;
            }

            // 자식과 손자를 함께 정리하기 위한 job이다. 만들지 못해도 실행은 계속하되
            // 종료 범위가 자식 하나로 줄어든다는 경고를 남긴다.
            unique_handle job { create_kill_on_close_job() };
            const bool job_available { job.valid() };

            PROCESS_INFORMATION created {};
            const std::chrono::steady_clock::time_point started { std::chrono::steady_clock::now() };
            result.started_at = std::chrono::system_clock::now();
            SetLastError(ERROR_SUCCESS);
            // 자식이 job에 들어가기 전에 손자를 만들지 못하도록 정지 상태로 시작한다.
            const BOOL launched {
                CreateProcessW(application_path.c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment_pointer, current_directory.c_str(), &startup.StartupInfo, &created),
            };
            if (launched == FALSE)
            {
                result.completion = process_completion::start_failed;
                result.native_error = last_error_or(ERROR_FILE_NOT_FOUND);
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.start_failed"), result.native_error));
                finish_result(result, started);
                return result;
            }

            const unique_handle process_handle { created.hProcess };
            const unique_handle thread_handle { created.hThread };

            bool job_assigned { false };
            if (job_available)
            {
                SetLastError(ERROR_SUCCESS);
                job_assigned = AssignProcessToJobObject(job.get(), process_handle.get()) != FALSE;
                if (job_assigned == false)
                    job.reset();
            }
            if (job_assigned == false)
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.job_object_unavailable"), std::nullopt, diagnostic_severity::warning));

            SetLastError(ERROR_SUCCESS);
            if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1))
            {
                const std::uint32_t resume_error { last_error_or(ERROR_INVALID_HANDLE) };
                terminate_execution(job.get(), process_handle.get());
                result.completion = process_completion::start_failed;
                result.native_error = resume_error;
                result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_start_failed, tr(u8"process.resume_failed"), result.native_error));
                finish_result(result, started);
                return result;
            }
            // 자식만 사용해야 하는 handle 사본을 닫는다. 이 시점에 닫지 않으면 자식이
            // 끝나도 부모의 쓰기 end가 남아 reader가 EOF를 관측할 수 없다.
            null_input.reset();
            standard_input.read.reset();
            standard_output.write.reset();
            standard_error.write.reset();

            // transcoder는 fallback을 요청한 실행에서만 만든다. 상태가 없으므로 두
            // reader 스레드가 같은 instance를 함께 사용해도 안전하다.
            std::unique_ptr<text_transcoder> transcoder {};
            if (request.text_encoding == process_text_encoding::active_code_page_fallback)
                transcoder = make_active_code_page_transcoder();

            // `lines` 스트림은 pipeline을 거치고 `raw` 스트림은 raw_stream_state만 쓴다.
            // 쓰지 않는 쪽은 비어 있는 채로 남아 합산에 영향을 주지 않는다.
            process_output_pipeline output_pipeline {
                process_stream::standard_output,
                request.maximum_record_bytes,
                request.maximum_captured_bytes_per_stream,
                request.text_encoding,
                transcoder.get(),
            };
            process_output_pipeline error_pipeline {
                process_stream::standard_error,
                request.maximum_record_bytes,
                request.maximum_captured_bytes_per_stream,
                request.text_encoding,
                transcoder.get(),
            };
            raw_stream_state raw_output {};
            raw_stream_state raw_error {};
            output_collector collector { sink };
            std::atomic<bool> read_failed { false };
            std::atomic<std::uint32_t> input_error { ERROR_SUCCESS };

            const auto start_reader = [&](std::vector<std::thread>& threads, output_channel& channel, const process_stream stream, const process_output_mode mode, process_output_pipeline& pipeline,
                                          raw_stream_state& raw) {
                const HANDLE pipe { channel.read.get() };
                if (mode == process_output_mode::raw)
                    threads.emplace_back(
                        [pipe, stream, &request, &raw, &collector, &read_failed]() { read_raw_stream(pipe, stream, request.maximum_captured_bytes_per_stream, raw, collector, read_failed); });
                else
                    threads.emplace_back([pipe, &pipeline, &collector, &read_failed]() { read_output_stream(pipe, pipeline, collector, read_failed); });
            };

            // pipe마다 전용 I/O 스레드를 둔다. 한 스레드로 두 pipe를 읽으면 읽지 않는
            // pipe의 buffer가 가득 차서 자식이 멈출 수 있고, 입력을 쓰는 동안 출력을 읽지
            // 않으면 자식이 출력 pipe에서 막혀 서로 기다리게 된다.
            std::vector<std::thread> io_threads {};
            try
            {
                io_threads.reserve(maximum_io_threads);
                start_reader(io_threads, standard_output, process_stream::standard_output, request.standard_output_mode, output_pipeline, raw_output);
                start_reader(io_threads, standard_error, process_stream::standard_error, request.standard_error_mode, error_pipeline, raw_error);
                if (has_standard_input)
                    io_threads.emplace_back([&standard_input, &request, &input_error]() { write_standard_input(standard_input.write, request.standard_input, input_error); });
            }
            catch (...)
            {
                // I/O 스레드가 없으면 자식이 pipe에서 막힐 수 있으므로 즉시 정리한다.
                terminate_execution(job.get(), process_handle.get());
                join_io_threads(io_threads, job.get(), process_handle.get());
                return make_reader_failure_result(std::move(result), started);
            }

            // reader가 joinable한 동안 예외로 unwinding하면 std::terminate가 일어난다.
            // 이 아래 구간의 어떤 예외도 트리 종료와 join을 거친 뒤 결과로 변환한다.
            try
            {
                // 취소 콜백은 event 하나만 신호한다. registration은 이 scope에서 해제되므로
                // 콜백이 실행 중이면 해제가 끝날 때까지 기다린다.
                const HANDLE signalled_event { cancel_event.get() };
                const process_cancellation_registration registration { token.register_callback([signalled_event]() { SetEvent(signalled_event); }) };

                const HANDLE wait_handles[] { process_handle.get(), cancel_event.get() };
                SetLastError(ERROR_SUCCESS);
                const DWORD wait_result { WaitForMultipleObjects(static_cast<DWORD>(std::size(wait_handles)), wait_handles, FALSE, wait_timeout_milliseconds(request.timeout)) };
                const std::uint32_t wait_error { wait_result == WAIT_FAILED ? last_error_or(ERROR_INVALID_HANDLE) : std::uint32_t { ERROR_SUCCESS } };

                // 자식이 스스로 끝난 경우가 아니면 트리를 정리한다. 정리하면 pipe가 닫혀
                // reader 스레드가 EOF를 보고 끝난다.
                if (wait_result != WAIT_OBJECT_0)
                    terminate_execution(job.get(), process_handle.get());

                const bool drained_promptly { join_io_threads(io_threads, job.get(), process_handle.get()) };

                result.captured_bytes = output_pipeline.captured_bytes() + error_pipeline.captured_bytes() + raw_output.captured_bytes + raw_error.captured_bytes;
                result.output_truncated = output_pipeline.truncated() || error_pipeline.truncated() || raw_output.truncated || raw_error.truncated;
                result.record_count = collector.count();
                if (result.output_truncated)
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_output_truncated, tr(u8"process.output_truncated"), std::nullopt, diagnostic_severity::warning));
                if (collector.sink_failed())
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.output_sink_threw")));
                if (drained_promptly == false)
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.inherited_pipe_lingered"), std::nullopt, diagnostic_severity::warning));
                if (input_error.load(std::memory_order_relaxed) != ERROR_SUCCESS)
                {
                    result.diagnostics.push_back(
                        make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.stdin_write_failed"), input_error.load(std::memory_order_relaxed), diagnostic_severity::warning));
                }
                if (read_failed.load(std::memory_order_relaxed))
                {
                    result.output_truncated = true;
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_pipe_failed, tr(u8"process.output_thread_failed")));
                }

                if (wait_result == WAIT_TIMEOUT)
                {
                    // 강제 종료한 자식의 종료 코드는 의미가 없으므로 채우지 않는다.
                    result.completion = process_completion::timed_out;
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_timed_out, tr(u8"process.timed_out")));
                    finish_result(result, started);
                    return result;
                }
                if (wait_result == WAIT_OBJECT_0 + 1)
                {
                    result.completion = process_completion::cancelled;
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::process_cancelled, tr(u8"process.cancelled")));
                    finish_result(result, started);
                    return result;
                }
                if (wait_result != WAIT_OBJECT_0)
                {
                    result.completion = process_completion::internal_error;
                    result.native_error = wait_error;
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.wait_failed"), result.native_error));
                    finish_result(result, started);
                    return result;
                }

                DWORD exit_code { 0 };
                SetLastError(ERROR_SUCCESS);
                if (GetExitCodeProcess(process_handle.get(), &exit_code) == FALSE)
                {
                    result.completion = process_completion::internal_error;
                    result.native_error = last_error_or(ERROR_INVALID_HANDLE);
                    result.diagnostics.push_back(make_process_diagnostic(diagnostic_code::operation_failed, tr(u8"process.exit_code_unreadable"), result.native_error));
                    finish_result(result, started);
                    return result;
                }

                result.completion = process_completion::exited;
                // Windows 종료 코드는 `0xC0000005`처럼 상위 bit를 쓰므로 bit 값을 보존한다.
                result.exit_code = static_cast<std::int32_t>(exit_code);
                finish_result(result, started);
                return result;
            }
            catch (...)
            {
                terminate_execution(job.get(), process_handle.get());
                join_io_threads(io_threads, job.get(), process_handle.get());
                result.completion = process_completion::internal_error;
                result.native_error = ERROR_NOT_ENOUGH_MEMORY;
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

        class win32_process_runner final : public process_runner
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
        return std::make_unique<win32_process_runner>();
    }
} // namespace cyella::win32
