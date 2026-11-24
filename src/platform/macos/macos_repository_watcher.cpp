#include "platform/win32/win32_repository_watcher.h"

#include "domain/path_syntax.h"
#include "domain/repository_path.h"
#include "domain/text_catalog.h"
#include "platform/macos/posix_file.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// `platform/win32/win32_repository_watcher.h`의 macOS 구현이다 (FSEvents).
//
//  - 감시 하나에 FSEvents stream 하나다. 뿌리(작업 트리, `.git`이 파일이면 gitdir과 commondir)를 모두 한 stream에 건다. 파일 단위
//    사건(`kFSEventStreamCreateFlagFileEvents`)이라 바뀐 파일의 경로가 온다. stream은 이 감시만의 직렬 dispatch queue에서 돈다.
//  - 사건은 감시 thread 하나가 받아 `classify_repository_change`로 가르고 `change_debouncer`로 모은다 (Win32 판과 같은 thread 모델이다).
//  - FSEvents가 사건을 놓쳤다고 하면(`MustScanSubDirs`·`UserDropped`·`KernelDropped`) 두 종류를 모두 알린다. 뿌리 폴더가 지워지거나
//    옮겨지면(`RootChanged`) 그 뿌리마다 한 번 `watch_lost`를 곧바로 알린다.
//  - FSEvents가 주는 경로는 심볼릭 링크를 푼 실제 경로다 (`/var` → `/private/var`). 뿌리도 `realpath`로 풀어 견준다.
namespace cyella::win32 {
    namespace {
        // FSEvents가 사건을 모으는 시간이다. 디바운스가 따로 있으므로 짧게 둔다.
        constexpr CFTimeInterval stream_latency_seconds { 0.05 };
        // 시작 전의 변경이 늦게 배달될 수 있는 시간이다. 이 동안만 사건의 파일 시각을 견준다.
        constexpr long stale_window_seconds { 5 };

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

        // 견줄 때 쓰는 뿌리다. 실제 경로와 끝의 `/`를 붙인 꼴을 함께 둔다.
        struct watched_root
        {
            watched_root_kind kind { watched_root_kind::working_tree };
            std::string real_path {};
            bool lost { false };
        };

        // 감시할 폴더의 실제 경로다 (심볼릭 링크를 푼다). 폴더가 아니거나 풀지 못하면 값이 없고 `error`에 errno가 남는다.
        std::optional<std::string> real_directory_of(const std::u8string& path, int& error)
        {
            const std::string text { macos::to_narrow(path) };
            struct stat info {};
            if (::stat(text.c_str(), &info) != 0)
            {
                error = errno;
                return std::nullopt;
            }
            if (S_ISDIR(info.st_mode) == false)
            {
                error = ENOTDIR;
                return std::nullopt;
            }
            char resolved[PATH_MAX] {};
            if (::realpath(text.c_str(), resolved) == nullptr)
            {
                error = errno;
                return std::nullopt;
            }
            return std::string { resolved };
        }

        std::optional<std::u8string> read_small_text_file(const std::u8string& path)
        {
            const macos::whole_file_read read { macos::read_whole_file(path, 4096) };
            if (read.bytes.has_value() == false)
                return std::nullopt;
            return macos::to_u8(*read.bytes);
        }

        // 감시할 뿌리를 정한다. `.git`이 파일이면 gitdir과 commondir도 뿌리다 (Win32 판과 같다).
        std::vector<root_spec> resolve_roots(const std::u8string& working_tree, std::vector<diagnostic>& diagnostics)
        {
            std::vector<root_spec> roots {};
            roots.push_back({ watched_root_kind::working_tree, working_tree });

            const std::u8string dot_git { join_native_path(working_tree, u8".git") };
            struct stat info {};
            const std::string dot_git_text { macos::to_narrow(dot_git) };
            // 디렉터리면 작업 트리 재귀 감시가 이미 덮는다. 없으면 저장소가 아직 아니다.
            if (::stat(dot_git_text.c_str(), &info) != 0 || S_ISDIR(info.st_mode))
                return roots;

            const std::optional<std::u8string> content { read_small_text_file(dot_git) };
            const std::optional<std::u8string> git_directory { content.has_value() ? resolve_gitdir_pointer(*content, working_tree) : std::nullopt };
            if (git_directory.has_value() == false)
            {
                diagnostics.push_back(make_diagnostic(diagnostic_severity::warning, tr(u8"watch.gitdir_unreadable")));
                return roots;
            }
            roots.push_back({ watched_root_kind::git_directory, *git_directory });

            const std::u8string commondir_file { join_native_path(*git_directory, u8"commondir") };
            if (const std::optional<std::u8string> commondir_content { read_small_text_file(commondir_file) }; commondir_content.has_value())
                if (const std::optional<std::u8string> common { resolve_commondir_pointer(*commondir_content, *git_directory) }; common.has_value())
                    roots.push_back({ watched_root_kind::common_directory, *common });
            return roots;
        }

