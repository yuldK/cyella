#include "platform/win32/cyella_driver.h"

#include "domain/operation_status.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_workspace_files.h"
#include "presentation/ui/cyella_caption.h"

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cyella::win32 {
    namespace {
        constexpr luil::ui_element_kind kind_placeholder_label { luil::application_element_kind(0) };

        // 논리 픽셀(96 DPI 기준) 상수다. 그릴 때 배율을 곱한다.
        constexpr float default_window_width { 1280.0f };
        constexpr float default_window_height { 720.0f };
        constexpr float content_padding { 24.0f };
        constexpr float placeholder_font_size { 14.0f };
        // 확인용 글은 최근 operation 몇 개만 보인다. 화면은 presentation 계층이 그린다.
        constexpr std::size_t debug_operation_count { 8 };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string number(const std::uint64_t value)
        {
            return to_u8(std::to_string(value));
        }

        // 상태를 눈으로 확인하는 글이다. 세션 상태를 그대로 읽어 줄로 만든다.
        std::vector<std::u8string> debug_lines(const git_tool_info& tool, const git_workspace* const workspace)
        {
            std::vector<std::u8string> lines {};
            if (workspace == nullptr)
            {
                lines.push_back(u8"git: " + std::u8string { git_tool_unavailable_message(tool.availability) });
                return lines;
            }
            lines.push_back(u8"git: " + tool.reported_version);
            for (const std::string& id : workspace->repository_ids())
            {
                const repository_session* const session { workspace->find_session(id) };
                if (session == nullptr)
                    continue;
                const repository_summary& summary { session->snapshot().summary };
                std::u8string line { session->path() + u8"  [" + to_u8(summary.is_detached ? summary.head_sha.substr(0, 7) : summary.current_branch) + u8"]" };
                line += u8"  +" + number(summary.ahead) + u8" -" + number(summary.behind);
                line += u8"  changes " + number(summary.changed_file_count);
                line += u8"  rows " + number(session->graph_rows().size());
                if (summary.in_progress.kind != repository_operation_state::none)
                    line += u8"  " + to_u8(repository_operation_state_name(summary.in_progress.kind)) + u8" " + number(summary.in_progress.step) + u8"/" + number(summary.in_progress.total);
                lines.push_back(std::move(line));
            }
            const std::deque<operation_id>& order { workspace->operation_order() };
            const std::size_t first { order.size() > debug_operation_count ? order.size() - debug_operation_count : 0 };
            for (std::size_t index { first }; index < order.size(); ++index)
            {
                const operation_status* const status { workspace->find_operation(order[index]) };
                if (status == nullptr)
                    continue;
                std::u8string line { u8"#" + number(status->id) + u8" " + to_u8(operation_kind_name(status->kind)) + u8" " + to_u8(operation_state_name(status->state)) };
                if (status->progress.phase_text.empty() == false)
                    line += u8"  " + status->progress.phase_text;
                if (status->failure.has_value())
                    line += u8"  " + to_u8(failure_kind_name(status->failure->kind)) + u8": " + status->failure->summary;
                lines.push_back(std::move(line));
            }
            return lines;
        }
    } // namespace

    cyella_driver::cyella_driver(std::u8string initial_repository)
        : initial_repository_ { std::move(initial_repository) }
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
        // git 확인은 5초 제한이 있는 한 번의 `--version`이다. logic thread라 UI는 기다리지 않는다.
        const std::unique_ptr<file_probe> probe { make_file_probe() };
        git_tool_ = resolve_git_tool({}, current_git_tool_environment(), *process_runner_, *probe, {});
        if (git_tool_.usable() == false)
            return;

        executor_ = std::make_unique<git_operation_runner>(*process_runner_, git_tool_.executable, *files_);
        scheduler_ = std::make_unique<task_scheduler>();
        task_scheduler* const scheduler { scheduler_.get() };
        operation_result_relay* const relay { &relay_ };
        workspace_ = std::make_unique<git_workspace>(
            *executor_, [scheduler](operation_request request) { return scheduler->submit(std::move(request)); },
            [relay](operation_event event) { relay->post(luil::app_message { std::move(event) }); }, &registry_);
        if (initial_repository_.empty() == false)
            static_cast<void>(workspace_->open_repository(initial_repository_));
    }

    void cyella_driver::handle(luil::app_message message)
    {
        // relay가 결과의 정본이다. 깨우기가 아닌 메시지가 먼저 와도 쌓인 결과부터 반영한다.
        drain_relay();
        if (message.get<relay_drain_intent>() != nullptr)
            return;
        handle_intent(message);
    }

    void cyella_driver::drain_relay()
    {
        relay_.drain([this](luil::app_message drained) {
            const auto* const event { drained.get<operation_event>() };
            if (event != nullptr && workspace_ != nullptr)
                workspace_->apply(*event);
        });
    }

    void cyella_driver::handle_intent(const luil::app_message& message)
    {
        if (message.get<close_intent>() != nullptr)
        {
            closed_.store(true);
            return;
        }
        if (const auto* const metrics { message.get<window_metrics_intent>() }; metrics != nullptr)
        {
            metrics_ = *metrics;
            return;
        }
        if (workspace_ == nullptr)
            return;
        if (const auto* const open { message.get<open_repository_intent>() }; open != nullptr)
        {
            static_cast<void>(workspace_->open_repository(open->path));
            return;
        }
        if (const auto* const close { message.get<close_repository_intent>() }; close != nullptr)
        {
            static_cast<void>(workspace_->close_repository(close->repository_id));
            return;
        }
        if (const auto* const submit { message.get<submit_operation_intent>() }; submit != nullptr)
        {
            static_cast<void>(workspace_->submit(submit->repository_id, submit->operation));
            return;
        }
        if (const auto* const setup { message.get<submit_setup_intent>() }; setup != nullptr)
        {
            static_cast<void>(workspace_->submit_setup(setup->operation));
            return;
        }
        if (const auto* const cancel { message.get<cancel_operation_intent>() }; cancel != nullptr)
            static_cast<void>(workspace_->cancel(cancel->id));
    }

    std::shared_ptr<const luil::win32::ui_frame> cyella_driver::make_frame()
    {
        const float scale { metrics_.scale > 0.0f ? metrics_.scale : 1.0f };
        const float width { metrics_.width > 0.0f ? metrics_.width : default_window_width };
        const float height { metrics_.height > 0.0f ? metrics_.height : default_window_height };

        auto root { std::make_unique<luil::root_element>() };
        root->arrange({ { 0.0f, 0.0f, width, height }, scale });

        // 데스크톱은 custom caption, 모바일은 앱 바다. 높이는 논리 픽셀이라 배율을 곱한다.
        const luil::caption_config caption_config { ui::cyella_caption() };
        float caption_height { 0.0f };
        if (luil::current_ui_platform().window_caption)
        {
            caption_height = luil::caption_element::height_for(caption_config) * scale;
            auto caption { std::make_unique<luil::caption_element>(caption_config) };
            caption->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
            root->add(std::move(caption));
        }
        else
        {
            const luil::app_bar_config app_bar_config { .title = caption_config.title };
            caption_height = luil::app_bar_element::height_for(app_bar_config) * scale;
            auto app_bar { std::make_unique<luil::app_bar_element>(app_bar_config) };
            app_bar->arrange({ { 0.0f, 0.0f, width, caption_height }, scale });
            root->add(std::move(app_bar));
        }

        luil::stack_config column_config {};
        column_config.padding = luil::edge_insets::all(content_padding);
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_placeholder_label, u8"layout" }, column_config) };

        std::vector<std::u8string> lines { u8"cyella" };
        for (std::u8string& line : debug_lines(git_tool_, workspace_.get()))
            lines.push_back(std::move(line));
        for (std::size_t index { 0 }; index < lines.size(); ++index)
        {
            luil::label_config label_config {};
            label_config.text = lines[index];
            label_config.font_size = placeholder_font_size;
            label_config.color = index == 0 ? luil::label_color_role::primary : luil::label_color_role::dim;
            auto label { std::make_unique<luil::label_element>(luil::ui_element_id { kind_placeholder_label, u8"line-" + number(index) }, label_config) };
            column->add(std::move(label), luil::label_element::height_for(label_config));
        }

        // caption 아래 나머지 영역이 내용의 slot이다.
        column->arrange({ { 0.0f, caption_height, width, height - caption_height }, scale });
        root->add(std::move(column));

        auto frame { std::make_shared<luil::win32::ui_frame>() };
        frame->tree = std::make_shared<const luil::ui_tree>(std::move(root));
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
        registry_.cancel_all();
        if (scheduler_ != nullptr)
            scheduler_->shutdown();
        relay_.unbind();
    }
} // namespace cyella::win32
