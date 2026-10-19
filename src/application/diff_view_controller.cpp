#include "application/diff_view_controller.h"

#include "domain/patch_builder.h"

#include <utility>

namespace cyella {
    namespace {
        patch_direction direction_of(const diff_hunk_command command) noexcept
        {
            switch (command)
            {
            case diff_hunk_command::stage:
                return patch_direction::stage;
            case diff_hunk_command::unstage:
                return patch_direction::unstage;
            case diff_hunk_command::discard:
                return patch_direction::discard;
            }
            return patch_direction::stage;
        }

        // 요청한 경로의 파일을 고른다. 이름이 바뀐 파일은 새 경로로도 맞춘다.
        const diff_file* choose_file(const file_diff& diff)
        {
            for (const diff_file& file : diff.files)
                if (file.display_path() == diff.request.path || file.old_path == diff.request.path)
                    return &file;
            return diff.files.empty() ? nullptr : &diff.files.front();
        }
    } // namespace

    diff_view_controller::diff_view_controller(diff_view_options options)
        : options_ { options }
    {
        state_.toolbar = options.toolbar;
    }

    void diff_view_controller::set_layout(const diff_layout layout)
    {
        if (state_.layout == layout)
            return;
        state_.layout = layout;
        layout_dirty_ = true;
    }

    void diff_view_controller::set_theme(diff_theme theme)
    {
        if (state_.theme == theme)
            return;
        state_.theme = std::move(theme);
        theme_dirty_ = true;
    }

    void diff_view_controller::set_image_mode(const image_compare_mode mode)
    {
        if (state_.image_mode == mode)
            return;
        state_.image_mode = mode;
        image_mode_dirty_ = true;
    }

    diff_side_plan diff_view_controller::make_plan() const
    {
        diff_side_plan plan {};
        plan.repository_path = repository_path_;
        plan.view = view_;
        plan.sha = sha_;
        plan.old_path = file_.old_path;
        plan.new_path = file_.new_path;
        plan.status = file_.status;
        plan.untracked = untracked_;
        return plan;
    }

    std::vector<diff_view_effect> diff_view_controller::show(const file_diff& diff, std::u8string repository_path)
    {
        std::vector<diff_view_effect> effects {};

        // 같은 파일을 다시 읽은 것이면(stage 뒤 새로 읽음) 전체 보기 선택을 유지한다.
        const diff_file* const chosen { choose_file(diff) };
        diff_file next {};
        if (chosen != nullptr)
        {
            next = *chosen;
        }
        else
        {
            // 변경이 없다. 요청한 경로만 가진 빈 diff를 보인다.
            next.old_path = next.new_path = diff.request.path;
        }
        const bool same_file { kind_ != content_kind::empty && view_ == diff.request.view && sha_ == diff.request.sha && file_.display_path() == next.display_path() };

        file_ = std::move(next);
        view_ = diff.request.view;
        sha_ = diff.request.sha;
        untracked_ = diff.request.untracked;
        repository_path_ = std::move(repository_path);
        sides_ = {};
        if (same_file == false)
            full_requested_ = false;
        awaiting_refresh_ = false;
        content_dirty_ = true;
        ++ticket_;

        if (file_.is_binary)
        {
            const bool image { is_image_path(file_.display_path()) };
            kind_ = image ? content_kind::pending_image : content_kind::pending_binary;
            effects.emplace_back(prepare_sides_effect { ticket_, make_plan(), image });
        }
        else
        {
            kind_ = content_kind::text;
        }
        return effects;
    }

    void diff_view_controller::clear()
    {
        kind_ = content_kind::empty;
        file_ = {};
        sides_ = {};
        full_requested_ = false;
        awaiting_refresh_ = false;
        content_dirty_ = true;
        ++ticket_;
    }

    void diff_view_controller::sides_prepared(const std::uint64_t ticket, const extracted_sides& sides)
    {
        if (ticket != ticket_ || (kind_ != content_kind::pending_image && kind_ != content_kind::pending_binary))
            return;
        sides_ = sides;
        // 두 쪽을 꺼내지 못했으면 그림을 보일 수 없으니 바이너리 카드로 물러선다.
        kind_ = kind_ == content_kind::pending_image && sides.ok ? content_kind::image : content_kind::binary;
        content_dirty_ = true;
    }

    void diff_view_controller::action_failed()
    {
        awaiting_refresh_ = false;
        // 웹뷰는 동작을 보낸 뒤 단추를 잠가 두므로 같은 diff를 다시 보내 잠금을 푼다.
        content_dirty_ = true;
    }