        // FSEvents 사건 하나를 감시 thread에 넘기는 꼴이다.
        struct raw_event
        {
            std::string path {};
            FSEventStreamEventFlags flags { 0 };
        };

        // dispatch queue의 콜백과 감시 thread가 함께 쓰는 상자다. stream의 context가 이 상자를 가리킨다.
        struct event_mailbox
        {
            std::mutex mutex {};
            std::condition_variable changed {};
            std::deque<raw_event> events {};
            bool stopping { false };
            // 시작하는 동안 받는 사건을 버린다. 시작 전에 일어난 변경이 FSEvents를 거쳐 늦게 오기 때문이다 (`start`).
            bool discarding { true };
        };

        void on_stream_events(ConstFSEventStreamRef, void* const info, const std::size_t count, void* const paths, const FSEventStreamEventFlags flags[], const FSEventStreamEventId[])
        {
            auto* const mailbox { static_cast<event_mailbox*>(info) };
            const auto* const names { static_cast<const char* const*>(paths) };
            try
            {
                const std::lock_guard<std::mutex> lock { mailbox->mutex };
                if (mailbox->stopping || mailbox->discarding)
                    return;
                for (std::size_t index { 0 }; index < count; ++index)
                    mailbox->events.push_back(raw_event { names[index] != nullptr ? std::string { names[index] } : std::string {}, flags[index] });
            }
            catch (...)
            {
                // 담지 못한 사건은 놓친 것으로 알린다.
                const std::lock_guard<std::mutex> lock { mailbox->mutex };
                mailbox->events.clear();
                mailbox->events.push_back(raw_event { {}, kFSEventStreamEventFlagMustScanSubDirs });
            }
            mailbox->changed.notify_all();
        }

        void drain_queue(void*) noexcept
        {}

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
                close_stream();
            }

            bool start(const std::vector<root_spec>& specs, std::vector<diagnostic>& diagnostics)
            {
                CFMutableArrayRef paths { CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks) };
                if (paths == nullptr)
                {
                    diagnostics.push_back(make_diagnostic(diagnostic_severity::error, tr(u8"watch.unexpected_failure")));
                    return false;
                }
                for (const root_spec& spec : specs)
                {
                    int error { 0 };
                    const std::optional<std::string> real { real_directory_of(spec.path, error) };
                    if (real.has_value() == false)
                    {
                        diagnostics.push_back(make_diagnostic(spec.kind == watched_root_kind::working_tree ? diagnostic_severity::error : diagnostic_severity::warning,
                            tr(u8"watch.folder_open_failed", { { u8"path", spec.path } }), static_cast<std::uint32_t>(error)));
                        // 작업 트리를 열지 못하면 감시할 이유가 없다. 나머지는 열리는 만큼만 쓴다.
                        if (spec.kind == watched_root_kind::working_tree)
                        {
                            CFRelease(paths);
                            return false;
                        }
                        continue;
                    }
                    const CFStringRef text { CFStringCreateWithCString(kCFAllocatorDefault, real->c_str(), kCFStringEncodingUTF8) };
                    if (text == nullptr)
                        continue;
                    CFArrayAppendValue(paths, text);
                    CFRelease(text);
                    roots_.push_back(watched_root { spec.kind, *real, false });
                }

