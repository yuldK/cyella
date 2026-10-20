#include "infrastructure/git_signing_operations.h"

#include "infrastructure/git_signing_commands.h"
#include "infrastructure/git_signing_parser.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cyella {
    namespace {
        std::string trim_line(std::string text)
        {
            while (text.empty() == false && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            return text;
        }

        // ssh 서명을 대조할 allowed signers 파일이 쓸 수 없는가: 설정이 없거나, 가리키는 파일이 없다.
        // `blank`는 설정이 아예 없다 (검증 명령이 빈 값으로 덮어야 한다).
        struct allowed_signers_state
        {
            bool blank { false };
            bool missing { false };
        };

        allowed_signers_state read_allowed_signers(git_operation_context& context)
        {
            git_command_spec spec {};
            spec.arguments = make_git_allowed_signers_path_args();
            spec.accepted_exit_codes = { 0, 1 };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            allowed_signers_state state {};
            if (outcome.accepted == false || outcome.exit_code() != 0)
            {
                state.blank = true;
                state.missing = true;
                return state;
            }
            const std::string path { trim_line(outcome.standard_output) };
            state.missing = path.empty() || context.owner().files().probe(to_utf8(path)) == path_kind::missing;
            return state;
        }

        bool looks_like_ssh_fingerprint(const std::u8string& key) noexcept
        {
            return key.starts_with(u8"SHA256:") || key.starts_with(u8"SHA512:");
        }

        // 요청한 커밋 중 결과가 없는 것을 `cannot_check`로 채운다. 세션은 요청한 커밋을 결과가 올 때까지 "읽는 중"으로 두므로, 실패하거나 git이 빠뜨린
        // 커밋도 화면에서 "확인할 수 없음"으로 풀려야 한다 (그렇지 않으면 배지가 "확인 중…"에 영영 머문다).
        void add_unverifiable(signatures_verified& verified, const std::vector<std::string>& requested)
        {
            for (const std::string& sha : requested)
            {
                const auto known { std::ranges::find_if(verified.signatures, [&sha](const commit_signature& signature) { return signature.sha == sha; }) };
                if (known != verified.signatures.end())
                    continue;
                commit_signature signature {};
                signature.sha = sha;
                signature.status = signature_status::cannot_check;
                verified.signatures.push_back(std::move(signature));
            }
        }

        operation_outcome verify_signatures(git_operation_context& context, const verify_signatures_request& request)
        {
            if (request.shas.empty())
                return operation_outcome::succeeded();

            const allowed_signers_state allowed { read_allowed_signers(context) };
            git_command_spec spec {};
            try
            {
                spec.arguments = make_git_verify_signatures_args(allowed.blank);
                spec.standard_input = make_git_verify_signatures_input(request.shas);
            }
            catch (const std::invalid_argument& error)
            {
                signatures_verified unverifiable {};
                add_unverifiable(unverifiable, request.shas);
                context.emit(std::move(unverifiable));
                const std::string_view what { error.what() };
                return operation_outcome::failed(failure_kind::invalid_request, u8"서명을 확인할 커밋 이름이 올바르지 않습니다.", std::u8string { what.begin(), what.end() });
            }
            spec.command_class = git_command_class::local_query;
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            if (context.run_required(std::move(spec), outcome, stop) == false)
            {
                // 취소는 세션이 다시 요청할 수 있게 풀어 준다. 실패는 다시 요청하지 않으므로 여기서 "확인할 수 없음"으로 끝낸다.
                if (stop->state == operation_state::failed)
                {
                    signatures_verified unverifiable {};
                    add_unverifiable(unverifiable, request.shas);
                    context.emit(std::move(unverifiable));
                }
                return *stop;
            }

            signatures_verified verified {};
            verified.signatures = parse_git_signature_records(outcome.standard_output);
            for (commit_signature& signature : verified.signatures)
            {
                // ssh 서명은 서명자 목록에 없으면 principal이 비고 키는 `SHA256:` 지문이다. 목록 파일 자체가 없을 때는 "모르는 키"가 아니라
                // "대조할 수단이 없다"는 뜻이라 따로 알린다.
                if (allowed.missing && signature.status == signature_status::unknown_key && signature.signer.empty() && looks_like_ssh_fingerprint(signature.key))
                {
                    signature.status = signature_status::cannot_check;
                    signature.note = signature_note::allowed_signers_missing;
                }
            }
            add_unverifiable(verified, request.shas);
            context.emit(std::move(verified));
            return operation_outcome::succeeded();
        }
    } // namespace

    std::optional<operation_outcome> run_signing_operation(git_operation_context& context)
    {
        return std::visit(
            [&context](const auto& request) -> std::optional<operation_outcome> {
                using request_type = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<request_type, verify_signatures_request>)
                    return verify_signatures(context, request);
                else
                    return std::nullopt;
            },
            context.job().operation);
    }

    void read_signing_snapshot(git_operation_context& context, repository_snapshot& snapshot)
    {
        git_command_spec spec {};
        spec.arguments = make_git_signing_config_args();
        spec.accepted_exit_codes = { 0, 1 };
        const git_command_outcome outcome { context.run(std::move(spec)) };
        if (outcome.accepted)
            snapshot.signing = parse_git_signing_config(outcome.standard_output).effective;
    }
} // namespace cyella
