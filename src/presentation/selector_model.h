#pragma once

#include "application/repository_tabs.h"
#include "domain/app_settings.h"
#include "domain/ref.h"
#include "presentation/menu_model.h"
#include "presentation/start_page_view.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 툴바의 저장소·브랜치 선택 상자가 펼치는 목록의 순수 모델이다. 어느 항목이 보이고 무엇을 고를 수 있는지만 정하고, 고른 키를
    // 실행하는 일(탭 바꾸기, 체크아웃)은 controller가 맡는다. 항목은 우클릭 메뉴와 같은 `menu_entry`다.

    enum class selector_kind
    {
        repository,
        branch,
        file_app,
    };

    // 한 번에 보이는 항목 수의 상한이다. 넘으면 "입력으로 좁히세요" 줄이 서고 나머지는 거르기로 찾는다.
    inline constexpr std::size_t selector_visible_limit { 12 };

    // 항목 키의 접두다.
    inline constexpr std::u8string_view selector_tab_key_prefix { u8"tab:" };
    inline constexpr std::u8string_view selector_recent_key_prefix { u8"recent:" };
    inline constexpr std::u8string_view selector_branch_key_prefix { u8"branch:" };
    inline constexpr std::u8string_view selector_open_folder_key { u8"open_folder" };
    inline constexpr std::u8string_view selector_more_key { u8"more" };
    inline constexpr std::u8string_view selector_empty_key { u8"none" };

    struct selector_list
    {
        std::vector<menu_entry> entries {};
        // 거르기를 통과한 항목 수다 (보이는 수를 줄이기 전).
        std::size_t matched { 0 };
        // 상한 때문에 감춘 항목 수다.
        std::size_t hidden { 0 };
    };

    // 저장소 선택 상자의 후보 하나다.
    struct repository_choice
    {
        // 열린 탭이면 탭 id다. 최근 목록의 항목이면 0이다.
        repository_tab_id tab { 0 };
        std::u8string name {};
        std::u8string path {};
        // 지금 보고 있는 탭이다.
        bool current { false };
        // 경로가 디스크에 있는가. 아니면 고를 수 없다.
        bool available { true };
    };

    // 열린 저장소 탭들(탭 순서)과 그 밖의 최근 저장소(고정이 앞)로 후보를 모은다. 이미 탭으로 열린 저장소는 최근 목록에 다시 넣지 않는다.
    // `exists`는 최근 항목에만 부른다 (열린 탭은 열려 있으니 있다고 본다).
    [[nodiscard]] std::vector<repository_choice> make_repository_choices(
        const repository_tabs& tabs, const app_settings& settings, const repository_exists_function& exists, repository_tab_id selected_tab);

    // 거르기 글(`query`)이 이름이나 경로에 들어 있는 후보로 목록을 짓는다. 열린 탭이 앞이고 최근이 뒤이며, 둘 사이에 구분선이 선다.
    // 목록 끝에는 폴더를 골라 여는 항목이 늘 있다. 후보가 하나도 맞지 않으면 꺼진 "일치하는 저장소 없음" 줄이 선다.
    [[nodiscard]] selector_list build_repository_selector(const std::vector<repository_choice>& choices, std::u8string_view query);

    // 로컬 브랜치를 이름으로 거른 목록이다. 현재 브랜치는 체크 표시가 붙고 고를 수 없으며(이미 거기 있다), 나머지는 체크아웃 대상이다.
    // 머리가 분리되었으면(`detached`) 현재 브랜치가 없다. 순서는 `refs`의 순서(이름순)다.
    [[nodiscard]] selector_list build_branch_selector(const std::vector<ref>& refs, std::u8string_view query, bool detached);

    // 거르기 일치: ASCII 대소문자를 무시하는 부분 문자열이고 `/`와 `\`는 같게 본다. 빈 글은 모두와 맞는다.
    [[nodiscard]] bool selector_matches(std::u8string_view text, std::u8string_view query);
} // namespace cyella
