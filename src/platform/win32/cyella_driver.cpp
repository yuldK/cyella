#include "platform/win32/cyella_driver.h"

#include "domain/operation_status.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/askpass_helper_path.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_settings_file_system.h"
#include "platform/win32/win32_workspace_files.h"
#include "presentation/ui/build_ui_tree.h"

#include <memory>
#include <string>
#include <utility>

namespace cyella::win32 {
    namespace {
        // 논리 픽셀(96 DPI 기준) 상수다. 첫 frame은 창 크기 메시지가 오기 전이라 이 값으로 배치한다.
        constexpr float default_window_width { 1280.0f };
        constexpr float default_window_height { 720.0f };

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
            git_workspace_options workspace_options {};
            workspace_options.askpass = askpass_.service();

            executor_ = std::make_unique<git_operation_runner>(*process_runner_, git_tool_.executable, *files_, std::move(runner_options));
            scheduler_ = std::make_unique<task_scheduler>();
            task_scheduler* const scheduler { scheduler_.get() };
            operation_result_relay* const relay { &relay_ };
            workspace_ = std::make_unique<git_workspace>(
                *executor_, [scheduler](operation_request request) { return scheduler->submit(std::move(request)); },
                [relay](operation_event event) { relay->post(luil::app_message { std::move(event) }); }, &registry_, workspace_options);
            dependencies.workspace = workspace_.get();
            dependencies.git_version = git_tool_.reported_version;
        }
        else
            dependencies.git_message = std::u8string { git_tool_unavailable_message(git_tool_.availability) };
        controller_ = std::make_unique<shell_controller>(std::move(dependencies));
        controller_->start(options_.initial_repository);
    }

    void cyella_driver::handle(luil::app_message message)
    {
        // relay가 결과의 정본이다. 깨우기가 아닌 메시지가 먼저 와도 쌓인 결과부터 반영한다.
        drain_relay();
        if (message.get<relay_drain_intent>() == nullptr && handle_backend_intent(message) == false && controller_ != nullptr)
            static_cast<void>(controller_->handle(message));
        if (controller_ != nullptr)
            controller_->after_events();
    }

    void cyella_driver::drain_relay()
    {
        relay_.drain([this](luil::app_message drained) {
            const auto* const event { drained.get<operation_event>() };
            if (event != nullptr && workspace_ != nullptr)
                workspace_->apply(*event);
        });
    }

    bool cyella_driver::handle_backend_intent(const luil::app_message& message)
    {
        if (message.get<close_intent>() != nullptr)
        {
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

        const view_snapshot view { controller_->make_view(width, height, scale) };
        ui::built_tree built { ui::build_ui_tree(view) };
        controller_->apply_feedback(built.feedback);
        frame->tree = std::move(built.tree);
        frame->appearance.theme = to_luil_theme(view.theme);
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

    void cyella_driver::cancel() noexcept
    {
        // close 메시지가 유실돼도 실행 중인 git이 종료 예산 안에 끝나게 한다.
        registry_.cancel_all();
    }

    void cyella_driver::stop_workers()
    {
        // 서버를 먼저 닫아 대기 질문을 취소로 답한다. 그래야 프롬프트에서 멈춘 git과 도우미가 풀려
        // scheduler가 worker를 join할 때 매달리지 않는다.
        askpass_.shutdown();
        registry_.cancel_all();
        if (scheduler_ != nullptr)
            scheduler_->shutdown();
        relay_.unbind();
    }
} // namespace cyella::win32
