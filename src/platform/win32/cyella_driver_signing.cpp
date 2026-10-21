#include "platform/win32/cyella_driver.h"

#include "domain/text_catalog.h"
#include "presentation/settings/signing_intents.h"

#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

// 환경 설정 "서명" 쪽의 설정 읽기·쓰기다. git·gpg를 띄우므로 worker가 하고 결과는 relay로 돌아온다 (SSH 화면의 `request_ssh_info`와 같은 길).
namespace cyella::win32 {
    namespace {
        // 저장소가 없을 때 git을 돌릴 폴더다. 전역 설정은 어디서 읽고 써도 같다. 사용자 폴더(`~/.ssh`의 부모)를 쓰고, 모르면 임시 폴더다.
        std::u8string fallback_directory_of(const std::filesystem::path& ssh_directory)
        {
            std::error_code error {};
            if (ssh_directory.empty() == false && ssh_directory.has_parent_path())
                if (const std::filesystem::path parent { ssh_directory.parent_path() }; std::filesystem::is_directory(parent, error))
                    return parent.u8string();
            return std::filesystem::temp_directory_path(error).u8string();
        }
    } // namespace

    void cyella_driver::request_signing_info(const std::u8string& repository_path)
    {
        signing_info_sources sources {};
        sources.runner = process_runner_.get();
        sources.git_executable = git_tool_.executable;
        sources.repository_path = repository_path;
        sources.fallback_directory = fallback_directory_of(ssh_directory_);
        sources.probe = probe_.get();
        if (scheduler_ == nullptr)
        {
            // git을 쓸 수 없어 scheduler가 없다. 읽을 설정도 없다.
            relay_.post(luil::app_message { signing_info_loaded_intent { signing_info_snapshot { .repository = repository_path.empty() == false } } });
            return;
        }
        auto source { std::make_shared<process_cancellation_source>() };
        registry_.add(*source);
        operation_request request {};
        request.repository_key = "signing-info";
        request.job = [sources = std::move(sources), source, relay = &relay_](const operation_context& context) mutable {
            const process_cancellation_registration link {
                context.shutdown.cancellable() ? context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); }) : process_cancellation_registration {}
            };
            sources.cancellation = source->token();
            // 설정은 곧바로 보이고, gpg 키 목록은 gpg-agent를 띄우느라 수 초 걸릴 수 있어 읽는 대로 한 번 더 보낸다.
            sources.read_gpg_keys = false;
            signing_info_snapshot info { collect_signing_info(sources) };
            relay->post(luil::app_message { signing_info_loaded_intent { info } });
            read_signing_gpg_keys(sources, info);
            relay->post(luil::app_message { signing_info_loaded_intent { std::move(info) } });
        };
        static_cast<void>(scheduler_->submit(std::move(request)));
    }

    void cyella_driver::write_signing_config(const bool global, const std::u8string& repository_path, std::vector<signing_change> changes)
    {
        signing_info_sources sources {};
        sources.runner = process_runner_.get();
        sources.git_executable = git_tool_.executable;
        sources.repository_path = repository_path;
        sources.fallback_directory = fallback_directory_of(ssh_directory_);
        sources.probe = probe_.get();
        if (scheduler_ == nullptr)
        {
            relay_.post(luil::app_message { signing_write_done_intent { .ok = false, .global = global, .error = tr(u8"signing_config.git_missing") } });
            return;
        }
        auto source { std::make_shared<process_cancellation_source>() };
        registry_.add(*source);
        operation_request request {};
        // 설정 쓰기끼리는 저장소(전역은 따로)마다 차례로 돈다. 설정 파일 잠금(config.lock)을 두고 서로 다투지 않게 한다.
        request.repository_key = global || repository_path.empty() ? "signing-config-global" : "signing-config:" + std::string { repository_path.begin(), repository_path.end() };
        request.job = [sources = std::move(sources), source, global, changes = std::move(changes), relay = &relay_](const operation_context& context) mutable {
            const process_cancellation_registration link {
                context.shutdown.cancellable() ? context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); }) : process_cancellation_registration {}
            };
            sources.cancellation = source->token();
            const signing_write_result result { cyella::write_signing_config(sources, global, changes) };
            relay->post(luil::app_message { signing_write_done_intent { .ok = result.ok, .global = global, .error = result.error } });
        };
        static_cast<void>(scheduler_->submit(std::move(request)));
    }
} // namespace cyella::win32
