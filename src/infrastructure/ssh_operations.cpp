#include "infrastructure/git_operation_context.h"
#include "infrastructure/ssh_tools.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

// 환경 설정의 SSH 도구(키 만들기·연결 시험)다. git이 아니라 ssh-keygen·ssh를 부르지만, 질문이 askpass로 와서
// 대화 상자를 거쳐야 하므로 git operation과 같은 길(작업 로그·needs_input·취소)에 태운다.
namespace cyella {
    namespace {
        // 두 스트림의 줄을 모은다. 분류와 실패 상세에 쓴다. 마스킹된 텍스트라 비밀이 섞이지 않는다.
        class text_sink final : public process_output_sink
        {
        public:
            void on_record(const process_output_record& record) override
            {
                std::u8string& target { record.stream == process_stream::standard_output ? standard_output : standard_error };
                if (target.empty() == false)
                    target.push_back(u8'\n');
                target.append(record.text);
            }

            std::u8string standard_output {};
            std::u8string standard_error {};
        };

        // 도우미가 질문을 이 operation에 묶어 보내고, 취소할 때도 이 id로 대기 질문을 찾는다.
        askpass_session session_of(const git_operation_context& context)
        {
            const std::string digits { std::to_string(context.job().id) };
            askpass_session session {};
            session.pipe_name = context.owner().options().askpass.pipe_name;
            session.token = context.owner().options().askpass.token;
            session.operation_id = std::u8string { digits.begin(), digits.end() };
            return session;
        }

        // 프로세스를 돌리고 작업 로그에 한 줄을 남긴다.
        process_result run_logged(git_operation_context& context, const process_request& request, text_sink& sink)
        {
            const process_result result { context.owner().runner().run(request, &sink, context.job().cancellation) };
            operation_command command {};
            command.command_line = result.masked_command_line;
            command.completion = result.completion;
            command.exit_code = result.exit_code;
            command.duration = result.duration;
            context.emit(operation_command_finished { std::move(command) });
            return result;
        }

        // 실행하지 못했거나 끝나지 않은 프로세스의 끝맺음이다. 정상 종료(`exited`)면 값이 없다.
        std::optional<operation_outcome> interruption_of(const process_result& result, const std::u8string_view tool)
        {
            switch (result.completion)
            {
            case process_completion::exited:
                return std::nullopt;
            case process_completion::cancelled:
                return operation_outcome::cancelled();
            case process_completion::timed_out:
                return operation_outcome::failed(failure_kind::timed_out, std::u8string { tool } + u8"이 시간 안에 끝나지 않았습니다.");
            case process_completion::start_failed:
            case process_completion::invalid_request:
            case process_completion::internal_error:
                break;
            }
            std::u8string detail {};
            for (const diagnostic& entry : result.diagnostics)
            {
                if (detail.empty() == false)
                    detail.push_back(u8'\n');
                detail.append(entry.message);
            }
            return operation_outcome::failed(failure_kind::other, std::u8string { tool } + u8"을 실행하지 못했습니다.", std::move(detail));
        }

        std::u8string keygen_error_message(const ssh_keygen_request_error error)
        {
            switch (error)
            {
            case ssh_keygen_request_error::askpass_missing:
                return u8"인증 도우미(cyella-askpass.exe)를 찾지 못해 passphrase를 물을 수 없습니다.";
            case ssh_keygen_request_error::invalid_key_path:
                return u8"키 파일 경로가 올바르지 않습니다. 절대 경로여야 합니다.";
            case ssh_keygen_request_error::invalid_comment:
                return u8"주석에 제어 문자를 쓸 수 없습니다.";
            case ssh_keygen_request_error::key_exists:
                return u8"같은 이름의 키 파일이 이미 있습니다. 다른 이름을 고르세요.";
            case ssh_keygen_request_error::none:
                break;
            }
            return u8"키를 만들 수 없습니다.";
        }
    } // namespace