                queue_ = dispatch_queue_create("io.github.yuldk.cyella.repository-watch", DISPATCH_QUEUE_SERIAL);
                FSEventStreamContext context {};
                context.info = mailbox_.get();
                constexpr FSEventStreamCreateFlags stream_flags { kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot };
                stream_ = queue_ != nullptr
                    ? FSEventStreamCreate(kCFAllocatorDefault, on_stream_events, &context, paths, kFSEventStreamEventIdSinceNow, stream_latency_seconds, stream_flags)
                    : nullptr;
                CFRelease(paths);
                if (stream_ == nullptr)
                {
                    diagnostics.push_back(make_diagnostic(diagnostic_severity::error, tr(u8"watch.change_watch_failed", { { u8"path", specs.front().path } })));
                    return false;
                }
                FSEventStreamSetDispatchQueue(stream_, queue_);
                // 돌아오기 전에 stream을 띄워 둔다. 돌아온 뒤의 변경은 놓치지 않는다.
                //  - 시작 직전에 쓴 파일의 사건은 커널에서 FSEvents 서비스로 늦게 건너와 "지금 이후"의 사건으로 배달된다. 그런 사건은
                //    이 감시의 몫이 아니므로(소비자는 시작할 때 따로 읽는다) 밀린 것을 모두 배달받아(`FlushSync`) 버린 뒤 받기 시작한다.
                //    그 뒤에도 늦게 오는 것은 `is_stale`이 파일의 바뀐 시각으로 거른다.
                ::clock_gettime(CLOCK_REALTIME, &started_at_);
                if (FSEventStreamStart(stream_) == false)
                {
                    diagnostics.push_back(make_diagnostic(diagnostic_severity::error, tr(u8"watch.change_watch_failed", { { u8"path", specs.front().path } })));
                    return false;
                }
                started_ = true;
                FSEventStreamFlushSync(stream_);
                {
                    const std::lock_guard<std::mutex> lock { mailbox_->mutex };
                    mailbox_->discarding = false;
                    mailbox_->events.clear();
                }

