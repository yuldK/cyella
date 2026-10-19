#pragma once

#include "application/app_settings_store.h"
#include "domain/app_settings.h"
#include "domain/diagnostic.h"

#include <functional>
#include <string>
#include <vector>

namespace cyella {
    // 메모리의 앱 설정과 파일을 이어 주는 소유자다. 한 스레드(logic 스레드)에서만 쓴다.
    //
    // 저장이 실패해도 메모리 상태는 그대로 유지한다. 디스크가 잠깐 막혔다고 사용자가 방금 한
    // 일(저장소 열기, 고정)이 화면에서 사라지면 안 되기 때문이다. 실패는 진단 하나로 알리고,
    // 다음 변경이나 `save()`가 그때까지의 상태를 한꺼번에 다시 쓴다.
    class app_settings_service
    {
    public:
        app_settings_service(app_settings_store& store, std::u8string path);

        // 파일에서 읽어 메모리 상태를 바꾼다. 어떤 경우에도 쓸 수 있는 상태가 된다.
        std::vector<diagnostic> load();
        // 변경을 메모리에 적용하고 곧바로 저장한다. 저장이 실패하면 진단 하나를 돌려준다.
        std::vector<diagnostic> update(const std::function<void(app_settings&)>& change);
        // 지금 상태를 저장한다.
        std::vector<diagnostic> save();

        [[nodiscard]] const app_settings& settings() const noexcept;
        // 마지막 저장이 실패해 디스크가 메모리보다 뒤처져 있다.
        [[nodiscard]] bool has_unsaved_changes() const noexcept;
        [[nodiscard]] const std::u8string& path() const noexcept;

    private:
        app_settings_store* store_ { nullptr };
        std::u8string path_ {};
        app_settings settings_ {};
        std::u8string shadow_source_json_ {};
        bool unsaved_ { false };
    };
} // namespace cyella
