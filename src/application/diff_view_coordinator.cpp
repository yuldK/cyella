#include "application/diff_view_coordinator.h"

#include "domain/text_catalog.h"

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
        reload_request_.reset();
        // 다른 파일로 넘어갔으면 앞 파일의 두 쪽 꺼내기는 쓸모없다.
        if (sides_source_ != nullptr)
            sides_source_->request_cancellation();
    }

    void diff_view_coordinator::sync_blame(const std::optional<blame_target>& wanted, const file_blame* const loaded, const std::uint64_t serial)
    {
        // blame으로 넘어왔다. 보이던 diff는 잊어 그 stage 단추와 두 쪽 꺼내기가 남지 않게 한다.
        if (shown_target_.has_value())
        {
            forget_shown();
            controller_.clear();
        }
        if (wanted.has_value() == false)
        {
            if (shown_blame_.has_value())
            {
                shown_blame_.reset();
                controller_.clear();
            }
            return;
        }

        const bool same_target { shown_blame_.has_value() && shown_blame_->repository_id == wanted->repository_id && shown_blame_->request == wanted->request };
        if (loaded != nullptr && loaded->request == wanted->request)
        {
            if (same_target && shown_blame_serial_ == serial && shown_blame_->focus == wanted->focus)
                return;
            shown_blame_ = wanted;
            shown_blame_serial_ = serial;
            controller_.show_blame(*loaded, wanted->focus);
            return;
        }
        // 아직 읽는 중이다. 다른 파일의 blame을 남기면 새 파일의 것으로 보인다.
        if (shown_blame_.has_value() && same_target == false)
        {
            shown_blame_.reset();
            controller_.clear();
        }
    }

    void diff_view_coordinator::sync(const std::optional<diff_target>& wanted, const file_diff* const loaded, const std::uint64_t serial)
    {
        // diff로 돌아왔다. 보이던 blame은 잊는다.
        if (shown_blame_.has_value())
        {
            shown_blame_.reset();
            controller_.clear();
        }
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
            // 대상이 바뀌었으면 앞 대상을 위해 고쳐 낸 다시 읽기 요청은 더는 쓸모없다. 화면이 그 요청으로 따라온 경우도 여기다.
            if (same_target == false)
                reload_request_.reset();
            shown_target_ = wanted;
            shown_serial_ = serial;
            // 새로 보이는 diff는 새 ticket을 가지므로 앞 diff의 두 쪽 꺼내기는 쓸모없다.
            if (sides_source_ != nullptr)
                sides_source_->request_cancellation();
            run_effects(controller_.show(*loaded, wanted->repository_path));
            return;
        }
        // 적용 뒤 고쳐 낸 요청(추적하게 된 파일)으로 다시 읽은 diff다. 화면이 원하는 요청은 다음 상태 snapshot이 와야 따라오는데,
        // 그 refresh가 실패하거나 늦으면 기다리는 동안 웹뷰 단추가 잠긴 채 남는다. 같은 파일의 새 내용이므로 곧바로 보인다.
        // `shown_target_`은 화면이 원하는 요청 그대로 두어야 위의 같은 대상 판정이 맞는다.
        if (loaded != nullptr && same_target && reload_request_.has_value() && loaded->request == *reload_request_)
        {
            if (shown_serial_ == serial)
                return;
            shown_serial_ = serial;
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
                    notice(reason.empty() ? tr(u8"diff_action.apply_failed") : tr(u8"diff_action.apply_failed_reason", { { u8"reason", reason } }), true);
                }
                return;
            }
            // 적용이 됐다. 같은 파일의 diff를 다시 읽는다. 결과가 오면 `sync`가 다시 보인다.
            if (shown_target_.has_value() == false)
                return;
            // 앞서 고친 요청이 있으면 그것에서 시작한다. 화면이 아직 따라오지 않은 채 줄을 버리면(index에 더하지 않는 적용)
            // 옛 요청으로 돌아가 다시 파일 전체를 `/dev/null`과 비교하게 된다.
            diff_file_request reload { reload_request_.value_or(shown_target_->request) };
            // 추적하지 않는 파일을 index에 올렸으면 이제 추적 중이다. 그대로 `/dev/null`과 비교하면 다시 파일 전체가 보이고
            // 남은 줄의 stage가 생성 patch가 되어 실패한다. 상태 snapshot은 이 뒤의 refresh가 가져오므로 기다리지 않고 여기서 고친다.
            // `shown_target_`은 그대로 둔다. 바꾸면 `sync`가 화면이 원하는 옛 요청과 맞지 않는다고 보고 지운다.
            // 고친 요청은 기억해 두어 그 결과가 오면 `sync`가 보이게 한다 (화면 요청이 따라오기 전에도 단추가 풀린다).
            if (reload.untracked && applied_to_index_)
                reload.untracked = false;
            if (reload != shown_target_->request)
                reload_request_ = reload;
            pending_reload_ = ports_.submit != nullptr ? ports_.submit(shown_target_->repository_id, std::move(reload)) : 0;
            if (pending_reload_ == 0)
                controller_.action_failed();
            return;
        }
        if (id == pending_reload_)
        {
            pending_reload_ = 0;
            // 다시 읽지 못하면 웹뷰의 단추가 잠긴 채 남으므로 풀어 준다. 성공이면 `sync`가 곧 새 diff를 보인다
            // (요청을 고쳐 냈으면 `reload_request_`로 맞춘다).
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
            else if (const auto* const commit { std::get_if<open_commit_effect>(&effect) }; commit != nullptr)
            {
                if (ports_.open_commit != nullptr && shown_blame_.has_value())
                    ports_.open_commit(shown_blame_->repository_id, commit->sha);
            }
            else if (const auto* const revision { std::get_if<blame_revision_effect>(&effect) }; revision != nullptr)
            {
                if (ports_.open_blame != nullptr && shown_blame_.has_value())
                    ports_.open_blame(shown_blame_->repository_id, revision->request);
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
        applied_to_index_ = effect.request.cached && effect.request.reverse == false;
        pending_apply_ = ports_.submit(shown_target_->repository_id, effect.request);
        if (pending_apply_ == 0)
        {
            controller_.action_failed();
            notice(tr(u8"diff_action.apply_submit_failed"), true);
        }
    }

    void diff_view_coordinator::run_prepare_sides(const prepare_sides_effect& effect)
    {
        const std::shared_ptr<process_cancellation_source> source { replace_source(sides_source_) };
        operation_request request {};
        request.repository_key = std::string { diff_view_lane_key };
        request.job = [gateway = ports_.gateway, plan = effect.plan, write_files = effect.write_files, ticket = effect.ticket, source, previous = preview_sides_, post = ports_.sides_ready](const operation_context& context) {
            const process_cancellation_registration link { link_shutdown(context, source) };
            // 새 미리보기를 준비할 때는 앞의 미리보기가 더는 보이지 않는다. 외부 도구용 사본은 따로 꺼내므로 여기에 없다.
            if (gateway != nullptr)
            {
                gateway->release(*previous);
                *previous = {};
            }
            diff_sides_ready_intent message {};
            message.ticket = ticket;
            if (gateway == nullptr || source->token().cancelled())
                message.sides.error = tr_narrow(u8"diff_action.cancelled");
            else
                message.sides = gateway->extract_cancellable(plan, write_files, source->token());
            // 늦게 와 버려지는 결과도 폴더는 만들었으므로 다음 준비 때 함께 놓아준다.
            if (write_files)
                *previous = message.sides;
            if (post != nullptr)
                post(std::move(message));
        };
        if (ports_.schedule == nullptr || ports_.schedule(std::move(request)) == false)
        {
            // 줄에 세우지 못했다(종료 중). 두 쪽을 꺼내지 못한 것으로 알려 바이너리 카드로 물러서게 한다.
            diff_sides_ready_intent failed {};
            failed.ticket = effect.ticket;
            failed.sides.error = tr_narrow(u8"diff_action.schedule_failed");
            controller_.sides_prepared(failed.ticket, failed.sides);
        }
    }

    void diff_view_coordinator::open_file_app(external_file_app app)
    {
        if (shown_target_.has_value() == false || controller_.content() == diff_content::empty)
        {
            notice(tr(u8"diff_action.retry_after_load"), true);
            return;
        }
        run_open_external({ controller_.make_plan() }, std::move(app));
    }

    void diff_view_coordinator::open_folder_app(external_file_app app, std::u8string folder)
    {
        if (folder.empty())
            return;
        run_open_external({}, std::move(app), std::move(folder));
    }

    void diff_view_coordinator::open_external(diff_side_plan plan)
    {
        if (plan.repository_path.empty() || plan.display_path().empty())
            return;
        run_open_external({ std::move(plan) });
    }

    void diff_view_coordinator::run_open_external(const open_external_effect& effect, std::optional<external_file_app> app, std::u8string folder)
    {
        const std::shared_ptr<process_cancellation_source> source { replace_source(external_source_) };
        external_open_request open {};
        open.plan = effect.plan;
        open.file_app = std::move(app);
        open.folder = std::move(folder);
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
                message.result = { external_open_status::launch_failed, {}, tr_narrow(u8"diff_action.cancelled") };
            else
                message.result = opener->open(open);
            if (post != nullptr)
                post(std::move(message));
        };
        if (ports_.schedule == nullptr || ports_.schedule(std::move(request)) == false)
            notice(tr(u8"external_diff.open_schedule_failed"), true);
    }

    diff_notice describe_external_open(const external_open_result& result)
    {
        const std::u8string detail { to_u8(result.detail) };
        switch (result.status)
        {
        case external_open_status::launched_tool:
            return { tr(u8"external_diff.launched_tool", { { u8"tool", result.tool } }), false };
        case external_open_status::launched_difftool:
            return { tr(u8"external_diff.launched_difftool"), false };
        case external_open_status::no_tool:
            return { tr(u8"external_diff.no_tool"), true };
        case external_open_status::extraction_failed:
            return { detail.empty() ? tr(u8"external_diff.extraction_failed") : tr(u8"external_diff.extraction_failed_detail", { { u8"detail", detail } }), true };
        case external_open_status::invalid_template:
            return { detail.empty() ? tr(u8"external_diff.invalid_template") : tr(u8"external_diff.invalid_template_detail", { { u8"detail", detail } }), true };
        case external_open_status::launch_failed: {
            if (result.tool.empty())
                return { detail.empty() ? tr(u8"external_diff.launch_failed") : tr(u8"external_diff.launch_failed_detail", { { u8"detail", detail } }), true };
            if (detail.empty())
                return { tr(u8"external_diff.launch_failed_tool", { { u8"tool", result.tool } }), true };
            return { tr(u8"external_diff.launch_failed_tool_detail", { { u8"tool", result.tool }, { u8"detail", detail } }), true };
        }
        }
        return { tr(u8"external_diff.open_failed"), true };
    }
} // namespace cyella
