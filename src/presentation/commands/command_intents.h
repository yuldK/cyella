#pragma once

#include <string>

namespace cyella {
    // 명령 레지스트리의 명령을 실행하라는 화면 메시지다. 단축키(`cyella_policy`)와 팔레트가 같은 메시지를 낸다.
    // 폴더 대화 상자처럼 UI thread가 실행해야 하는 명령(`repo.open_folder`·`repo.init`)은 이 메시지가 아니라 UI 명령 액션을 낸다
    // (`ui::action_for_command`).
    struct run_command_intent
    {
        std::string id {};
    };
} // namespace cyella