    void diff_view_controller::page_reloading() noexcept
    {
        page_ready_ = false;
    }

    diff_content diff_view_controller::content() const noexcept
    {
        switch (kind_)
        {
        case content_kind::empty:
            return diff_content::empty;
        case content_kind::pending_image:
        case content_kind::pending_binary:
            return diff_content::loading;
        case content_kind::text:
            return diff_content::text;
        case content_kind::image:
            return diff_content::image;
        case content_kind::binary:
            return diff_content::binary;
        }
        return diff_content::empty;
    }

    bool diff_view_controller::truncated() const noexcept
    {
        return kind_ == content_kind::text && full_requested_ == false && options_.line_limit != 0 && count_diff_lines(file_) > options_.line_limit;
    }

    bool diff_view_controller::can_stage() const noexcept
    {
        return kind_ == content_kind::text && view_ == diff_view::unstaged && truncated() == false && awaiting_refresh_ == false;
    }

    bool diff_view_controller::can_unstage() const noexcept
    {
        return kind_ == content_kind::text && view_ == diff_view::staged && truncated() == false && awaiting_refresh_ == false;
    }

    bool diff_view_controller::can_discard() const noexcept
    {
        // 추적하지 않는 파일의 줄을 되돌리면 파일을 지우는 일이 되므로 휴지통으로 보내는 `trash_files_request`가 맡는다.
        return can_stage() && untracked_ == false;
    }

    bool diff_view_controller::has_actionable_content() const noexcept
    {
        return kind_ == content_kind::text || kind_ == content_kind::image || kind_ == content_kind::binary;
    }

    std::string diff_view_controller::build_content_message()
    {
        content_seq_ = ++next_seq_;
        switch (kind_)
        {
        case content_kind::empty:
        case content_kind::pending_image:
        case content_kind::pending_binary:
            return serialize_clear(content_seq_, state_);
        case content_kind::text: {
            show_diff_message message {};
            message.seq = content_seq_;
            message.view = view_;
            message.state = state_;
            message.can_stage = can_stage();
            message.can_unstage = can_unstage();
            message.can_discard = can_discard();
            message.file = &file_;
            message.line_limit = full_requested_ ? 0 : options_.line_limit;
            return serialize_show_diff(message);
        }
        case content_kind::image: {
            show_image_message message {};
            message.seq = content_seq_;
            message.state = state_;
            message.path = file_.display_path();
            message.status = file_.status;
            if (sides_.left.present)
                message.left_url = make_file_url(sides_.left.file);
            if (sides_.right.present)
                message.right_url = make_file_url(sides_.right.file);
            message.left_size = sides_.left.size;
            message.right_size = sides_.right.size;
            return serialize_show_image(message);
        }
        case content_kind::binary: {
            show_binary_message message {};
            message.seq = content_seq_;
            message.state = state_;
            message.path = file_.display_path();
            message.status = file_.status;
            message.old_size = sides_.left.size;
            message.new_size = sides_.right.size;
            return serialize_show_binary(message);
        }
        }
        return serialize_clear(content_seq_, state_);
    }

    bool diff_view_controller::poll()
    {
        if (page_ready_ == false)
            return false;
        const int small_changes { (layout_dirty_ ? 1 : 0) + (theme_dirty_ ? 1 : 0) + (image_mode_dirty_ ? 1 : 0) };
        if (content_dirty_ == false && small_changes == 0)
            return false;

        if (content_dirty_ || small_changes > 1)
            latest_message_ = build_content_message();
        else if (layout_dirty_)
            latest_message_ = serialize_set_layout(++next_seq_, state_.layout);
        else if (theme_dirty_)
            latest_message_ = serialize_set_theme(++next_seq_, state_.theme);
        else
            latest_message_ = serialize_set_image_mode(++next_seq_, state_.image_mode);

        content_dirty_ = false;
        layout_dirty_ = false;
        theme_dirty_ = false;
        image_mode_dirty_ = false;
        ++post_revision_;
        return true;
    }

    web_message_result diff_view_controller::reject(std::string reason) const
    {
        return { false, std::move(reason), {} };
    }

    bool diff_view_controller::command_allowed(const diff_hunk_command command) const noexcept
    {
        switch (command)
        {
        case diff_hunk_command::stage:
            return can_stage();
        case diff_hunk_command::unstage:
            return can_unstage();
        case diff_hunk_command::discard:
            return can_discard();
        }
        return false;
    }

