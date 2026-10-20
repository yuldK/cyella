#include "platform/win32/cyella_driver.h"

#include "domain/operation_status.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/askpass_helper_path.h"
#include "platform/win32/rebase_editor_path.h"
#include "platform/win32/win32_diff_tool_launcher.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_git_environment.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_repository_watcher.h"
#include "platform/win32/win32_settings_file_system.h"
#include "platform/win32/win32_sha256.h"
#include "platform/win32/win32_ssh_environment.h"
#include "platform/win32/win32_url_opener.h"
#include "platform/win32/win32_webview_support.h"
#include "platform/win32/win32_workspace_files.h"
#include "presentation/dialog_intents.h"
#include "presentation/diff_view_intents.h"
#include "presentation/selector_intents.h"
#include "presentation/settings/profiles_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/signing_intents.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/diff_webview.h"
#include "presentation/ui/diff_webview_theme.h"
#include "presentation/ui_intents.h"

#include "luil/ui/image_decode.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace cyella::win32 {
    namespace {
        // 논리 픽셀(96 DPI 기준) 상수다. 첫 frame은 창 크기 메시지가 오기 전이라 이 값으로 배치한다.
        constexpr float default_window_width { 1280.0f };
        constexpr float default_window_height { 720.0f };

        std::u8string to_u8(const std::string& text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::string to_text(const std::u8string& text)
        {
            return std::string { text.begin(), text.end() };
        }

        luil::theme_preference to_luil_theme(const theme_preference theme) noexcept
        {
            switch (theme)
            {
            case theme_preference::light:
                return luil::theme_preference::light;
            case theme_preference::dark:
                return luil::theme_preference::dark;
            case theme_preference::system:
                break;
            }
            return luil::theme_preference::system;
        }
    } // namespace

    cyella_driver::cyella_driver(std::u8string initial_repository)
        : cyella_driver { cyella_driver_options { .initial_repository = std::move(initial_repository), .settings_path = {} } }
    {}

    cyella_driver::cyella_driver(cyella_driver_options options)
        : options_ { std::move(options) }
    {}

    cyella_driver::~cyella_driver()
    {
        // stop_workers가 불리지 않은 경로(test, 시작 실패)에서도 worker를 멈춘 뒤 나머지를 푼다.
        stop_workers();
    }

    void cyella_driver::start()
    {
        process_runner_ = make_process_runner();
        files_ = make_workspace_files();
        probe_ = make_file_probe();
        if (options_.settings_path.empty() == false)
        {
            settings_files_ = make_settings_file_system();
            settings_store_ = std::make_unique<json_app_settings_store>(*settings_files_);
            settings_ = std::make_unique<app_settings_service>(*settings_store_, options_.settings_path);
            static_cast<void>(settings_->load());
        }

        // git 확인은 5초 제한이 있는 한 번의 `--version`이다. logic thread라 UI는 기다리지 않는다.
        git_tool_ = resolve_git_tool({}, current_git_tool_environment(), *process_runner_, *probe_, {});
        shell_controller::dependencies dependencies {};
        dependencies.settings = settings_.get();
        dependencies.probe = probe_.get();
        dependencies.git_tool = git_tool_;
        dependencies.request_ssh_info = [this] { request_ssh_info(); };
        dependencies.request_signing_info = [this](const std::u8string& repository_path) { request_signing_info(repository_path); };
        dependencies.write_signing_config
            = [this](const bool global, const std::u8string& repository_path, std::vector<signing_change> changes) { write_signing_config(global, repository_path, std::move(changes)); };
        dependencies.copy_text = [this](std::string text) { copy_to_clipboard(text); };
        dependencies.submit_hosting_job = [this](hosting_job job) { hosting_.submit(std::move(job)); };
        dependencies.hosting_sign_out = [this](const hosting_sign_out_request& request) { hosting_.sign_out(request); };
        dependencies.open_url = [this](const std::string& url) { open_url(url); };
        if (options_.ssh_directory.empty() == false)
            ssh_directory_ = std::filesystem::path { options_.ssh_directory };
        else if (const std::optional<std::filesystem::path> ssh_directory { user_ssh_directory() }; ssh_directory.has_value())
            ssh_directory_ = *ssh_directory;
        dependencies.ssh_directory = ssh_directory_.u8string();
        dependencies.default_key_comment = default_ssh_key_comment();
        if (git_tool_.usable())
        {
            // 서버는 앱 실행마다 한 번만 연다. 도우미나 서버가 없으면 askpass 없이 돌고 진단이 하나 남는다.
            askpass_.start(locate_askpass_helper());
            git_operation_runner_options runner_options {};
            if (askpass_.available())
            {
                runner_options.askpass_helper = askpass_.helper_path();
                runner_options.askpass = askpass_.endpoint();
            }
            // 환경 설정의 SSH 도구(키 만들기·연결 시험)는 passphrase를 askpass로 묻는다. 도우미가 없으면 쓰지 않는다.
            ssh_programs_ = locate_ssh_programs(git_tool_.executable, windows_directory(), *probe_);
            runner_options.ssh_executable = ssh_programs_.ssh;
            runner_options.ssh_keygen_executable = ssh_programs_.ssh_keygen;
            // 인터랙티브 rebase의 편집기 도우미다. 없으면 그 operation만 이유를 알리고 실패한다.
            if (const std::optional<std::filesystem::path> rebase_editor { locate_rebase_editor_helper() }; rebase_editor.has_value())
                runner_options.rebase_editor_helper = rebase_editor->u8string();
            dependencies.ssh_tools_found = ssh_programs_.found() && askpass_.available();
            dependencies.ssh_tools_origin = ssh_programs_.origin;
            git_workspace_options workspace_options {};
            workspace_options.askpass = askpass_.service();

            executor_ = std::make_unique<git_operation_runner>(*process_runner_, git_tool_.executable, *files_, std::move(runner_options));
            scheduler_ = std::make_unique<task_scheduler>();
            task_scheduler* const scheduler { scheduler_.get() };
            operation_result_relay* const relay { &relay_ };
            // GitHub·GitLab 연동: 토큰은 `git credential`로만 받고 메모리에만 둔다 (ADR-007).
            {
                hosting_bridge::options hosting_options {};
                hosting_options.git_executable = git_tool_.executable;
                hosting_options.use_system_proxy = options_.hosting_system_proxy;
                hosting_options.post = [relay](luil::app_message message) { relay->post(std::move(message)); };
                std::u8string hosting_error {};
                if (hosting_.start(*process_runner_, scheduler, &registry_, std::move(hosting_options), hosting_error) == false)
                    hosting_start_error_ = std::move(hosting_error);
            }
            // 커밋 작성자 사진(Gravatar)은 같은 전송으로 받는다. 설정에서 켰을 때만 controller가 청한다.
            if (hosting_transport* const transport { hosting_.transport() }; transport != nullptr)
            {
                avatar_fetch_options avatar_options {};
                avatar_options.cache_directory = std::filesystem::path { local_data_directory(L"avatars") };
                avatar_options.sha256_hex = [](const std::string_view text) { return sha256_hex(text); };
                avatars_ = std::make_unique<avatar_fetcher>(*transport, std::move(avatar_options));
                dependencies.request_avatar = [this](const std::string& email) { request_avatar(email); };
            }
            workspace_ = std::make_unique<git_workspace>(
                *executor_, [scheduler](operation_request request) { return scheduler->submit(std::move(request)); },
                [relay](operation_event event) { relay->post(luil::app_message { std::move(event) }); }, &registry_, workspace_options);
            dependencies.workspace = workspace_.get();
            dependencies.git_version = git_tool_.reported_version;
            start_diff_view();
            // 작성자 신원을 읽고 프로필을 저장소의 로컬 설정에 쓴다. 전역 설정 파일의 경로는 includeIf로 끌어온 파일과 가르는 데 쓴다.
            git_identity_options identity_options {};
            identity_options.hints.global_files = global_git_config_files();
            identity_ = std::make_unique<git_identity_gateway>(*process_runner_, git_tool_.executable, std::move(identity_options));
            dependencies.request_identity = [this](const std::string& repository_id, const std::u8string& path) { request_identity(repository_id, path); };
            dependencies.apply_identity = [this](const std::string& repository_id, const std::u8string& path, const std::u8string& user_name, const std::u8string& user_email) {
                apply_identity(repository_id, path, user_name, user_email);
            };
        }
        else
            dependencies.git_message = std::u8string { git_tool_unavailable_message(git_tool_.availability) };
        // 인증 대화 상자의 답·닫기는 이 driver의 백엔드 처리기로 들어간다 (`answer_input_intent`는 처리하고 바로 비밀을 지운다).
        dependencies.send_backend_intent = [this](const luil::app_message& message) { static_cast<void>(handle_backend_intent(message)); };
        controller_ = std::make_unique<shell_controller>(std::move(dependencies));
        controller_->start(options_.initial_repository);
        watch_factory_ = options_.watcher_factory;
        // 시작할 때 복원된 탭에도 감시를 켠다.
        sync_watchers();
        if (hosting_start_error_.empty() == false)
            static_cast<void>(controller_->handle(luil::app_message { show_notice_intent { .text = u8"GitHub·GitLab 연동을 쓸 수 없습니다: " + hosting_start_error_, .warning = true } }));
        // 인증 도우미를 쓸 수 없으면 인증이 필요한 작업은 즉시 실패한다. 이유를 한 번 안내로 띄운다.
        if (const std::optional<diagnostic>& problem { askpass_.startup_diagnostic() }; problem.has_value() && git_tool_.usable())
            static_cast<void>(controller_->handle(luil::app_message { show_notice_intent { .text = problem->message, .warning = true } }));
    }

    void cyella_driver::start_diff_view()
    {
        // 웹뷰를 세울 수 있는 PC인가를 앱이 미리 안다. luil은 환경을 만들지 못해도 사건을 보내지 않고 placeholder만 남기므로,
        // 여기서 거르지 않으면 diff가 영영 빈 자리로 남는다.
        const viewer_location viewer { locate_diff_viewer() };
        webview_data_folder_ = webview_user_data_directory();
        webview_capable_ = options_.force_text_diff == false && viewer.exists && webview_data_folder_.empty() == false && webview_runtime_installed();
        viewer_url_ = to_u8(make_file_url(viewer.index_path));

        diff_temp_ = std::make_unique<diff_temp_directory>();
        diff_gateway_ = std::make_unique<git_diff_gateway>(*process_runner_, git_tool_.executable, *diff_temp_);
        diff_launcher_ = make_diff_tool_launcher();
        diff_opener_ = std::make_unique<external_diff_opener>(*diff_gateway_, *diff_launcher_);

        diff_view_ports ports {};
        ports.submit = [this](const std::string& repository_id, git_operation operation) { return workspace_->submit(repository_id, std::move(operation)); };
        ports.schedule = [scheduler = scheduler_.get()](operation_request request) { return scheduler->submit(std::move(request)); };
        ports.copy_text = [this](std::string text) { copy_to_clipboard(text); };
        ports.sides_ready = [relay = &relay_](diff_sides_ready_intent message) { relay->post(luil::app_message { std::move(message) }); };
        ports.external_done = [relay = &relay_](external_open_done_intent message) { relay->post(luil::app_message { std::move(message) }); };
        ports.tools = [this] { return controller_ != nullptr ? controller_->settings().external_diff_tools : std::vector<external_diff_tool> {}; };
        ports.registry = &registry_;
        ports.gateway = diff_gateway_.get();
        ports.opener = diff_opener_.get();
        ports.git_executable = git_tool_.executable;
        // 머리 줄(경로·보기 전환)은 luil이 그리므로 뷰어 안의 머리는 끈다.
        diff_ = std::make_unique<diff_view_coordinator>(std::move(ports), diff_view_options { .line_limit = diff_view_default_line_limit, .toolbar = false });
    }

    void cyella_driver::request_ssh_info()
    {
        // 서비스 상태·`ssh-add -l`·키 목록은 프로세스를 띄우고 파일을 읽으므로 worker가 한다. 결과는 relay로 돌아온다.
        ssh_info_sources sources {};
        sources.service_status = [] { return query_ssh_agent_service_status(); };
        sources.runner = process_runner_.get();
        sources.ssh_add_executable = ssh_programs_.ssh_add;
        sources.ssh_directory = ssh_directory_;
        if (scheduler_ == nullptr)
        {
            // git을 쓸 수 없어 scheduler가 없다. 드문 경로이므로 이 thread에서 바로 읽는다.
            relay_.post(luil::app_message { ssh_info_loaded_intent { collect_ssh_info(sources) } });
            return;
        }
        auto source { std::make_shared<process_cancellation_source>() };
        registry_.add(*source);
        operation_request request {};
        request.repository_key = "ssh-info";
        request.job = [sources = std::move(sources), source, relay = &relay_](const operation_context& context) mutable {
            const process_cancellation_registration link {
                context.shutdown.cancellable() ? context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); }) : process_cancellation_registration {}
            };
            sources.cancellation = source->token();
            relay->post(luil::app_message { ssh_info_loaded_intent { collect_ssh_info(sources) } });
        };
        static_cast<void>(scheduler_->submit(std::move(request)));
    }

    void cyella_driver::open_url(const std::string& url)
    {
        const std::u8string address { to_u8(url) };
        if (is_openable_web_url(address) == false)
            return;
        if (options_.open_url_log.empty() == false)
        {
            // 검증용: 브라우저를 띄우지 않고 어떤 주소를 열려 했는지만 적는다.
            std::ofstream log { std::filesystem::path { options_.open_url_log }, std::ios::app | std::ios::binary };
            log << url << std::endl;
            return;
        }
        if (scheduler_ == nullptr)
            return;
        operation_request request {};
        request.repository_key = "open-url";
        request.job = [address](const operation_context&) { static_cast<void>(open_url_in_browser(address)); };
        static_cast<void>(scheduler_->submit(std::move(request)));
    }

    void cyella_driver::request_identity(const std::string& repository_id, const std::u8string& path)
    {
        if (scheduler_ == nullptr || identity_ == nullptr)
            return;
        // 같은 저장소의 읽기와 쓰기는 한 줄(lane)에서 차례로 돈다. 적용한 직후에 다시 읽으면 쓴 값을 읽는다.
        auto source { std::make_shared<process_cancellation_source>() };
        registry_.add(*source);
        operation_request request {};
        request.repository_key = "identity:" + repository_id;
        request.job = [gateway = identity_.get(), source, relay = &relay_, repository_id, path](const operation_context& context) {
            const process_cancellation_registration link {
                context.shutdown.cancellable() ? context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); }) : process_cancellation_registration {}
            };
            const git_identity_read read { gateway->read(path, source->token()) };
            relay->post(luil::app_message { identity_loaded_intent { .repository_id = repository_id, .identity = read.identity, .ok = read.ok, .error = read.error } });
        };
        static_cast<void>(scheduler_->submit(std::move(request)));
    }

    void cyella_driver::request_avatar(const std::string& email)
    {
        if (scheduler_ == nullptr || avatars_ == nullptr)
            return;
        auto source { std::make_shared<process_cancellation_source>() };
        registry_.add(*source);
        operation_request request {};
        // 한 줄(lane)에서 차례로 받는다. 커밋을 빠르게 훑어도 요청이 한꺼번에 몰리지 않는다.
        request.repository_key = "avatar";
        request.job = [fetcher = avatars_.get(), source, relay = &relay_, email](const operation_context& context) {
            const process_cancellation_registration link {
                context.shutdown.cancellable() ? context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); }) : process_cancellation_registration {}
            };
            const avatar_fetch_result fetched { fetcher->fetch(email, source->token()) };
            avatar_loaded_intent loaded { .email = email, .image = {}, .failed = fetched.failed };
            if (fetched.bytes.empty() == false)
            {
                // 사진은 20 논리 픽셀 동그라미에 앉는다. 큰 파일이 와도 상한으로 줄여 푼다.
                luil::image_decode_options options {};
                options.max_width = 128;
                options.max_height = 128;
                luil::image_decode_error error {};
                loaded.image = luil::decode_image_bytes(std::span { reinterpret_cast<const std::uint8_t*>(fetched.bytes.data()), fetched.bytes.size() }, options, error);
            }
            relay->post(luil::app_message { std::move(loaded) });
        };
        static_cast<void>(scheduler_->submit(std::move(request)));
    }

    void cyella_driver::apply_identity(const std::string& repository_id, const std::u8string& path, const std::u8string& user_name, const std::u8string& user_email)
    {
        if (scheduler_ == nullptr || identity_ == nullptr)
            return;
        auto source { std::make_shared<process_cancellation_source>() };
        registry_.add(*source);
        operation_request request {};
        request.repository_key = "identity:" + repository_id;
        request.job = [gateway = identity_.get(), source, relay = &relay_, repository_id, path, user_name, user_email](const operation_context& context) {
            const process_cancellation_registration link {
                context.shutdown.cancellable() ? context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); }) : process_cancellation_registration {}
            };
            const git_identity_write written { gateway->apply(path, user_name, user_email, source->token()) };
            relay->post(luil::app_message { identity_applied_intent { .repository_id = repository_id, .ok = written.ok, .message = written.message } });
        };
        static_cast<void>(scheduler_->submit(std::move(request)));
    }

    void cyella_driver::copy_to_clipboard(const std::string& text)
    {
        // 앱 UI thread가 클립보드를 소유한다. host를 묶기 전이거나 푼 뒤에는 복사할 곳이 없다.
        if (luil::app_host* const host { host_.load() }; host != nullptr)
            host->post_clipboard_request(luil::clipboard_copy_request { to_u8(text) });
    }

    void cyella_driver::handle(luil::app_message message)
    {
        // relay가 결과의 정본이다. 깨우기가 아닌 메시지가 먼저 와도 쌓인 결과부터 반영한다.
        drain_relay();
        if (message.get<relay_drain_intent>() == nullptr && handle_backend_intent(message) == false && handle_diff_view_intent(message) == false && controller_ != nullptr)
            static_cast<void>(controller_->handle(message));
        if (controller_ != nullptr)
        {
            controller_->after_events();
            // 메뉴가 복사하라고 한 글은 UI thread의 클립보드로 넘긴다.
            if (std::optional<std::u8string> text { controller_->take_clipboard_text() }; text.has_value())
                relay_.copy_to_clipboard(std::move(*text));
        }
        // 탭이 열리고 닫혔을 수 있다. 감시를 열린 저장소에 맞춘다.
        sync_watchers();
    }

    // ------------------------------------------------------------------ 파일 감시와 창 초점

    void cyella_driver::sync_watchers()
    {
        if (workspace_ == nullptr || options_.disable_file_watching)
            return;
        const std::vector<std::string> ids { workspace_->repository_ids() };
        const auto is_open = [&ids](const std::string& id) { return std::ranges::find(ids, id) != ids.end(); };
        for (auto watcher { watchers_.begin() }; watcher != watchers_.end();)
        {
            if (is_open(watcher->first))
            {
                ++watcher;
                continue;
            }
            // 탭을 닫았다. 감시를 멈추면 돌아온 뒤에는 콜백이 불리지 않는다.
            watcher->second->stop();
            refresh_policy_.forget(watcher->first);
            watcher = watchers_.erase(watcher);
        }
        std::erase_if(unwatched_, [&is_open](const std::string& id) { return is_open(id) == false; });
        for (const std::string& id : ids)
            if (watchers_.contains(id) == false && unwatched_.contains(id) == false)
                start_watcher(id);
    }

    void cyella_driver::start_watcher(const std::string& repository_id)
    {
        const repository_session* const session { std::as_const(*workspace_).find_session(repository_id) };
        if (session == nullptr)
            return;
        if (watch_factory_ == nullptr)
            watch_factory_ = std::shared_ptr<repository_watcher_factory> { make_repository_watcher_factory() };
        // 없는 폴더는 감시할 수 없지만 그 탭은 이미 "열 수 없음"으로 보이므로 따로 알리지 않는다.
        if (probe_ != nullptr && probe_->probe(session->path()) != path_kind::directory)
        {
            unwatched_.insert(repository_id);
            refresh_policy_.mark_watch_lost(repository_id);
            return;
        }
        // 감시 스레드가 부른다. 상태를 건드리지 않고 relay로 logic thread에 넘기기만 한다.
        repository_change_callback callback
            = [relay = &relay_, repository_id](const repository_change_kind kind) { relay->post(luil::app_message { repository_changed_intent { repository_id, kind } }); };
        repository_watch_start_result started { watch_factory_->start(session->path(), std::move(callback), {}) };
        if (started.watcher == nullptr)
        {
            give_up_watching(repository_id, false);
            return;
        }
        refresh_policy_.mark_watch_started(repository_id);
        watchers_[repository_id] = std::move(started.watcher);
    }

    void cyella_driver::give_up_watching(const std::string& repository_id, const bool lost)
    {
        if (const auto found { watchers_.find(repository_id) }; found != watchers_.end())
        {
            found->second->stop();
            watchers_.erase(found);
        }
        unwatched_.insert(repository_id);
        refresh_policy_.mark_watch_lost(repository_id);
        if (controller_ == nullptr || workspace_ == nullptr)
            return;
        const repository_session* const session { std::as_const(*workspace_).find_session(repository_id) };
        std::u8string name { session != nullptr ? session->path() : std::u8string {} };
        while (name.empty() == false && (name.back() == u8'\\' || name.back() == u8'/'))
            name.pop_back();
        if (const std::size_t separator { name.find_last_of(u8"\\/") }; separator != std::u8string::npos)
            name.erase(0, separator + 1);
        std::u8string text { u8"'" + name + u8"' 폴더 " };
        text += lost ? u8"감시가 끊겨 자동 갱신을 멈췄습니다." : u8"감시를 시작하지 못해 자동 갱신을 쓸 수 없습니다.";
        text += u8" 창을 다시 누를 때 새로 읽습니다.";
        static_cast<void>(controller_->handle(luil::app_message { show_notice_intent { .text = std::move(text), .warning = true } }));
    }

    void cyella_driver::on_repository_changed(const repository_changed_intent& changed)
    {
        // 닫았거나 감시를 접은 저장소에 늦게 도착한 알림이다.
        if (workspace_ == nullptr || watchers_.contains(changed.repository_id) == false)
            return;
        if (changed.kind == repository_change_kind::watch_lost)
        {
            give_up_watching(changed.repository_id, true);
            // 끊기기 전후에 놓친 변경이 있을 수 있다. 지금 전체를 한 번 읽는다.
            const repository_activity activity { workspace_->activity_of(changed.repository_id) };
            const std::optional<refresh_scope> scope { refresh_policy_.on_change(changed.repository_id, refresh_scope::full, activity, std::chrono::steady_clock::now()) };
            if (scope.has_value())
                run_refresh(changed.repository_id, *scope);
            return;
        }
        const refresh_scope wanted { changed.kind == repository_change_kind::repository_state_changed ? refresh_scope::full : refresh_scope::status };
        if (const std::optional<refresh_scope> scope { refresh_policy_.on_change(changed.repository_id, wanted, workspace_->activity_of(changed.repository_id), std::chrono::steady_clock::now()) };
            scope.has_value())
            run_refresh(changed.repository_id, *scope);
    }

    void cyella_driver::on_window_focus()
    {
        if (workspace_ == nullptr || controller_ == nullptr)
            return;
        const std::string repository_id { controller_->selected_repository_id() };
        if (repository_id.empty() || std::as_const(*workspace_).find_session(repository_id) == nullptr)
            return;
        if (const std::optional<refresh_scope> scope { refresh_policy_.on_focus(repository_id, workspace_->activity_of(repository_id), std::chrono::steady_clock::now()) }; scope.has_value())
            run_refresh(repository_id, *scope);
    }

    void cyella_driver::run_refresh(const std::string& repository_id, const refresh_scope scope)
    {
        if (workspace_ == nullptr)
            return;
        // 자동으로 낸다: 작업 로그에 (자동)으로 남고, 인증 질문이 오면 조용히 취소한다.
        if (scope == refresh_scope::full)
            static_cast<void>(workspace_->submit(repository_id, refresh_request {}, true));
        else
            static_cast<void>(workspace_->submit(repository_id, refresh_status_request {}, true));
    }

    void cyella_driver::flush_pending_refreshes()
    {
        if (workspace_ == nullptr || finished_repositories_.empty())
            return;
        const std::set<std::string> finished { std::move(finished_repositories_) };
        finished_repositories_.clear();
        for (const std::string& repository_id : finished)
            if (const std::optional<refresh_scope> scope { refresh_policy_.on_activity(repository_id, workspace_->activity_of(repository_id)) }; scope.has_value())
                run_refresh(repository_id, *scope);
    }

    void cyella_driver::drain_relay()
    {
        relay_.drain([this](luil::app_message drained) {
            if (const auto* const event { drained.get<operation_event>() }; event != nullptr)
            {
                if (workspace_ != nullptr)
                    workspace_->apply(*event);
                if (std::holds_alternative<file_diff_loaded>(event->payload))
                    ++diff_serials_[event->repository_id];
                if (const auto* const finished { std::get_if<operation_finished>(&event->payload) }; finished != nullptr)
                {
                    if (diff_ != nullptr)
                        diff_->on_operation_finished(event->id, *finished);
                    if (event->repository_id.empty() == false)
                        finished_repositories_.insert(event->repository_id);
                }
            }
            else if (const auto* const changed { drained.get<repository_changed_intent>() }; changed != nullptr)
                on_repository_changed(*changed);
            else if (drained.get<window_focus_intent>() != nullptr)
                on_window_focus();
            else if (const auto* const sides { drained.get<diff_sides_ready_intent>() }; sides != nullptr && diff_ != nullptr)
                diff_->sides_prepared(*sides);
            else if (const auto* const external { drained.get<external_open_done_intent>() }; external != nullptr && diff_ != nullptr)
                diff_->external_opened(*external);
            else if ((drained.get<ssh_info_loaded_intent>() != nullptr || drained.get<signing_info_loaded_intent>() != nullptr || drained.get<signing_write_done_intent>() != nullptr
                         || drained.get<identity_loaded_intent>() != nullptr || drained.get<identity_applied_intent>() != nullptr)
                && controller_ != nullptr)
                static_cast<void>(controller_->handle(drained));
            else if ((drained.get<hosting_result_intent>() != nullptr || drained.get<avatar_loaded_intent>() != nullptr) && controller_ != nullptr)
                static_cast<void>(controller_->handle(drained));
        });
        // 끝난 operation 뒤에 보류해 둔 새로 고침을 낸다. 한 묶음을 다 반영한 뒤라야 쓰기가 아직 줄 서 있는지 정확히 안다.
        flush_pending_refreshes();
    }

    bool cyella_driver::handle_diff_view_intent(const luil::app_message& message)
    {
        if (const auto* const web { message.get<ui::diff_webview_event_intent>() }; web != nullptr)
        {
            handle_webview_event(web->event);
            return true;
        }
        if (const auto* const layout { message.get<set_diff_layout_intent>() }; layout != nullptr)
        {
            if (diff_ != nullptr)
                diff_->controller().set_layout(layout->layout);
            return true;
        }
        if (const auto* const mode { message.get<set_diff_image_mode_intent>() }; mode != nullptr)
        {
            if (diff_ != nullptr)
                diff_->controller().set_image_mode(mode->mode);
            return true;
        }
        return false;
    }

    void cyella_driver::handle_webview_event(const luil::webview_event& event)
    {
        if (diff_ == nullptr)
            return;
        switch (event.kind)
        {
        case luil::webview_event_kind::navigation_started:
        case luil::webview_event_kind::render_process_failed:
            // 페이지가 다시 읽힌다. 새 `ready`가 올 때까지 보내지 않는다.
            diff_->page_reloading();
            break;
        case luil::webview_event_kind::navigation_failed:
        case luil::webview_event_kind::browser_process_failed:
            // 뷰어를 열지 못했거나 웹뷰가 사라졌다. 이후 diff는 텍스트 대체 보기로 보인다.
            webview_failed_ = true;
            break;
        case luil::webview_event_kind::web_message_received:
            // 웹뷰는 간 적이 있는 어느 문서든 메시지를 보낼 수 있다. 우리가 연 뷰어가 보낸 것만 믿는다.
            if (ui::is_viewer_document(event.url, viewer_url_))
                static_cast<void>(diff_->handle_web_message(to_text(event.message)));
            break;
        case luil::webview_event_kind::navigation_completed:
        case luil::webview_event_kind::navigation_blocked:
        case luil::webview_event_kind::permission_denied:
        case luil::webview_event_kind::download_blocked:
        case luil::webview_event_kind::events_dropped:
            break;
        }
    }

    void cyella_driver::sync_diff_view()
    {
        if (diff_ == nullptr || controller_ == nullptr)
            return;
        const std::optional<diff_target> wanted { controller_->current_diff_target() };
        const file_diff* loaded { nullptr };
        std::uint64_t serial { 0 };
        if (wanted.has_value() && workspace_ != nullptr)
        {
            if (const repository_session* const session { std::as_const(*workspace_).find_session(wanted->repository_id) }; session != nullptr && session->selected_diff().has_value())
                loaded = &*session->selected_diff();
            if (const auto found { diff_serials_.find(wanted->repository_id) }; found != diff_serials_.end())
                serial = found->second;
        }
        diff_->sync(wanted, loaded, serial);
        while (const std::optional<diff_notice> notice { diff_->take_notice() })
            static_cast<void>(controller_->handle(luil::app_message { show_notice_intent { notice->text, notice->warning } }));
    }

    bool cyella_driver::handle_backend_intent(const luil::app_message& message)
    {
        if (message.get<close_intent>() != nullptr)
        {
            // 끌기가 멈추기를 기다리던 설정도 닫기 전에 쓴다.
            if (controller_ != nullptr)
                controller_->flush();
            closed_.store(true);
            return true;
        }
        if (const auto* const metrics { message.get<window_metrics_intent>() }; metrics != nullptr)
        {
            metrics_ = *metrics;
            return true;
        }
        if (const auto* const open { message.get<open_repository_intent>() }; open != nullptr)
        {
            // 탭과 최근 목록까지 함께 바뀌어야 하므로 controller가 연다.
            if (controller_ != nullptr)
                controller_->open_repository(open->path);
            return true;
        }
        if (workspace_ == nullptr)
            return false;
        if (const auto* const close { message.get<close_repository_intent>() }; close != nullptr)
        {
            static_cast<void>(workspace_->close_repository(close->repository_id));
            return true;
        }
        if (const auto* const submit { message.get<submit_operation_intent>() }; submit != nullptr)
        {
            static_cast<void>(workspace_->submit(submit->repository_id, submit->operation));
            return true;
        }
        if (const auto* const setup { message.get<submit_setup_intent>() }; setup != nullptr)
        {
            static_cast<void>(workspace_->submit_setup(setup->operation));
            return true;
        }
        if (const auto* const answer { message.get<answer_input_intent>() }; answer != nullptr)
        {
            // 답은 비밀일 수 있다. workspace에는 사본을 넘기고 메시지가 쥔 원본은 바로 지운다.
            std::u8string text { answer->answer };
            wipe_secret_text(answer->answer);
            static_cast<void>(workspace_->answer_input(answer->id, answer->prompt_id, text));
            wipe_secret_text(text);
            return true;
        }
        if (const auto* const close_input { message.get<cancel_input_intent>() }; close_input != nullptr)
        {
            static_cast<void>(workspace_->cancel_input(close_input->id, close_input->prompt_id));
            return true;
        }
        if (const auto* const cancel { message.get<cancel_operation_intent>() }; cancel != nullptr)
        {
            static_cast<void>(workspace_->cancel(cancel->id));
            return true;
        }
        return false;
    }

    std::shared_ptr<const luil::win32::ui_frame> cyella_driver::make_frame()
    {
        auto frame { std::make_shared<luil::win32::ui_frame>() };
        if (controller_ == nullptr)
            return frame;
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float width { metrics_.width > 0.0f ? metrics_.width : default_window_width * scale };
        const float height { metrics_.height > 0.0f ? metrics_.height : default_window_height * scale };

        sync_diff_view();
        if (diff_ != nullptr)
        {
            // 앱 테마를 웹뷰의 CSS 변수로 옮긴다. 테마가 바뀌면 `set_theme` 한 통만 간다.
            // 팔레트를 만들고 CSS 변수로 옮기는 일은 선택된 테마가 바뀔 때만 한다 (frame마다 문자열 수십 개를 짓지 않는다).
            const luil::color_theme theme { luil::resolve_color_theme(to_luil_theme(controller_->settings().appearance.theme), high_contrast_enabled(), system_prefers_light_theme()) };
            const appearance_settings& appearance { controller_->settings().appearance };
            if (diff_theme_source_.has_value() == false || *diff_theme_source_ != theme || diff_ui_font_ != appearance.ui_font || diff_code_font_ != appearance.code_font)
            {
                diff_theme_source_ = theme;
                diff_ui_font_ = appearance.ui_font;
                diff_code_font_ = appearance.code_font;
                diff_theme_ = ui::make_diff_theme(luil::color_palette_for(theme), appearance.ui_font, appearance.code_font);
            }
            diff_->controller().set_theme(diff_theme_);
            // 환경 설정의 기본 보기가 바뀌면 열려 있는 diff도 그 보기로 바꾼다. diff 머리의 전환 단추는 그 diff 하나만 바꾼다.
            if (const diff_layout_preference preference { controller_->settings().diff.layout }; applied_diff_layout_.has_value() == false || *applied_diff_layout_ != preference)
            {
                applied_diff_layout_ = preference;
                diff_->controller().set_layout(preference == diff_layout_preference::split ? diff_layout::split : diff_layout::inline_view);
            }
            static_cast<void>(diff_->controller().poll());
            diff_surface_state surface {};
            surface.layout = diff_->controller().state().layout;
            surface.image_mode = diff_->controller().state().image_mode;
            surface.content = diff_->controller().content();
            surface.truncated = diff_->controller().truncated();
            surface.textconv_driver = diff_->controller().textconv_driver();
            surface.webview_available = webview_usable();
            controller_->set_diff_surface(surface);
        }

        const view_snapshot view { controller_->make_view(width, height, scale) };
        ui::built_tree built { ui::build_ui_tree(view) };
        controller_->apply_feedback(built.feedback);
        frame->tree = std::move(built.tree);
        for (ui::built_popup& popup : built.popups)
        {
            luil::win32::ui_popup shown {};
            shown.id = std::move(popup.id);
            shown.x = popup.x;
            shown.y = popup.y;
            shown.width = popup.width;
            shown.height = popup.height;
            shown.tree = std::move(popup.tree);
            // 바깥을 누르거나 Esc·휠·창 이동이면 닫는다. 닫는 것은 상태(`ui_state::menu`)이고 popup은 다음 frame에 빠진다.
            if (popup.font)
                shown.dismiss = [](const luil::win32::popup_dismiss_reason reason) {
                    if (reason == luil::win32::popup_dismiss_reason::activation_changed || reason == luil::win32::popup_dismiss_reason::surface_moved)
                        return luil::input_action {};
                    return luil::make_app_action(close_font_list_intent {});
                };
            else if (popup.selector)
                // 거르는 칸이 있는 목록이다. 폴더 대화 상자를 열거나 다른 창을 보느라 창 활성이 바뀌어도 남는다 (luil의 검색 popup 규약).
                shown.dismiss = [](const luil::win32::popup_dismiss_reason reason) {
                    if (reason == luil::win32::popup_dismiss_reason::activation_changed || reason == luil::win32::popup_dismiss_reason::surface_moved)
                        return luil::input_action {};
                    return luil::make_app_action(close_selector_intent {});
                };
            else
                shown.dismiss = [](const luil::win32::popup_dismiss_reason) { return luil::make_app_action(close_menu_intent {}); };
            frame->popups.push_back(std::move(shown));
        }
        frame->appearance.theme = to_luil_theme(view.theme);
        frame->fonts.ui_family = controller_->settings().appearance.ui_font;
        frame->fonts.code_family = controller_->settings().appearance.code_font;

        // 웹뷰는 한 번 필요해진 뒤로 계속 싣는다. 빼면 luil이 웹뷰를 닫아 페이지 상태가 날아간다. 자리표(webview_element)가
        // tree에 없는 동안은 luil이 숨겨 둔다.
        if (diff_ != nullptr && webview_usable())
        {
            if (view.repository.has_value() && view.repository->diff.has_value())
                webview_requested_ = true;
            if (webview_requested_)
            {
                // 메시지 내용은 번호가 바뀔 때만 다시 만든다. 번호가 같은 frame에도 내용을 실어야 건너뛴 frame에서 잃지 않는다.
                if (diff_->controller().post_revision() != posted_revision_)
                {
                    posted_revision_ = diff_->controller().post_revision();
                    posted_message_ = to_u8(diff_->controller().latest_message());
                }
                frame->webviews.push_back(ui::make_diff_webview({ viewer_url_, webview_data_folder_, posted_revision_, posted_message_ }));
            }
        }
        return frame;
    }

    luil::app_message cyella_driver::make_close_message()
    {
        return luil::app_message { close_intent {} };
    }

    bool cyella_driver::shutdown_completed() const
    {
        return closed_.load();
    }

    std::optional<std::chrono::steady_clock::time_point> cyella_driver::next_tick()
    {
        return controller_ != nullptr ? controller_->next_tick() : std::nullopt;
    }

    void cyella_driver::tick(const std::chrono::steady_clock::time_point now)
    {
        if (controller_ != nullptr)
            controller_->tick(now);
    }

    void cyella_driver::cancel() noexcept
    {
        // close 메시지가 유실돼도 실행 중인 git이 종료 예산 안에 끝나게 한다. 응답을 기다리는 GitHub·GitLab 요청도 접는다.
        hosting_.cancel_all();
        registry_.cancel_all();
    }

    void cyella_driver::stop_workers()
    {
        host_.store(nullptr);
        // 감시 스레드를 먼저 멈춘다. 돌아온 뒤에는 콜백이 relay를 부르지 않는다.
        for (auto& [id, watcher] : watchers_)
            watcher->stop();
        watchers_.clear();
        // 서버를 먼저 닫아 대기 질문을 취소로 답한다. 그래야 프롬프트에서 멈춘 git과 도우미가 풀려
        // scheduler가 worker를 join할 때 매달리지 않는다.
        askpass_.shutdown();
        hosting_.cancel_all();
        registry_.cancel_all();
        if (scheduler_ != nullptr)
            scheduler_->shutdown();
        // 워커가 모두 멈췄다: 전송을 닫고 메모리의 토큰을 지운다.
        hosting_.stop();
        relay_.unbind();
    }
} // namespace cyella::win32
