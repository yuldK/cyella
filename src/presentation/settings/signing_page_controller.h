#pragma once

#include "presentation/settings/signing_intents.h"
#include "presentation/settings/signing_page_state.h"
#include "presentation/settings/signing_page_view.h"
#include "presentation/ui_state.h"

#include "luil/ui/app_message.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 환경 설정 "서명" 쪽의 상태를 소유하고 그 메시지를 처리한다. `settings_controller`가 소유하고 logic thread에서만 쓴다.
    //
    // 설정은 git이 정본이다. 읽기와 쓰기는 worker가 하고(`signing_info`), 결과는 `signing_info_loaded_intent`·`signing_write_done_intent`로 돌아온다.
    // 이 저장소(`--local`)의 설정은 바로 쓰고, 전역(`--global`)은 사용자의 실제 git 설정이라 확인을 받은 뒤에만 쓴다.
    class signing_page_controller
    {
    public:
        struct dependencies
        {
            // 서명 설정과 gpg 키 목록을 worker에게 읽게 한다. 인자는 선택한 저장소 폴더다 (없으면 비어 있다).
            std::function<void(const std::u8string& repository_path)> request_info {};
            // 설정을 worker에게 쓰게 한다. 인자는 전역 여부, 선택한 저장소 폴더, 바꿀 설정이다.
            std::function<void(bool global, const std::u8string& repository_path, std::vector<signing_change> changes)> write {};
            // 지금 선택한 탭의 저장소 폴더다. 저장소 탭이 아니면 비어 있다.
            std::function<std::u8string()> repository_path {};
            // 설정이 바뀌었다. 열린 저장소의 서명 상태와 커밋 상자의 기본값을 다시 읽게 한다 (전역이면 모든 저장소).
            std::function<void(bool global)> changed {};
        };

        explicit signing_page_controller(dependencies dependencies);

        // 쪽을 열었다. 아직 읽지 않았으면 읽는다.
        void on_page_opened();
        // 환경 설정을 닫았다. 상태를 버린다.
        void on_closed();
        // 서명 쪽의 메시지면 처리하고 true다. 쪽이 열려 있지 않을 때 늦게 온 결과는 삼킨다.
        bool handle(const luil::app_message& message, bool page_open);

        [[nodiscard]] const signing_page_state& state() const noexcept
        {
            return state_;
        }

        // 텍스트 입력 칸의 초안이다. 서명 쪽의 칸이 아니면 null이다.
        [[nodiscard]] luil::text::text_edit_state* draft(text_target target);

        [[nodiscard]] signing_page_view make_view(const ssh_info_snapshot* ssh, const file_probe* probe, const std::optional<luil::text_composition_event>& composition) const;

    private:
        void request_info();
        void adopt_info(signing_info_snapshot info);
        void fill_drafts();
        [[nodiscard]] bool global_scope() const;
        [[nodiscard]] const signing_config& scope_config() const;
        // 바꿀 설정을 낸다. 이 저장소면 바로 쓰고, 전역이면 확인을 기다린다.
        void request_change(std::vector<signing_change> changes, std::u8string summary);
        void write_now(bool global, std::vector<signing_change> changes);
        void apply_text(signing_key_id key, bool clear);

        dependencies dependencies_ {};
        signing_page_state state_ {};
    };
} // namespace cyella
