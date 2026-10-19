#include "platform/win32/win32_repository_watcher.h"

#include "domain/path_syntax.h"
#include "domain/repository_path.h"
#include "platform/win32/utf8.h"
#include "platform/win32/win32_file_io.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cyella::win32 {
    namespace {
        // 네트워크 드라이브에서도 되는 상한이다 (64 KiB를 넘으면 ERROR_INVALID_PARAMETER).
        constexpr std::size_t notification_buffer_size { 64u * 1024u };
        constexpr DWORD notification_filter { FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION };

        diagnostic make_diagnostic(const diagnostic_severity severity, std::u8string message, const std::optional<std::uint32_t> native_error = std::nullopt)
        {
            diagnostic value {};
            value.code = diagnostic_code::repository_watch_failed;
            value.severity = severity;
            value.message = std::move(message);
            value.native_error = native_error;
            return value;
        }

        struct root_spec
        {
            watched_root_kind kind { watched_root_kind::working_tree };
            std::u8string path {};
        };

        // 감시 뿌리 하나다. 버퍼 주소가 겹치기 I/O 동안 고정이어야 하므로 힙에 둔다.
        struct watched_root
        {
            watched_root_kind kind { watched_root_kind::working_tree };
            std::u8string path {};
            unique_handle directory {};
            unique_handle event {};
            OVERLAPPED overlapped {};
            // FILE_NOTIFY_INFORMATION은 DWORD 정렬을 요구한다.
            alignas(DWORD) std::array<std::byte, notification_buffer_size> buffer {};
            // 읽기가 걸려 있는가. 걸려 있는 동안은 버퍼와 OVERLAPPED를 건드리면 안 된다.
            bool pending { false };

            // 읽기를 건다. 실패하면 false다.
            bool issue_read() noexcept
            {
                ResetEvent(event.get());
                overlapped = {};
                overlapped.hEvent = event.get();
                DWORD ignored { 0 };
                if (ReadDirectoryChangesW(directory.get(), buffer.data(), static_cast<DWORD>(buffer.size()), TRUE, notification_filter, &ignored, &overlapped, nullptr) == FALSE)
                    return false;
                pending = true;
                return true;
            }

            // 걸려 있는 읽기를 취소하고 끝날 때까지 기다린다. 이 뒤에야 버퍼를 풀어도 안전하다.
            void cancel_read() noexcept
            {
                if (pending == false)
                    return;
                CancelIoEx(directory.get(), &overlapped);
                DWORD ignored { 0 };
                GetOverlappedResult(directory.get(), &overlapped, &ignored, TRUE);
                pending = false;
            }
        };

        std::optional<std::u8string> read_small_text_file(const std::u8string& path)
        {
            const settings_file_read_result file { read_whole_file(path) };
            if (file.state != settings_file_read_state::available || file.bytes.size() > 4096)
                return std::nullopt;
            return file.bytes;
        }

        // 감시할 뿌리를 정한다. `.git`이 파일이면 gitdir과 commondir도 뿌리다.
        std::vector<root_spec> resolve_roots(const std::u8string& working_tree, std::vector<diagnostic>& diagnostics)
        {
            std::vector<root_spec> roots {};
            roots.push_back({ watched_root_kind::working_tree, working_tree });

            std::u8string dot_git { working_tree };
            dot_git.append(u8"\\.git");
            const prepared_path prepared { prepare_file_path(dot_git) };
            if (prepared.value.has_value() == false)
                return roots;
            const DWORD attributes { GetFileAttributesW(prepared.value->c_str()) };
            // 디렉터리면 작업 트리 재귀 감시가 이미 덮는다. 없으면 저장소가 아직 아니다.
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return roots;

            const std::optional<std::u8string> content { read_small_text_file(dot_git) };
            const std::optional<std::u8string> git_directory { content.has_value() ? resolve_gitdir_pointer(*content, working_tree) : std::nullopt };
            if (git_directory.has_value() == false)
            {
                diagnostics.push_back(make_diagnostic(diagnostic_severity::warning, u8".git 파일에서 gitdir을 읽지 못해 작업 트리만 감시합니다."));
                return roots;
            }
            roots.push_back({ watched_root_kind::git_directory, *git_directory });

            std::u8string commondir_file { *git_directory };
            commondir_file.append(u8"\\commondir");
            if (const std::optional<std::u8string> commondir_content { read_small_text_file(commondir_file) }; commondir_content.has_value())
                if (const std::optional<std::u8string> common { resolve_commondir_pointer(*commondir_content, *git_directory) }; common.has_value())
                    roots.push_back({ watched_root_kind::common_directory, *common });
            return roots;
        }

        class repository_watch_session final : public repository_watcher
        {
        public:
            repository_watch_session(repository_change_callback callback, const repository_watch_options& options)
                : callback_ { std::move(callback) }
                , debouncer_ { options.debounce }
            {}

            ~repository_watch_session() override
            {
                stop();
                // 스레드를 시작하기 전에 실패한 세션은 걸어 둔 읽기를 거둘 스레드가 없다.
                // 버퍼를 풀기 전에 여기서 거둔다. 스레드가 돌았다면 이미 거둔 뒤라 할 일이 없다.
                if (thread_.joinable() == false)
                    cancel_all_reads();
            }

            // 디렉터리를 열고 첫 읽기를 건 뒤 스레드를 시작한다.
            bool start(const std::vector<root_spec>& specs, std::vector<diagnostic>& diagnostics)
            {
                stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (stop_event_.valid() == false)
                {
                    diagnostics.push_back(make_diagnostic(diagnostic_severity::error, u8"감시 종료 이벤트를 만들지 못했습니다.", static_cast<std::uint32_t>(GetLastError())));
                    return false;
                }

                for (const root_spec& spec : specs)
                {
                    std::unique_ptr<watched_root> root { open_root(spec, diagnostics) };
                    if (root != nullptr)
                        roots_.push_back(std::move(root));
                    // 작업 트리를 열지 못하면 감시할 이유가 없다. 나머지는 열리는 만큼만 쓴다.
                    else if (spec.kind == watched_root_kind::working_tree)
                        return false;
                }

                try
                {
                    thread_ = std::thread { [this] { run(); } };
                }
                catch (...)
                {
                    cancel_all_reads();
                    diagnostics.push_back(make_diagnostic(diagnostic_severity::error, u8"감시 스레드를 시작하지 못했습니다."));
                    return false;
                }
                return true;
            }

            void stop() noexcept override
            {
                stop_requested_.store(true);
                if (stop_event_.valid())
                    SetEvent(stop_event_.get());

                // 동시에 여러 스레드가 불러도 한 스레드만 join한다. 나머지는 끝나기를 기다린다.
                const std::lock_guard lock { join_mutex_ };
                // 콜백 안에서 불렸다면 자기 자신을 기다릴 수 없다. 표시만 해 두면 콜백이 끝난 뒤
                // 스레드가 스스로 빠져나간다.
                if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
                    thread_.join();
            }

        private:
            std::unique_ptr<watched_root> open_root(const root_spec& spec, std::vector<diagnostic>& diagnostics)
            {
                const prepared_path prepared { prepare_file_path(spec.path) };
                if (prepared.value.has_value() == false)
                {
                    diagnostics.push_back(make_diagnostic(spec.kind == watched_root_kind::working_tree ? diagnostic_severity::error : diagnostic_severity::warning, u8"감시할 경로가 올바르지 않습니다: " + spec.path, prepared.native_error));
                    return nullptr;
                }

                auto root { std::make_unique<watched_root>() };
                root->kind = spec.kind;
                root->path = spec.path;
                SetLastError(ERROR_SUCCESS);
                root->directory.reset(CreateFileW(prepared.value->c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
                if (root->directory.valid() == false)
                {
                    diagnostics.push_back(make_diagnostic(spec.kind == watched_root_kind::working_tree ? diagnostic_severity::error : diagnostic_severity::warning, u8"감시할 폴더를 열지 못했습니다: " + spec.path, static_cast<std::uint32_t>(GetLastError())));
                    return nullptr;
                }

                root->event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (root->event.valid() == false || root->issue_read() == false)
                {
                    diagnostics.push_back(make_diagnostic(spec.kind == watched_root_kind::working_tree ? diagnostic_severity::error : diagnostic_severity::warning, u8"폴더 변경 감시를 시작하지 못했습니다: " + spec.path, static_cast<std::uint32_t>(GetLastError())));
                    return nullptr;
                }
                return root;
            }

            void cancel_all_reads() noexcept
            {
                for (const std::unique_ptr<watched_root>& root : roots_)
                    root->cancel_read();
            }

            void run() noexcept
            {
                try
                {
                    loop();
                }
                catch (...)
                {}
                // 걸린 읽기를 거두기 전에는 버퍼를 풀 수 없다.
                cancel_all_reads();
            }

            void loop()
            {
                while (stop_requested_.load() == false)
                {
                    std::array<HANDLE, 8> handles {};
                    std::array<watched_root*, 8> owners {};
                    DWORD count { 0 };
                    handles[count++] = stop_event_.get();
                    for (const std::unique_ptr<watched_root>& root : roots_)
                        if (root->pending && count < handles.size())
                        {
                            owners[count] = root.get();
                            handles[count++] = root->event.get();
                        }

                    DWORD timeout { INFINITE };
                    if (const auto due { debouncer_.due_time() }; due.has_value())
                    {
                        const auto remaining { *due - change_debouncer::clock::now() };
                        // 올림해서 깬 시각에는 방출 시각이 지나 있게 한다.
                        timeout = remaining.count() <= 0 ? 0 : static_cast<DWORD>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
                    }

                    const DWORD waited { WaitForMultipleObjects(count, handles.data(), FALSE, timeout) };
                    if (waited == WAIT_OBJECT_0)
                        return;
                    if (waited > WAIT_OBJECT_0 && waited < WAIT_OBJECT_0 + count)
                        handle_completion(*owners[waited - WAIT_OBJECT_0]);
                    else if (waited != WAIT_TIMEOUT)
                    {
                        // 기다리기 자체가 실패했다. 이벤트를 놓칠 수 있으므로 한 번 알리고 그만둔다.
                        debouncer_.note_overflow(change_debouncer::clock::now());
                        flush();
                        return;
                    }

                    if (stop_requested_.load())
                        return;
                    flush();
                }
            }

            void handle_completion(watched_root& root)
            {
                const auto now { change_debouncer::clock::now() };
                DWORD bytes { 0 };
                const BOOL succeeded { GetOverlappedResult(root.directory.get(), &root.overlapped, &bytes, FALSE) };
                root.pending = false;

                if (succeeded == FALSE)
                {
                    // 버퍼가 넘쳤다(ERROR_NOTIFY_ENUM_DIR). 그 밖의 오류(폴더가 지워짐 등)는 읽기를 다시
                    // 걸어도 실패하므로 아래 재시도에서 감시를 포기한다. 어느 쪽이든 놓친 변경이 있다.
                    debouncer_.note_overflow(now);
                    static_cast<void>(root.issue_read());
                    return;
                }
                if (bytes == 0)
                {
                    debouncer_.note_overflow(now);
                    static_cast<void>(root.issue_read());
                    return;
                }

                const std::byte* cursor { root.buffer.data() };
                while (true)
                {
                    const auto* info { reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(cursor) };
                    const std::wstring_view name { info->FileName, info->FileNameLength / sizeof(wchar_t) };
                    const auto converted { utf16_to_utf8(name) };
                    // 변환하지 못한 이름은 모르는 채로 두는 것보다 한 번 더 읽는 쪽이 안전하다.
                    const change_classification classification { converted.value.has_value() ? classify_repository_change(root.kind, *converted.value) : change_classification::working_tree };
                    debouncer_.note(classification, now);

                    if (info->NextEntryOffset == 0)
                        break;
                    cursor += info->NextEntryOffset;
                }

                // 버퍼를 다 읽었으니 다음 변경을 받을 읽기를 다시 건다. 실패하면 이 뿌리는 더는 감시되지
                // 않으므로 마지막으로 모두 알려 소비자가 다시 읽게 한다.
                if (root.issue_read() == false)
                    debouncer_.note_overflow(now);
            }

            void flush()
            {
                const pending_changes ready { debouncer_.take_ready(change_debouncer::clock::now()) };
                if (ready.repository_state)
                    notify(repository_change_kind::repository_state_changed);
                if (ready.working_tree)
                    notify(repository_change_kind::working_tree_changed);
            }

            void notify(const repository_change_kind kind) noexcept
            {
                // stop() 이후에는 콜백을 부르지 않는다. 소비자가 소멸 중일 수 있다.
                if (stop_requested_.load() || callback_ == nullptr)
                    return;
                try
                {
                    callback_(kind);
                }
                catch (...)
                {}
            }

            repository_change_callback callback_ {};
            change_debouncer debouncer_;
            std::vector<std::unique_ptr<watched_root>> roots_ {};
            unique_handle stop_event_ {};
            std::atomic_bool stop_requested_ { false };
            std::mutex join_mutex_ {};
            std::thread thread_ {};
        };

        class win32_repository_watcher_factory final : public repository_watcher_factory
        {
        public:
            [[nodiscard]] repository_watch_start_result start(const std::u8string_view working_tree, repository_change_callback callback, const repository_watch_options& options) override
            {
                return start_repository_watcher(working_tree, std::move(callback), options);
            }
        };
    } // namespace

    repository_watch_start_result start_repository_watcher(const std::u8string_view working_tree, repository_change_callback callback, const repository_watch_options& options)
    {
        repository_watch_start_result result {};
        try
        {
            if (is_absolute_windows_path(working_tree) == false)
            {
                result.diagnostics.push_back(make_diagnostic(diagnostic_severity::error, u8"감시할 작업 트리는 절대 경로여야 합니다."));
                return result;
            }

            const std::u8string normalized { normalize_repository_path(working_tree) };
            const std::vector<root_spec> specs { resolve_roots(normalized, result.diagnostics) };
            auto session { std::make_unique<repository_watch_session>(std::move(callback), options) };
            if (session->start(specs, result.diagnostics) == false)
                return result;
            result.watcher = std::move(session);
            return result;
        }
        catch (...)
        {
            result.watcher.reset();
            try
            {
                result.diagnostics.push_back(make_diagnostic(diagnostic_severity::error, u8"저장소 감시를 시작하는 중 예기치 않은 오류가 발생했습니다."));
            }
            catch (...)
            {}
            return result;
        }
    }

    std::unique_ptr<repository_watcher_factory> make_repository_watcher_factory()
    {
        return std::make_unique<win32_repository_watcher_factory>();
    }
} // namespace cyella::win32
