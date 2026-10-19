#include "application/app_settings_service.h"

#include <utility>

namespace cyella {
    app_settings_service::app_settings_service(app_settings_store& store, std::u8string path)
        : store_ { &store }
        , path_ { std::move(path) }
    {}

    std::vector<diagnostic> app_settings_service::load()
    {
        app_settings_load_result result { store_->load(path_) };
        settings_ = std::move(result.settings);
        shadow_source_json_ = std::move(result.shadow_source_json);
        unsaved_ = false;
        return std::move(result.diagnostics);
    }

    std::vector<diagnostic> app_settings_service::update(const std::function<void(app_settings&)>& change)
    {
        if (change)
            change(settings_);
        return save();
    }

    std::vector<diagnostic> app_settings_service::save()
    {
        app_settings_save_result result { store_->save(path_, settings_, shadow_source_json_) };
        if (result.succeeded == false)
        {
            unsaved_ = true;
            return std::move(result.diagnostics);
        }

        // 성공한 저장의 결과가 다음 저장의 기준이다. 알 수 없는 키는 이 원본을 거쳐 계속
        // 살아남는다.
        shadow_source_json_ = std::move(result.shadow_source_json);
        unsaved_ = false;
        return {};
    }

    const app_settings& app_settings_service::settings() const noexcept
    {
        return settings_;
    }

    bool app_settings_service::has_unsaved_changes() const noexcept
    {
        return unsaved_;
    }

    const std::u8string& app_settings_service::path() const noexcept
    {
        return path_;
    }
} // namespace cyella
