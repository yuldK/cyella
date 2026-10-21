#include "application/app_settings_service.h"

#include "domain/text_catalog.h"

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
        read_only_ = result.read_failed;
        read_error_.reset();
        if (read_only_ && result.diagnostics.empty() == false)
            read_error_ = result.diagnostics.front().native_error;
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
        if (read_only_)
        {
            // 읽지 못한 파일이 사용자의 진짜 설정일 수 있다. 쓰지 않고 메모리만 앞서 둔다.
            unsaved_ = true;
            diagnostic skipped {};
            skipped.code = diagnostic_code::app_settings_write_failed;
            skipped.severity = diagnostic_severity::warning;
            skipped.message = tr(u8"app.settings_save_skipped_read_only");
            skipped.native_error = read_error_;
            std::vector<diagnostic> diagnostics {};
            diagnostics.push_back(std::move(skipped));
            return diagnostics;
        }

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

    bool app_settings_service::is_read_only() const noexcept
    {
        return read_only_;
    }

    const std::u8string& app_settings_service::path() const noexcept
    {
        return path_;
    }
} // namespace cyella
