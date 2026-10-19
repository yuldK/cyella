#include "infrastructure/git_error_classifier.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace {
    using cyella::classify_git_failure;
    using cyella::git_failure_kind;
} // namespace

TEST_CASE("classify_git_failure recognizes outputs captured from real git", "[git][error]")
{
    CHECK(classify_git_failure(cyella::test::read_fixture("err_push_rejected.bin")) == git_failure_kind::push_rejected_non_fast_forward);
    CHECK(classify_git_failure(cyella::test::read_fixture("err_checkout_overwrite.bin")) == git_failure_kind::local_changes_would_be_overwritten);
    CHECK(classify_git_failure(cyella::test::read_fixture("err_index_lock.bin")) == git_failure_kind::index_lock_exists);
    CHECK(classify_git_failure(cyella::test::read_fixture("err_not_a_repository.bin")) == git_failure_kind::repository_not_found);
}

TEST_CASE("classify_git_failure finds merge conflicts in stdout", "[git][error]")
{
    // git은 병합 충돌 문구를 stdout으로 내고 stderr는 비어 있다.
    const std::string standard_output { cyella::test::read_fixture("merge_conflict_stdout.bin") };
    const std::string standard_error { cyella::test::read_fixture("merge_conflict_stderr.bin") };

    CHECK(classify_git_failure(standard_error, standard_output) == git_failure_kind::conflict);
    CHECK(classify_git_failure(standard_error) == git_failure_kind::unclassified);
}

TEST_CASE("classify_git_failure uses OpenSSH and libcurl tokens that are never translated", "[git][error]")
{
    CHECK(classify_git_failure("Host key verification failed.\nfatal: Could not read from remote repository.") == git_failure_kind::host_key_verification_failed);
    CHECK(classify_git_failure("git@github.com: Permission denied (publickey).\nfatal: Could not read from remote repository.") == git_failure_kind::permission_denied_publickey);
    CHECK(classify_git_failure("git@host: Permission denied (publickey,keyboard-interactive).") == git_failure_kind::permission_denied_publickey);
    CHECK(classify_git_failure("fatal: unable to access 'https://example.invalid/r.git/': Could not resolve host: example.invalid") == git_failure_kind::could_not_resolve_host);
    CHECK(classify_git_failure("ssh: Could not resolve hostname example.invalid: No such host is known.") == git_failure_kind::could_not_resolve_host);
}

TEST_CASE("classify_git_failure uses HTTP status numbers in a translated sentence", "[git][error]")
{
    // 문장이 번역되어도 상태 번호와 URL은 남는다.
    CHECK(classify_git_failure("fatal: unable to access 'https://example.com/r.git/': The requested URL returned error: 403") == git_failure_kind::authentication_required);
    CHECK(classify_git_failure("fatal: no se pudo acceder a 'https://example.com/r.git/': error 401") == git_failure_kind::authentication_required);
    CHECK(classify_git_failure("remote: Repository not found.\nfatal: repository 'https://example.com/r.git/' not found") == git_failure_kind::repository_not_found);
    CHECK(classify_git_failure("fatal: unable to access 'https://example.com/r.git/': The requested URL returned error: 404") == git_failure_kind::repository_not_found);
    // 브랜치 이름에 든 숫자는 HTTP 상태가 아니다.
    CHECK(classify_git_failure("fatal: invalid reference: release-4040") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("fatal: invalid reference: release-4040 (see https://example.com/help)") == git_failure_kind::unclassified);
}

TEST_CASE("classify_git_failure recognizes HTTPS prompts that cannot be answered", "[git][error]")
{
    CHECK(classify_git_failure("fatal: could not read Username for 'https://example.com': terminal prompts disabled") == git_failure_kind::authentication_required);
    CHECK(classify_git_failure("remote: Invalid username or password.\nfatal: Authentication failed for 'https://example.com/r.git/'") == git_failure_kind::authentication_required);
}

TEST_CASE("classify_git_failure separates non-fast-forward from other rejections", "[git][error]")
{
    CHECK(classify_git_failure(" ! [rejected]        main -> main (fetch first)") == git_failure_kind::push_rejected_non_fast_forward);
    CHECK(classify_git_failure(" ! [rejected]        v1 -> v1 (already exists)") == git_failure_kind::push_rejected);
    CHECK(classify_git_failure(" ! [remote rejected] main -> main (pre-receive hook declined)") == git_failure_kind::push_rejected);
    // 사유 문구가 번역되어도 표지 `[rejected]`는 남아 거절로 분류된다.
    CHECK(classify_git_failure(" ! [rejected]        main -> main (no es un avance rapido)") == git_failure_kind::push_rejected);
}

TEST_CASE("classify_git_failure recognizes conflicts from rebase and cherry-pick", "[git][error]")
{
    CHECK(classify_git_failure("error: could not apply 1234567... subject\nhint: Resolve all conflicts manually") == git_failure_kind::conflict);
    CHECK(classify_git_failure("Auto-merging a.txt\nCONFLICT (content): Merge conflict in a.txt\nAutomatic merge failed; fix conflicts and then commit the result.") == git_failure_kind::conflict);
}

TEST_CASE("classify_git_failure leaves unknown failures unclassified", "[git][error]")
{
    CHECK(classify_git_failure("") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("fatal: something nobody has seen before") == git_failure_kind::unclassified);
}

TEST_CASE("git_failure_kind has a name and a message for every kind", "[git][error]")
{
    for (int value { 0 }; value <= static_cast<int>(git_failure_kind::local_changes_would_be_overwritten); ++value)
    {
        const auto kind { static_cast<git_failure_kind>(value) };
        CHECK_FALSE(cyella::git_failure_kind_name(kind).empty());
        CHECK_FALSE(cyella::git_failure_message(kind).empty());
    }
}
