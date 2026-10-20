#pragma once

#include "domain/app_settings.h"
#include "domain/ref.h"
#include "presentation/commands/command_registry.h"
#include "presentation/menu_model.h"
#include "presentation/selector_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 명령 팔레트가 보이는 목록의 순수 모델이다. 어떤 항목이 보이고 어떤 순서인지만 정하고, 항목을 실행하는 일은 controller가 맡는다.

    // 팔레트가 지금 고르게 하는 것이다. 명령 하나를 실행하면 목록이 닫히지만, "브랜치로 이동…" 같은 명령은 모드를 바꿔 이어서 고르게 한다.
    enum class palette_mode : std::uint8_t
    {
        commands,
        branches,
        repositories,
        profiles,
    };

    // 항목 키의 접두다. controller가 키로 무엇을 할지 되찾는다.
    inline constexpr std::u8string_view palette_command_key_prefix { u8"cmd:" };
    inline constexpr std::u8string_view palette_branch_key_prefix { u8"branch:" };
    inline constexpr std::u8string_view palette_remote_branch_key_prefix { u8"remote:" };
    inline constexpr std::u8string_view palette_tab_key_prefix { u8"tab:" };
    inline constexpr std::u8string_view palette_recent_key_prefix { u8"recent:" };
    inline constexpr std::u8string_view palette_profile_key_prefix { u8"profile:" };
    inline constexpr std::u8string_view palette_menu_key_prefix { u8"menu:" };
    inline constexpr std::u8string_view palette_manage_profiles_key { u8"manage_profiles" };
    inline constexpr std::u8string_view palette_more_key { u8"more" };
    inline constexpr std::u8string_view palette_empty_key { u8"none" };

    // 한 번에 보이는 항목 수다. 넘으면 "입력으로 좁히세요" 줄이 서고 나머지는 검색으로 찾는다 (툴바 선택 상자와 같은 규칙).
    inline constexpr std::size_t palette_visible_limit { 14 };

    struct palette_item
    {
        std::u8string key {};
        std::u8string title {};
        // 오른쪽에 흐리게 보이는 글이다 (단축키, 경로, 현재 표시).
        std::u8string detail {};
        // 영문 검색어다.
        std::u8string keywords {};
        bool enabled { true };
        // 실행하면 UI thread의 폴더 대화 상자가 뜬다 (`ui_command_id`). 이 항목만 메시지가 아니라 UI 명령을 낸다.
        bool ui_thread_command { false };
    };

    struct palette_context
    {
        palette_mode mode { palette_mode::commands };
        const app_settings* settings { nullptr };
        command_availability availability {};
        // 고른 항목의 우클릭 메뉴다. 제목이 없거나 항목이 없으면 메뉴 명령이 없다.
        menu_model selection_menu {};
        // 브랜치 모드: 저장소의 ref(로컬·원격 브랜치만 쓴다)와 현재 브랜치다.
        std::vector<ref> refs {};
        std::string current_branch {};
        bool detached { false };
        // 저장소 모드: 열린 탭과 최근 저장소다 (`make_repository_choices`).
        std::vector<repository_choice> repositories {};
        // 프로필 모드: 이 저장소에 지금 적용된(또는 규칙이 제안하는) 프로필 이름이다. 목록에 표시를 단다.
        std::u8string current_profile {};
        std::u8string suggested_profile {};
    };

    struct palette_list
    {
        std::vector<palette_item> items {};
        // 질의와 맞는 항목 수다 (보이는 수를 줄이기 전).
        std::size_t matched { 0 };
        std::size_t hidden { 0 };
    };

    // 모드에 맞는 후보를 모아 질의로 거르고 순위를 매긴다 (보이는 수 상한은 아직 적용하지 않는다).
    //  - 명령 모드: 레지스트리의 명령(지금 할 수 있는 것만)과 고른 항목의 메뉴 항목이다. 질의가 비면 최근에 쓴 명령이 앞이고 나머지는 레지스트리 순서다.
    //    질의가 있으면 일치 점수에 최근 사용 가산점을 더해 높은 순이다.
    //  - 나머지 모드: 일치 점수 순이고, 질의가 비면 원래 순서다.
    [[nodiscard]] std::vector<palette_item> rank_palette_items(const palette_context& context, std::u8string_view query);

    // 보이는 수를 상한으로 줄이고 넘친 수를 센다. 일치하는 것이 없으면 꺼진 안내 줄 하나가 선다.
    [[nodiscard]] palette_list build_palette_list(const palette_context& context, std::u8string_view query);

    // 모드의 제목과 입력 칸 안내 글이다.
    [[nodiscard]] std::u8string_view palette_mode_title(palette_mode mode) noexcept;
    [[nodiscard]] std::u8string_view palette_mode_placeholder(palette_mode mode) noexcept;
} // namespace cyella
