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
    // 상태 표지(`returned error: `, `HTTP `) 없이 이름에 붙은 숫자는 앞뒤가 숫자가 아니어도 상태가 아니다.
    CHECK(classify_git_failure("fatal: could not checkout https://example.com/r/issue-403") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("fatal: pathspec 'https://example.com/public/404.html' did not match any file") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("error: RPC failed; HTTP 403 curl 22 The requested URL returned error: 403") == git_failure_kind::authentication_required);
    CHECK(classify_git_failure("< HTTP/1.1 401 Unauthorized") == git_failure_kind::authentication_required);
    // 표지처럼 보이는 글(`error: `) 바로 뒤라도 이름이 이어지면(`404.html`) 상태가 아니다. 문장 끝의 마침표는 이름이 아니다.
    CHECK(classify_git_failure("From https://example.com/r\nerror: 404.html: needs merge") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("fatal: unable to access 'https://example.com/r.git/': error 403.") == git_failure_kind::authentication_required);
}

TEST_CASE("classify_git_failure prefers push rejections and local changes over look-alike HTTP status numbers", "[git][error]")
{
    // `To https://...`가 있어 HTTP 문맥이 되고 브랜치 이름 `issue-403`이 상태 번호처럼 보이는 출력이다.
    CHECK(classify_git_failure("To https://h/r.git\n ! [rejected]        issue-403 -> issue-403 (fetch first)\nerror: failed to push some refs to 'https://h/r.git'")
        == git_failure_kind::push_rejected_fetch_first);
    // `From https://...` 뒤의 경로 `public/404.html`이 상태 번호처럼 보인다.
    CHECK(classify_git_failure("From https://h/r\nerror: Your local changes to the following files would be overwritten by merge:\n\tpublic/404.html\nPlease commit your changes or stash them before you merge.")
        == git_failure_kind::local_changes_would_be_overwritten);
    CHECK(classify_git_failure("From https://h/r\nAuto-merging public/403.html\nCONFLICT (content): Merge conflict in public/403.html") == git_failure_kind::conflict);
}

TEST_CASE("classify_git_failure ignores lines the server sent after remote:", "[git][error]")
{
    // 서버가 보낸 글로는 호스트 키 경고도, HTTP 상태도 만들 수 없다.
    CHECK(classify_git_failure("remote: @@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED @@@\nfatal: Could not read from remote repository.") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("remote: HTTP 403 returned by the hook\nfatal: https://example.com/r.git: something nobody has seen before") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("remote: Permission denied (publickey).") == git_failure_kind::unclassified);
    // `remote:` 줄이 섞여 있어도 그 밖의 줄의 신호는 그대로 찾는다.
    CHECK(classify_git_failure("remote: Resolving deltas: 100% (1/1), completed with 1 local object.\n ! [remote rejected] main -> main (pre-receive hook declined)") == git_failure_kind::push_rejected);
    CHECK(classify_git_failure("remote: Invalid username or password.\nfatal: Authentication failed for 'https://example.com/r.git/'") == git_failure_kind::authentication_required);
    // 저장소 없음은 `remote:` 줄로만 알리는 서버가 있어 다른 신호가 없을 때에 한해 받는다.
    CHECK(classify_git_failure("remote: Repository not found.") == git_failure_kind::repository_not_found);
}

TEST_CASE("classify_git_failure recognizes HTTPS prompts that cannot be answered", "[git][error]")
{
    CHECK(classify_git_failure("fatal: could not read Username for 'https://example.com': terminal prompts disabled") == git_failure_kind::authentication_required);
    CHECK(classify_git_failure("remote: Invalid username or password.\nfatal: Authentication failed for 'https://example.com/r.git/'") == git_failure_kind::authentication_required);
}

