#include "application/diff_view_coordinator.h"

#include <utility>
#include <variant>

namespace cyella {
    namespace {
        // 이미지·바이너리 두 쪽 꺼내기와 외부 도구 열기가 서는 scheduler의 한 줄이다. 저장소 쓰기 lane과 섞이지 않고,
        // 읽기 worker(log 스트림이 붙잡을 수 있다)도 쓰지 않는다.
        constexpr std::string_view diff_view_lane_key { "diff-view" };

        std::u8string to_u8(const std::string& text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        // 앱 종료(scheduler 종료 token)를 이 일의 취소 source에 이어 준다. 반환값을 job이 끝날 때까지 쥔다.
        process_cancellation_registration link_shutdown(const operation_context& context, const std::shared_ptr<process_cancellation_source>& source)
        {
            if (context.shutdown.cancellable() == false)
                return {};
            return context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); });
        }
    } // namespace

    diff_view_coordinator::diff_view_coordinator(diff_view_ports ports, const diff_view_options options)
        : ports_ { std::move(ports) }
        , controller_ { options }
    {}

    diff_view_coordinator::~diff_view_coordinator()
    {
        // worker가 쥔 source를 취소해 두고 떠난다 (scheduler가 먼저 멈춘 뒤라 보통은 이미 끝났다).
        if (sides_source_ != nullptr)
            sides_source_->request_cancellation();
        if (external_source_ != nullptr)
            external_source_->request_cancellation();
    }

    std::shared_ptr<process_cancellation_source> diff_view_coordinator::replace_source(std::shared_ptr<process_cancellation_source>& slot)
    {
        if (slot != nullptr)
            slot->request_cancellation();
        slot = std::make_shared<process_cancellation_source>();
        if (ports_.registry != nullptr)
            ports_.registry->add(*slot);
        return slot;
    }

    void diff_view_coordinator::forget_shown()
    {
        shown_target_.reset();
        shown_serial_ = 0;
        // 다른 파일로 넘어갔으면 앞 파일의 두 쪽 꺼내기는 쓸모없다.
        if (sides_source_ != nullptr)
            sides_source_->request_cancellation();
    }

    void diff_view_coordinator::sync(const std::optional<diff_target>& wanted, const file_diff* const loaded, const std::uint64_t serial)
    {
        if (wanted.has_value() == false)
        {
            if (shown_target_.has_value())
            {
                forget_shown();
                controller_.clear();
            }
            return;
        }

        const bool same_target { shown_target_.has_value() && shown_target_->repository_id == wanted->repository_id && shown_target_->request == wanted->request };
        if (loaded != nullptr && loaded->request == wanted->request)
        {
            if (same_target && shown_serial_ == serial)
                return;
            shown_target_ = wanted;
            shown_serial_ = serial;
            // 새로 보이는 diff는 새 ticket을 가지므로 앞 diff의 두 쪽 꺼내기는 쓸모없다.
            if (sides_source_ != nullptr)
                sides_source_->request_cancellation();
            run_effects(controller_.show(*loaded, wanted->repository_path));
            return;
        }

        // 아직 읽는 중이거나 다른 파일의 diff가 와 있다. 낡은 내용을 남기면 stage 단추가 엉뚱한 파일에 걸린다.
        if (shown_target_.has_value() && same_target == false)
        {
            forget_shown();
            controller_.clear();
        }
    }

    web_message_result diff_view_coordinator::handle_web_message(const std::string_view message)
    {
        web_message_result result { controller_.handle_web_message(message) };
        run_effects(result.effects);
        return result;
    }

    void diff_view_coordinator::page_reloading() noexcept
    {
        controller_.page_reloading();
    }

    void diff_view_coordinator::sides_prepared(const diff_sides_ready_intent& message)
    {
        controller_.sides_prepared(message.ticket, message.sides);
    }

    void diff_view_coordinator::external_opened(const external_open_done_intent& message)
    {
        diff_notice result { describe_external_open(message.result) };
        notice(std::move(result.text), result.warning);
    }

    void diff_view_coordinator::on_operation_finished(const operation_id id, const operation_finished& finished)
    {
        if (id == 0)
            return;
        if (id == pending_apply_)
        {
            pending_apply_ = 0;
            if (finished.state != operation_state::succeeded)
            {
                controller_.action_failed();
                if (finished.state != operation_state::cancelled)
                {
                    const std::u8string& reason { finished.failure.has_value() && finished.failure->summary.empty() == false ? finished.failure->summary : finished.summary };
                    notice(u8"변경을 적용하지 못했습니다" + (reason.empty() ? std::u8string {} : u8": " + reason), true);
                }
                return;
            }
            // 적용이 됐다. 같은 파일의 diff를 다시 읽는다. 결과가 오면 `sync`가 다시 보인다.
            if (shown_target_.has_value() == false)
                return;
            pending_reload_ = ports_.submit != nullptr ? ports_.submit(shown_target_->repository_id, shown_target_->request) : 0;
            if (pending_reload_ == 0)
                controller_.action_failed();
            return;
        }
        if (id == pending_reload_)
        {
            pending_reload_ = 0;
            // 다시 읽지 못하면 웹뷰의 단추가 잠긴 채 남으므로 풀어 준다. 성공이면 `sync`가 곧 새 diff를 보인다.
            if (finished.state != operation_state::succeeded)
                controller_.action_failed();
        }
    }

    std::optional<diff_notice> diff_view_coordinator::take_notice()
    {
        if (notices_.empty())
            return std::nullopt;
        diff_notice front { std::move(notices_.front()) };
        notices_.erase(notices_.begin());
        return front;
    }

    void diff_view_coordinator::notice(std::u8string text, const bool warning)
    {
        notices_.push_back({ std::move(text), warning });
    }

    void diff_view_coordinator::run_effects(const std::vector<diff_view_effect>& effects)
    {
        for (const diff_view_effect& effect : effects)
        {
            if (const auto* const apply { std::get_if<apply_patch_effect>(&effect) }; apply != nullptr)
                run_apply(*apply);
            else if (const auto* const prepare { std::get_if<prepare_sides_effect>(&effect) }; prepare != nullptr)
                run_prepare_sides(*prepare);
            else if (const auto* const external { std::get_if<open_external_effect>(&effect) }; external != nullptr)
                run_open_external(*external);
            else if (const auto* const copy { std::get_if<copy_text_effect>(&effect) }; copy != nullptr)
            {
                if (ports_.copy_text != nullptr)
                    ports_.copy_text(copy->text);
            }
        }
    }

    void diff_view_coordinator::run_apply(const apply_patch_effect& effect)
    {
        // 한 번에 하나만 적용한다. 컨트롤러가 이미 막지만 같은 hunk의 두 번 적용은 파일을 망가뜨리므로 한 번 더 막는다.
        if (shown_target_.has_value() == false || ports_.submit == nullptr || pending_apply_ != 0)
        {
            controller_.action_failed();
            return;
        }
        pending_apply_ = ports_.submit(shown_target_->repository_id, effect.request);
        if (pending_apply_ == 0)
        {
            controller_.action_failed();
            notice(u8"변경을 적용하지 못했습니다: 작업을 낼 수 없습니다.", true);
        }
    }

    void diff_view_coordinator::run_prepare_sides(const prepare_sides_effect& effect)
    {
        const std::shared_ptr<process_cancellation_source> source { replace_source(sides_source_) };
        operation_request request {};
        request.repository_key = std::string { diff_view_lane_key };
        request.job = [gateway = ports_.gateway, plan = effect.plan, write_files = effect.write_files, ticket = effect.ticket, source, post = ports_.sides_ready](const operation_context& context) {
            const process_cancellation_registration link { link_shutdown(context, source) };
            diff_sides_ready_intent message {};
            message.ticket = ticket;
            if (gateway == nullptr || source->token().cancelled())
                message.sides.error = "취소되었습니다.";
            else
                message.sides = gateway->extract_cancellable(plan, write_files, source->token());
            if (post != nullptr)
                post(std::move(message));
        };
        if (ports_.schedule == nullptr || ports_.schedule(std::move(request)) == false)
        {
            // 줄에 세우지 못했다(종료 중). 두 쪽을 꺼내지 못한 것으로 알려 바이너리 카드로 물러서게 한다.
            diff_sides_ready_intent failed {};
            failed.ticket = effect.ticket;
            failed.sides.error = "작업을 줄에 세우지 못했습니다.";
            controller_.sides_prepared(failed.ticket, failed.sides);
        }
    }

    void diff_view_coordinator::open_file_app(external_file_app app)
    {
        if (shown_target_.has_value() == false || controller_.content() == diff_content::empty)
        {
            notice(u8"파일을 읽은 뒤 다시 시도해 주세요.", true);
            return;
        }
        run_open_external({ controller_.make_plan() }, std::move(app));
    }

    void diff_view_coordinator::run_open_external(const open_external_effect& effect, std::optional<external_file_app> app)
    {
        const std::shared_ptr<process_cancellation_source> source { replace_source(external_source_) };
        external_open_request open {};
        open.plan = effect.plan;
        open.file_app = std::move(app);
        open.git_executable = ports_.git_executable;
        if (ports_.tools != nullptr)
            open.tools = ports_.tools();
        open.cancellation = source->token();

        operation_request request {};
        request.repository_key = std::string { diff_view_lane_key };
        request.job = [opener = ports_.opener, open = std::move(open), source, post = ports_.external_done](const operation_context& context) {
            const process_cancellation_registration link { link_shutdown(context, source) };
            external_open_done_intent message {};
            if (opener == nullptr || source->token().cancelled())
                message.result = { external_open_status::launch_failed, {}, "취소되었습니다." };
            else
                message.result = opener->open(open);
            if (post != nullptr)
                post(std::move(message));
        };
        if (ports_.schedule == nullptr || ports_.schedule(std::move(request)) == false)
            notice(u8"외부 도구를 열지 못했습니다: 작업을 줄에 세우지 못했습니다.", true);
    }

    diff_notice describe_external_open(const external_open_result& result)
    {
        const std::u8string detail { to_u8(result.detail) };
        switch (result.status)
        {
        case external_open_status::launched_tool:
            return { u8"외부 도구를 열었습니다: " + result.tool, false };
        case external_open_status::launched_difftool:
            return { u8"git difftool로 열었습니다.", false };
        case external_open_status::no_tool:
            return { u8"이 파일을 열 외부 도구가 없습니다. 환경 설정의 'diff 도구'에서 확장자별 도구를 지정하거나 git의 diff.tool을 설정하세요.", true };
        case external_open_status::extraction_failed:
            return { u8"diff의 두 쪽 파일을 꺼내지 못했습니다" + (detail.empty() ? std::u8string {} : u8": " + detail), true };
        case external_open_status::invalid_template:
            return { u8"외부 도구의 인자 템플릿이 올바르지 않습니다. 환경 설정의 'diff 도구'를 확인하세요" + (detail.empty() ? std::u8string {} : u8": " + detail), true };
        case external_open_status::launch_failed:
            return { u8"외부 도구를 실행하지 못했습니다" + (result.tool.empty() ? std::u8string {} : u8": " + result.tool) + (detail.empty() ? std::u8string {} : u8" (" + detail + u8")"), true };
        }
        return { u8"외부 도구를 열지 못했습니다.", true };
    }
} // namespace cyella
