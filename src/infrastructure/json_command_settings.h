#pragma once

#include "application/app_settings_store.h"
#include "domain/app_settings.h"

#include <nlohmann/json_fwd.hpp>

namespace cyella {
    // 프로필·단축키·명령 기록 키(`profiles`·`profile_rules`·`profile_assignments`·`shortcuts`·`command_history`)의 읽기·쓰기다.
    // `json_app_settings_store`가 같은 파일을 읽고 쓰는 길에서 한 번씩 부른다. 다른 설정 키와 부딪치지 않게 따로 둔 파일이고,
    // 알 수 없는 키는 호출한 쪽이 들고 있는 원본 위에 덮어쓰므로 그대로 남는다.

    // 틀린 항목은 경고를 남기고 건너뛴다 (프로필 이름 중복, 이름이 없는 줄, 없는 프로필을 가리키는 규칙).
    void parse_command_settings(const nlohmann::ordered_json& root, app_settings_load_result& result);
    // 값이 있는 키만 쓴다. 비어 있으면 키를 지워 쓰지 않는 사용자의 파일을 짧게 둔다.
    void serialize_command_settings(const app_settings& settings, nlohmann::ordered_json& root);
} // namespace cyella
