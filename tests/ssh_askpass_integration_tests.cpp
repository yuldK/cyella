// 실제 ssh-keygen·ssh, 실제 askpass 서버와 도우미, 실제 scheduler로 환경 설정의 SSH 도구를 끝까지 돌린다.
// 키 만들기는 passphrase 질문이 operation의 `needs_input`으로 오고 그 답으로 키가 만들어지는지를, 연결 시험은 없는 호스트가
// "연결하지 못함"으로 분류되는지를 본다. 모두 `[integration]` 태그를 가지며 ssh 도구가 없으면 건너뛴다.
#include "application/git_workspace.h"
#include "application/task_scheduler.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/ssh_info.h"
#include "platform/win32/askpass_bridge.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_ssh_environment.h"
#include "platform/win32/win32_workspace_files.h"

#include "askpass_test_support.h"
#include "fake_git_runner.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace {
    using namespace cyella;
    using namespace std::chrono_literals;

    constexpr std::chrono::seconds step_timeout { 60 };

    // 실제 구성이다. job은 scheduler의 worker에서 돌고 사건은 test 스레드가 `pump_until`에서 반영한다.
    class live_ssh_workspace
    {
    public:
        explicit live_ssh_workspace(const ssh_programs& programs)
            : process_ { win32::make_process_runner() }
            , files_ { win32::make_workspace_files() }
            , bridge_ { events_.sink() }
        {
            bridge_.start(test::askpass_helper_path());
            REQUIRE(bridge_.available());

            git_operation_runner_options runner_options {};
            runner_options.askpass_helper = bridge_.helper_path();
            runner_options.askpass = bridge_.endpoint();
            runner_options.ssh_executable = programs.ssh;
            runner_options.ssh_keygen_executable = programs.ssh_keygen;
            // git은 쓰지 않지만 실행기가 경로를 요구한다.
            executor_ = std::make_unique<git_operation_runner>(*process_, u8"C:/Git/cmd/git.exe", *files_, runner_options);
            scheduler_ = std::make_unique<task_scheduler>();
            git_workspace_options workspace_options {};
            workspace_options.refresh_on_open = false;
            workspace_options.askpass = bridge_.service();
            task_scheduler* const scheduler { scheduler_.get() };
            workspace_
                = std::make_unique<git_workspace>(*executor_, [scheduler](operation_request request) { return scheduler->submit(std::move(request)); }, events_.sink(), nullptr, workspace_options);
        }

        live_ssh_workspace(const live_ssh_workspace&) = delete;
        live_ssh_workspace& operator=(const live_ssh_workspace&) = delete;

        // 실패한 test가 프롬프트에서 멈춘 프로세스를 남기지 않게 모두 정리한 뒤 닫는다.
        ~live_ssh_workspace()
        {
            for (const operation_id id : workspace_->operation_order())
            {
                const operation_status* const status { workspace_->find_operation(id) };
                if (status != nullptr && status->finished() == false)
                    static_cast<void>(workspace_->cancel(id));
            }
            bridge_.shutdown();
            scheduler_->shutdown();
        }

        [[nodiscard]] git_workspace& workspace() noexcept
        {
            return *workspace_;
        }

        void pump_until(const std::function<bool()>& condition)
        {
            const auto deadline { std::chrono::steady_clock::now() + step_timeout };
            while (std::chrono::steady_clock::now() < deadline)
            {
                for (const operation_event& event : events_.take())
                    workspace_->apply(event);
                if (condition())
                    return;
                std::this_thread::sleep_for(5ms);
            }
            FAIL("timed out waiting for the operation");
        }

        void wait_for_prompt(const operation_id id, const credential_prompt_kind kind)
        {
            pump_until([this, id, kind] {
                const operation_status* const status { workspace_->find_operation(id) };
                return status != nullptr && status->state == operation_state::needs_input && status->needs_input.has_value() && status->needs_input->prompt.kind == kind;
            });
        }

        void wait_for_finish(const operation_id id)
        {
            pump_until([this, id] {
                const operation_status* const status { workspace_->find_operation(id) };
                return status != nullptr && status->finished();
            });
        }

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace_->find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

    private:
        std::unique_ptr<process_runner> process_ {};
        std::unique_ptr<workspace_files> files_ {};
        test::event_queue events_ {};
        win32::askpass_bridge bridge_;
        std::unique_ptr<git_operation_runner> executor_ {};
        std::unique_ptr<task_scheduler> scheduler_ {};
        std::unique_ptr<git_workspace> workspace_ {};
    };

    ssh_programs find_ssh_programs()
    {
        const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
        // git 위치는 모르는 값이다. 시스템 OpenSSH만 있어도 test는 돈다.
        return locate_ssh_programs(u8"C:\\nowhere\\git.exe", win32::windows_directory(), *probe);
    }
} // namespace