                try
                {
                    thread_ = std::thread { [this] { run(); } };
                }
                catch (...)
                {
                    diagnostics.push_back(make_diagnostic(diagnostic_severity::error, tr(u8"watch.thread_failed")));
                    return false;
                }
                return true;
            }

            void stop() noexcept override
            {
                stop_requested_.store(true);
                {
                    const std::lock_guard<std::mutex> lock { mailbox_->mutex };
                    mailbox_->stopping = true;
                }
                mailbox_->changed.notify_all();

                const std::lock_guard lock { join_mutex_ };
                // 콜백 안에서 불렸다면 자기 자신을 기다릴 수 없다. 표시만 해 두면 콜백이 끝난 뒤 thread가 스스로 빠져나간다.
                if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
                    thread_.join();
            }

        private:
            void close_stream() noexcept
            {
                if (stream_ != nullptr)
                {
                    if (started_)
                        FSEventStreamStop(stream_);
                    FSEventStreamInvalidate(stream_);
                    // 이미 queue에 올라간 콜백이 끝날 때까지 기다린 뒤 stream과 상자를 푼다.
                    if (queue_ != nullptr)
                        dispatch_sync_f(queue_, nullptr, drain_queue);
                    FSEventStreamRelease(stream_);
                    stream_ = nullptr;
                }
                if (queue_ != nullptr)
                {
                    dispatch_release(queue_);
                    queue_ = nullptr;
                }
            }

            void run() noexcept
            {
                try
                {
                    loop();
                }
                catch (...)
                {}
            }

            void loop()
            {
                while (stop_requested_.load() == false)
                {
                    std::deque<raw_event> received {};
                    {
                        std::unique_lock<std::mutex> lock { mailbox_->mutex };
                        const auto ready = [this] { return mailbox_->stopping || mailbox_->events.empty() == false; };
                        if (const auto due { debouncer_.due_time() }; due.has_value())
                            mailbox_->changed.wait_until(lock, *due, ready);
                        else
                            mailbox_->changed.wait(lock, ready);
                        if (mailbox_->stopping)
                            return;
                        received.swap(mailbox_->events);
                    }
                    for (const raw_event& event : received)
                        handle_event(event);
                    if (stop_requested_.load())
                        return;
                    flush();
                }
            }

            void handle_event(const raw_event& event)
            {
                const auto now { change_debouncer::clock::now() };
                constexpr FSEventStreamEventFlags dropped { kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped };
                if ((event.flags & dropped) != 0)
                {
                    debouncer_.note_overflow(now);
                    return;
                }
                watched_root* const root { owner_of(event.path) };
                if (root == nullptr)
                {
                    // 어느 뿌리의 것인지 모르는 사건은 모르는 채로 두는 것보다 한 번 더 읽는 쪽이 안전하다.
                    debouncer_.note(change_classification::working_tree, now);
                    return;
                }
                if ((event.flags & kFSEventStreamEventFlagRootChanged) != 0)
                {
                    // 뿌리 폴더가 지워지거나 옮겨졌다. 이 뿌리는 더는 감시되지 않는다.
                    debouncer_.note_overflow(now);
                    if (root->lost == false)
                    {
                        root->lost = true;
                        notify(repository_change_kind::watch_lost);
                    }
                    return;
                }
                if (is_stale(event.path))
                    return;
                const std::u8string relative { relative_to(*root, event.path) };
                change_classification classification { classify_repository_change(root->kind, relative) };
                // 상태로 분류된 폴더(`refs/heads` 등) 자신의 사건은 거른다. 그 안의 ref가 늘거나 줄면 그 파일의 사건이 따로 온다 (Win32 판의
                // `refs\heads` 규칙과 같은 뜻이다). FSEvents는 한 경로의 플래그를 쌓아 보내므로(예전의 만듦이 남는다) 플래그로 폴더의
                // 만듦·지움을 가르지 않는다.
                if (classification == change_classification::repository_state && (event.flags & kFSEventStreamEventFlagItemIsDir) != 0)
                    classification = change_classification::ignored;
                debouncer_.note(classification, now);
            }

            // 감시를 시작하기 전의 변경이 늦게 배달된 것인가. 항목이 아직 있고 그 마지막 바뀜(ctime)이 시작보다 이르면 그렇다.
            //  - 시작한 뒤에 다시 바뀐 항목은 ctime이 시작보다 늦어 남는다. 지워진 항목은 시각을 알 수 없어 남긴다 (지운 것은 알려야 한다).
            //  - 시작한 지 오래면 보지 않는다. 늦은 배달은 시작 직후의 일이고, 그 뒤로는 사건마다 파일 정보를 읽을 까닭이 없다.
            [[nodiscard]] bool is_stale(const std::string& path) const noexcept
            {
                timespec now {};
                ::clock_gettime(CLOCK_REALTIME, &now);
                if (now.tv_sec - started_at_.tv_sec > stale_window_seconds)
                    return false;
                struct stat info {};
                if (::lstat(path.c_str(), &info) != 0)
                    return false;
                const timespec changed { info.st_ctimespec };
                return changed.tv_sec < started_at_.tv_sec || (changed.tv_sec == started_at_.tv_sec && changed.tv_nsec < started_at_.tv_nsec);
            }

            // 경로가 속한 뿌리다. 더 깊은 뿌리(작업 트리 안의 gitdir)가 이긴다.
            watched_root* owner_of(const std::string& path) noexcept
            {
                watched_root* best { nullptr };
                for (watched_root& root : roots_)
                {
                    const std::string& base { root.real_path };
                    const bool inside { path == base || (path.size() > base.size() && path.compare(0, base.size(), base) == 0 && (base == "/" || path[base.size()] == '/')) };
                    if (inside && (best == nullptr || base.size() > best->real_path.size()))
                        best = &root;
                }
                return best;
            }

            static std::u8string relative_to(const watched_root& root, const std::string& path)
            {
                if (path.size() <= root.real_path.size())
                    return {};
                const std::size_t start { root.real_path == "/" ? root.real_path.size() : root.real_path.size() + 1 };
                return macos::to_u8(std::string_view { path }.substr(start));
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
            std::vector<watched_root> roots_ {};
            std::unique_ptr<event_mailbox> mailbox_ { std::make_unique<event_mailbox>() };
            dispatch_queue_t queue_ { nullptr };
            FSEventStreamRef stream_ { nullptr };
            bool started_ { false };
            timespec started_at_ {};
            std::atomic_bool stop_requested_ { false };
            std::mutex join_mutex_ {};
            std::thread thread_ {};
        };

        class posix_repository_watcher_factory final : public repository_watcher_factory
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
            if (is_absolute_path(working_tree) == false)
            {
                result.diagnostics.push_back(make_diagnostic(diagnostic_severity::error, tr(u8"watch.working_tree_not_absolute")));
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
                result.diagnostics.push_back(make_diagnostic(diagnostic_severity::error, tr(u8"watch.unexpected_failure")));
            }
            catch (...)
            {}
            return result;
        }
    }

    std::unique_ptr<repository_watcher_factory> make_repository_watcher_factory()
    {
        return std::make_unique<posix_repository_watcher_factory>();
    }
} // namespace cyella::win32
