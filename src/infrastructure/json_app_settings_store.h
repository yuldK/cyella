#pragma once

#include "application/app_settings_store.h"
#include "application/settings_file_system.h"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace cyella {
    // 원본 JSON 위에 값만 덮어써 알 수 없는 키를 보존한다. shadow가 비어 있거나 object가
    // 아니면 새 object에서 시작한다. 알려진 키는 항상 모두 쓴다 (값이 파일에 그대로 보여
    // 사용자가 직접 고치기도 쉽다). test가 직접 부를 수 있도록 store와 분리해 둔다.
    [[nodiscard]] std::u8string serialize_app_settings_json(const app_settings& settings, std::u8string_view shadow_source_json);

    // 필드 단위 오류는 진단으로 알리고 읽을 수 있는 항목만 남긴다. JSON object가 아니면
    // 기본값과 `corrupt`를 돌려준다. `schema_version`이 현재보다 크면 경고 하나를 남기고
    // 아는 필드를 최선으로 읽으며 그 버전 번호를 지킨다.
    [[nodiscard]] app_settings_load_result parse_app_settings_json(std::u8string_view source_json);

    class json_app_settings_store final : public app_settings_store
    {
    public:
        using clock_function = std::function<std::chrono::system_clock::time_point()>;

        // `clock`은 깨진 파일의 백업 이름에 넣는 시각의 출처다. 기본은 시스템 시계다.
        explicit json_app_settings_store(settings_file_system& file_system, clock_function clock = {}) noexcept;

        // 읽을 수 없는 JSON이면 파일을 `<이름>.bad-<UTC 시각>`으로 옮겨 두고 기본값으로 시작한다.
        // 옮긴 것은 다음 저장이 깨진 파일을 덮어써 잃지 않게 하려는 것이다.
        [[nodiscard]] app_settings_load_result load(std::u8string_view path) noexcept override;
        // 실패하면 진단 하나를 돌려주고 호출한 쪽의 메모리 상태는 건드리지 않는다.
        [[nodiscard]] app_settings_save_result save(std::u8string_view path, const app_settings& settings, std::u8string_view shadow_source_json) noexcept override;

    private:
        settings_file_system* file_system_ { nullptr };
        clock_function clock_ {};
    };

    // 백업 파일 이름에 붙는 시각이다. `20261020T093012Z` 모양이라 파일 이름에 쓸 수 없는
    // `:`가 없다.
    [[nodiscard]] std::u8string format_backup_timestamp(std::chrono::system_clock::time_point time);
} // namespace cyella