    web_message_result diff_view_controller::build_patch_effect(const diff_hunk_command command, const std::size_t hunk, std::optional<std::vector<std::size_t>> lines)
    {
        if (command_allowed(command) == false)
            return reject("이 보기에서는 허용하지 않는 동작입니다.");
        if (hunk >= file_.hunks.size())
            return reject("hunk 번호가 범위 밖입니다.");

        const patch_direction direction { direction_of(command) };
        std::optional<std::string> patch { lines.has_value() ? build_line_patch(file_, hunk, *lines, direction) : build_hunk_patch(file_, hunk, direction) };
        if (patch.has_value() == false)
            return reject("고른 줄로 patch를 만들 수 없습니다.");

        const patch_apply_mode mode { patch_apply_mode_for(direction) };
        apply_patch_effect effect {};
        effect.request.patch = std::move(*patch);
        effect.request.cached = mode.cached;
        effect.request.reverse = mode.reverse;
        effect.request.recount = true;
        effect.command = command;
        effect.hunk = hunk;
        effect.whole_hunk = lines.has_value() == false;

        awaiting_refresh_ = true;
        web_message_result result {};
        result.accepted = true;
        result.effects.emplace_back(std::move(effect));
        return result;
    }

    web_message_result diff_view_controller::handle_hunk_action(const hunk_action& action)
    {
        return build_patch_effect(action.command, action.hunk, std::nullopt);
    }

    web_message_result diff_view_controller::handle_lines_action(const lines_action& action)
    {
        if (kind_ != content_kind::text || action.hunk >= file_.hunks.size())
            return reject("hunk 번호가 범위 밖입니다.");
        std::optional<std::vector<std::size_t>> lines { expand_line_ranges(file_.hunks[action.hunk], action.ranges) };
        if (lines.has_value() == false)
            return reject("줄 범위가 hunk 밖입니다.");
        if (lines->empty())
            return reject("고른 구간에 바뀐 줄이 없습니다.");
        return build_patch_effect(action.command, action.hunk, std::move(lines));
    }

    web_message_result diff_view_controller::handle_web_message(const std::string_view message)
    {
        diff_action_parse_result parsed { parse_diff_view_action(message) };
        if (parsed.action.has_value() == false)
            return reject(std::move(parsed.error));

        // 내용과 무관한 동작들이다. seq를 보지 않는다.
        if (const auto* const ready { std::get_if<ready_action>(&*parsed.action) }; ready != nullptr)
        {
            if (ready->protocol != diff_view_protocol_version)
                return reject("지원하지 않는 프로토콜 판입니다.");
            page_ready_ = true;
            content_dirty_ = true;
            return { true, {}, {} };
        }
        if (const auto* const layout { std::get_if<layout_changed_action>(&*parsed.action) }; layout != nullptr)
        {
            state_.layout = layout->layout;
            return { true, {}, {} };
        }
        if (const auto* const mode { std::get_if<image_mode_changed_action>(&*parsed.action) }; mode != nullptr)
        {
            state_.image_mode = mode->mode;
            return { true, {}, {} };
        }

        // 나머지는 보고 있는 diff에 대한 동작이다. 그 diff를 실어 온 메시지의 seq여야 한다.
        const auto sequence_of = [](const auto& action) -> std::uint64_t {
            if constexpr (requires { action.seq; })
                return action.seq;
            else
                return 0;
        };
        const std::uint64_t seq = std::visit(sequence_of, *parsed.action);
        if (seq != content_seq_)
            return reject("이미 바뀐 diff에 대한 동작입니다.");

        if (const auto* const hunk { std::get_if<hunk_action>(&*parsed.action) }; hunk != nullptr)
            return handle_hunk_action(*hunk);
        if (const auto* const lines { std::get_if<lines_action>(&*parsed.action) }; lines != nullptr)
            return handle_lines_action(*lines);

        if (std::holds_alternative<open_external_action>(*parsed.action))
        {
            if (has_actionable_content() == false)
                return reject("열 파일이 없습니다.");
            web_message_result result {};
            result.accepted = true;
            result.effects.emplace_back(open_external_effect { make_plan() });
            return result;
        }

        if (const auto* const copy { std::get_if<copy_action>(&*parsed.action) }; copy != nullptr)
        {
            web_message_result result {};
            result.accepted = true;
            result.effects.emplace_back(copy_text_effect { copy->text });
            return result;
        }

        // show_full
        if (truncated() == false)
            return reject("잘린 diff가 아닙니다.");
        full_requested_ = true;
        content_dirty_ = true;
        return { true, {}, {} };
    }
} // namespace cyella