TEST_CASE("ssh-keygen asks for the new passphrase through the app and writes the key pair", "[askpass][ssh][integration]")
{
    const ssh_programs programs { find_ssh_programs() };
    if (programs.found() == false)
        SKIP("OpenSSH is not installed");

    const test::temporary_directory scratch { "ssh-keygen-live" };
    const std::filesystem::path key { scratch.path() / "home" / ".ssh" / "id_live" };
    live_ssh_workspace live { programs };

    const operation_id id { live.workspace().submit_setup(ssh_keygen_request { .key_path = key.u8string(), .comment = u8"live@test" }) };
    REQUIRE(id != 0);

    // 첫 질문은 새 passphrase다. 빈 답도 유효하고 비밀로 가려서 묻는다.
    live.wait_for_prompt(id, credential_prompt_kind::ssh_new_key_passphrase);
    {
        const operation_status& status { live.status(id) };
        REQUIRE(status.needs_input.has_value());
        CHECK(status.needs_input->kind == input_kind::passphrase);
        CHECK(status.needs_input->secret);
        CHECK(status.cancellable);
        REQUIRE(live.workspace().answer_input(id, status.needs_input->prompt_id, u8"correct horse battery"));
    }

    // 확인 질문이 같은 operation의 둘째 질문으로 온다.
    live.wait_for_prompt(id, credential_prompt_kind::ssh_new_key_passphrase_confirm);
    {
        const operation_status& status { live.status(id) };
        REQUIRE(status.needs_input.has_value());
        REQUIRE(live.workspace().answer_input(id, status.needs_input->prompt_id, u8"correct horse battery"));
    }

    live.wait_for_finish(id);
    const operation_status& status { live.status(id) };
    INFO(std::string(status.result_summary.begin(), status.result_summary.end()));
    CHECK(status.state == operation_state::succeeded);
    CHECK(std::filesystem::is_regular_file(key));
    CHECK(std::filesystem::is_regular_file(key.string() + ".pub"));
    // passphrase는 명령줄과 작업 로그 어디에도 남지 않는다.
    for (const operation_command& command : status.commands)
        CHECK(command.command_line.find(u8"correct horse") == std::u8string::npos);

    // 만든 키는 환경 설정의 키 목록에 그대로 나온다.
    const std::vector<ssh_key_pair> pairs { list_ssh_key_pairs(key.parent_path()) };
    REQUIRE(pairs.size() == 1);
    CHECK(pairs[0].name == u8"id_live");
    CHECK(pairs[0].key_type == u8"ssh-ed25519");
    CHECK(pairs[0].comment == u8"live@test");
}

TEST_CASE("closing the passphrase prompt cancels key creation and leaves no key behind", "[askpass][ssh][integration]")
{
    const ssh_programs programs { find_ssh_programs() };
    if (programs.found() == false)
        SKIP("OpenSSH is not installed");

    const test::temporary_directory scratch { "ssh-keygen-cancel" };
    const std::filesystem::path key { scratch.path() / "id_cancel" };
    live_ssh_workspace live { programs };

    const operation_id id { live.workspace().submit_setup(ssh_keygen_request { .key_path = key.u8string(), .comment = u8"x" }) };
    live.wait_for_prompt(id, credential_prompt_kind::ssh_new_key_passphrase);
    REQUIRE(live.workspace().cancel_input(id, live.status(id).needs_input->prompt_id));
    live.wait_for_finish(id);

    const operation_status& status { live.status(id) };
    CHECK(status.state == operation_state::failed);
    REQUIRE(status.failure.has_value());
    CHECK(status.failure->kind == failure_kind::auth_cancelled);
    CHECK_FALSE(std::filesystem::exists(key));
}

TEST_CASE("a connection test to a host that does not exist is classified as a connection failure", "[ssh][integration]")
{
    const ssh_programs programs { find_ssh_programs() };
    if (programs.found() == false)
        SKIP("OpenSSH is not installed");

    live_ssh_workspace live { programs };
    const operation_id id { live.workspace().submit_setup(ssh_connection_test_request { .host = u8"cyella-no-such-host.invalid" }) };
    REQUIRE(id != 0);
    live.wait_for_finish(id);

    const operation_status& status { live.status(id) };
    INFO(std::string(status.result_summary.begin(), status.result_summary.end()));
    CHECK(status.state == operation_state::succeeded);
    const std::optional<ssh_connection_classification> result { live.workspace().ssh_connection_result_of(id) };
    REQUIRE(result.has_value());
    CHECK(result->result == ssh_connection_result::connection_failed);
    CHECK(status.result_summary == describe_ssh_connection(*result));
}