    operation_outcome run_ssh_keygen(git_operation_context& context, const ssh_keygen_request& request)
    {
        const git_operation_runner_options& options { context.owner().options() };
        if (options.ssh_keygen_executable.empty())
            return operation_outcome::failed(failure_kind::invalid_request, u8"ssh-keygen을 찾지 못했습니다. OpenSSH 클라이언트나 Git for Windows를 설치하세요.");

        ssh_keygen_options keygen {};
        keygen.ssh_keygen_executable = options.ssh_keygen_executable;
        keygen.key_path = std::filesystem::path { request.key_path };
        keygen.comment = request.comment;
        keygen.askpass_helper = options.askpass_helper;
        keygen.askpass = session_of(context);
        ssh_keygen_request_result built { make_ssh_keygen_request(keygen) };
        if (built.error != ssh_keygen_request_error::none)
            return operation_outcome::failed(failure_kind::invalid_request, keygen_error_message(built.error));

        // ssh-keygen은 폴더를 만들지 않는다. 처음 키를 만드는 PC에는 ~/.ssh가 없을 수 있다.
        std::error_code directory_error {};
        std::filesystem::create_directories(keygen.key_path.parent_path(), directory_error);

        text_sink sink {};
        const process_result result { run_logged(context, built.request, sink) };
        if (const std::optional<operation_outcome> interrupted { interruption_of(result, u8"ssh-keygen") }; interrupted.has_value())
            return *interrupted;

        // 사용자가 passphrase 대화 상자를 닫았다. askpass가 답 없이 끝나도 ssh-keygen은 빈 passphrase로 키를 만들어 버리므로
        // (종료 코드 0), 성공으로 보기 전에 먼저 가른다. 사용자가 원하지 않은 passphrase 없는 키는 지운다 (이 파일들은
        // 요청을 만들 때 없던 것이다).
        if (context.job().prompt_cancelled != nullptr && context.job().prompt_cancelled->load())
        {
            std::filesystem::path public_path { keygen.key_path };
            public_path += ".pub";
            std::error_code ignored {};
            std::filesystem::remove(keygen.key_path, ignored);
            std::filesystem::remove(public_path, ignored);
            return operation_outcome::failed(failure_kind::auth_cancelled, u8"passphrase 입력을 취소해 키를 만들지 않았습니다.");
        }
        if (result.exit_code.has_value() && *result.exit_code == 0)
            return operation_outcome::succeeded(u8"키를 만들었습니다: " + keygen.key_path.u8string());
        std::u8string detail { sink.standard_error };
        if (detail.empty())
            detail = sink.standard_output;
        return operation_outcome::failed(failure_kind::other, u8"ssh-keygen이 키를 만들지 못했습니다.", std::move(detail));
    }

    operation_outcome run_ssh_connection_test(git_operation_context& context, const ssh_connection_test_request& request)
    {
        const git_operation_runner_options& options { context.owner().options() };
        if (options.ssh_executable.empty())
            return operation_outcome::failed(failure_kind::invalid_request, u8"ssh를 찾지 못했습니다. OpenSSH 클라이언트나 Git for Windows를 설치하세요.");

        ssh_connection_test_options test {};
        test.ssh_executable = options.ssh_executable;
        // 프로세스 요청은 절대 경로의 작업 디렉터리를 요구한다. 어디서 돌아도 상관없으므로 ssh가 있는 폴더를 쓴다.
        test.working_directory = parent_directory(options.ssh_executable);
        test.host = request.host;
        test.user_name = request.user_name;
        test.port = request.port;
        test.askpass_helper = options.askpass_helper;
        test.askpass = session_of(context);
        std::optional<process_request> built { make_ssh_connection_test_request(test) };
        if (built.has_value() == false)
            return operation_outcome::failed(failure_kind::invalid_request, u8"호스트나 계정 이름이 올바르지 않거나 인증 도우미가 없어 시험할 수 없습니다.");
        // passphrase나 호스트 키 확인을 사람이 답하는 시간까지 넉넉히 잡되, 끝나지 않는 연결은 끊는다.
        built->timeout = std::chrono::minutes { 2 };

        text_sink sink {};
        const process_result result { run_logged(context, *built, sink) };
        if (const std::optional<operation_outcome> interrupted { interruption_of(result, u8"ssh") }; interrupted.has_value())
            return *interrupted;

        const ssh_connection_classification classification { classify_ssh_connection_test(result.exit_code.value_or(-1), sink.standard_output, sink.standard_error) };
        // 대화 상자를 닫아 호스트 키를 확인하지 않은 것은 시험을 못 한 것이다.
        if (context.job().prompt_cancelled != nullptr && context.job().prompt_cancelled->load())
            return operation_outcome::failed(failure_kind::auth_cancelled, u8"확인 대화 상자를 닫아 시험을 중단했습니다.");
        context.emit(ssh_connection_tested { classification });
        return operation_outcome::succeeded(describe_ssh_connection(classification));
    }
} // namespace cyella