TEST_CASE("classify_git_failure separates non-fast-forward from other rejections", "[git][error]")
{
    CHECK(classify_git_failure(" ! [rejected]        main -> main (fetch first)") == git_failure_kind::push_rejected_fetch_first);
    CHECK(classify_git_failure(" ! [rejected]        main -> main (stale info)") == git_failure_kind::push_rejected_stale_info);
    CHECK(classify_git_failure(" ! [rejected]        main -> main (non-fast-forward)") == git_failure_kind::push_rejected_non_fast_forward);
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
    for (int value { 0 }; value <= static_cast<int>(git_failure_kind::not_possible_to_fast_forward); ++value)
    {
        const auto kind { static_cast<git_failure_kind>(value) };
        CHECK_FALSE(cyella::git_failure_kind_name(kind).empty());
        CHECK_FALSE(cyella::git_failure_message(kind).empty());
    }
}

TEST_CASE("classify_git_failure separates a changed host key from an unknown one", "[git][error]")
{ // OpenSSH 9.x가 실제로 내는 모양이다. 마지막 줄은 처음 보는 키와 같으므로 경고를 먼저 봐야 한다.
    const std::string standard_error = "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                                       "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
                                       "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                                       "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\n"
                                       "The fingerprint for the ED25519 key sent by the remote host is\n"
                                       "SHA256:abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG.\n"
                                       "Add correct host key in /c/Users/me/.ssh/known_hosts to get rid of this message.\n"
                                       "Offending ED25519 key in C:/Users/me/.ssh/known_hosts:12\r\n"
                                       "Host key for github.com has changed and you have requested strict checking.\n"
                                       "Host key verification failed.\n"
                                       "fatal: Could not read from remote repository.\n";
    CHECK(classify_git_failure(standard_error) == git_failure_kind::host_key_changed);

    const auto location { cyella::find_offending_known_hosts_line(standard_error) };
    REQUIRE(location.has_value());
    CHECK(location->path == "C:/Users/me/.ssh/known_hosts");
    CHECK(location->line == 12);

    CHECK_FALSE(cyella::find_offending_known_hosts_line("Host key verification failed.").has_value());
}

TEST_CASE("known_hosts and worktree paths are never read from lines the server sent", "[git][error]")
{
    const std::string forged = "remote: @@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED @@@\n"
                               "remote: Offending RSA key in C:/forged/known_hosts:1\n";
    CHECK_FALSE(cyella::find_offending_known_hosts_line(forged).has_value());

    // 진짜 경고에 서버 줄이 섞여도 경로는 로컬 ssh가 낸 줄에서만 읽는다.
    const std::string mixed = "remote: Offending RSA key in C:/forged/known_hosts:1\n"
                              "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
                              "Offending ED25519 key in C:/Users/me/.ssh/known_hosts:12\r\n";
    const auto location { cyella::find_offending_known_hosts_line(mixed) };
    REQUIRE(location.has_value());
    CHECK(location->path == "C:/Users/me/.ssh/known_hosts");
    CHECK(location->line == 12);

    const auto path { cyella::find_checked_out_worktree_path("fatal: 'main' is already used by worktree at 'C:/work/other'\n") };
    REQUIRE(path.has_value());
    CHECK(*path == "C:/work/other");
    CHECK_FALSE(cyella::find_checked_out_worktree_path("remote: fatal: 'main' is already used by worktree at 'C:/forged'\n").has_value());
}

TEST_CASE("classify_git_failure recognizes refused connections and impossible fast-forwards", "[git][error]")
{
    CHECK(classify_git_failure("fatal: unable to access 'https://example.com/r.git/': Failed to connect to example.com port 443 after 21 ms: Couldn't connect to server")
        == git_failure_kind::connection_failed);
    CHECK(classify_git_failure("ssh: connect to host example.com port 22: Connection refused") == git_failure_kind::connection_failed);
    CHECK(classify_git_failure("hint: Diverging branches can't be fast-forwarded\nfatal: Not possible to fast-forward, aborting.") == git_failure_kind::not_possible_to_fast_forward);
}
