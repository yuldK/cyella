// 커밋 서명의 순수 규칙이다: `%G?` 해석, 설정 모음과 gpg 키 목록 해석, 명령줄 조립, 서명 실패 분류.
// 고정 출력은 tests/data/git/make_signing_fixtures.sh가 실제 git 2.52·gpg 2.4.8·OpenSSH 10.2로 만든 것이다.
#include "domain/commit_signature.h"
#include "domain/signing_config.h"
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_error_classifier.h"
#include "infrastructure/git_signing_commands.h"
#include "infrastructure/git_signing_failure.h"
#include "infrastructure/git_signing_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    using cyella::test::to_text;
    using text_list = std::vector<std::string>;

    std::string fixture(const char* name)
    {
        return test::read_fixture(name);
    }
} // namespace

// ---------------------------------------------------------------------- 서명 상태

TEST_CASE("signature_status maps every %G? letter git documents", "[signing][domain]")
{
    CHECK(signature_status_from_code('G') == signature_status::good);
    CHECK(signature_status_from_code('B') == signature_status::bad);
    CHECK(signature_status_from_code('U') == signature_status::unknown_key);
    CHECK(signature_status_from_code('X') == signature_status::expired_signature);
    CHECK(signature_status_from_code('Y') == signature_status::expired_key);
    CHECK(signature_status_from_code('R') == signature_status::revoked_key);
    CHECK(signature_status_from_code('E') == signature_status::cannot_check);
    CHECK(signature_status_from_code('N') == signature_status::none);
    // 모르는 글자는 서명이 없는 것으로 오해하지 않고 "확인할 수 없음"이다.
    CHECK(signature_status_from_code('?') == signature_status::cannot_check);
    for (const char code : { 'G', 'B', 'U', 'X', 'Y', 'R', 'E', 'N' })
        CHECK(signature_status_code(signature_status_from_code(code)) == code);
    CHECK(is_signed(signature_status::none) == false);
    CHECK(is_signed(signature_status::bad));
}

TEST_CASE("signature badges are Korean and mention the missing allowed signers file", "[signing][domain]")
{
    CHECK(signature_badge_text(signature_status::good) == u8"서명 확인됨");
    CHECK(signature_badge_text(signature_status::bad) == u8"서명이 올바르지 않음");
    CHECK(signature_badge_text(signature_status::unknown_key) == u8"알 수 없는 키");
    CHECK(signature_badge_text(signature_status::expired_signature) == u8"서명 만료");
    CHECK(signature_badge_text(signature_status::expired_key) == u8"키 만료");
    CHECK(signature_badge_text(signature_status::revoked_key) == u8"키 폐기됨");
    CHECK(signature_badge_text(signature_status::none) == u8"서명 없음");

    commit_signature unverifiable {};
    unverifiable.status = signature_status::cannot_check;
    unverifiable.note = signature_note::allowed_signers_missing;
    CHECK(signature_badge_label(unverifiable) == u8"확인할 수 없음 (allowed signers 없음)");
    CHECK(signature_description(unverifiable).find(u8"allowedSignersFile") != std::u8string::npos);

    commit_signature good {};
    good.status = signature_status::good;
    good.signer = u8"ann@example.com";
    good.key = u8"SHA256:abc";
    CHECK(signature_badge_label(good) == u8"서명 확인됨");
    CHECK(signature_description(good) == u8"올바른 서명입니다.");
    CHECK(signature_description(commit_signature {}).empty());
}

// ---------------------------------------------------------------------- %G? 출력 해석

TEST_CASE("parse_git_signature_records reads ssh results captured from real git", "[signing][parser]")
{
    const std::vector<commit_signature> records { parse_git_signature_records(fixture("signing_log_ssh.bin")) };
    // 변조(B), 목록에 없는 서명자(U), 올바른 서명(G), 서명 없음(N) 순서다.
    REQUIRE(records.size() == 4);
    CHECK(records[0].status == signature_status::bad);
    CHECK(records[0].signer.empty());
    CHECK(records[1].status == signature_status::unknown_key);
    CHECK(records[1].signer.empty());
    CHECK(records[1].key.starts_with(u8"SHA256:"));
    CHECK(records[2].status == signature_status::good);
    CHECK(records[2].signer == u8"ann@example.com");
    CHECK(records[2].key.starts_with(u8"SHA256:"));
    CHECK(records[3].status == signature_status::none);
    for (const commit_signature& record : records)
        CHECK(record.sha.size() == 40);
}

TEST_CASE("an unset allowedSignersFile makes ssh signatures look unsigned unless it is blanked", "[signing][parser]")
{
    // 설정이 없으면 git은 ssh 서명을 검증하지 못하고 모두 N으로 보고한다. 서명 없는 커밋과 구별할 수 없다.
    for (const commit_signature& record : parse_git_signature_records(fixture("signing_log_ssh_unset.bin")))
        CHECK(record.status == signature_status::none);
    CHECK(fixture("signing_log_ssh_unset_stderr.bin").find("gpg.ssh.allowedSignersFile needs to be configured") != std::string::npos);

    // 빈 값으로 덮어 읽으면 서명이 있는 커밋은 U와 지문이 나오고, 서명 없는 커밋만 N이다.
    const std::vector<commit_signature> blank { parse_git_signature_records(fixture("signing_log_ssh_blank.bin")) };
    REQUIRE(blank.size() == 4);
    CHECK(blank[0].status == signature_status::bad);
    CHECK(blank[1].status == signature_status::unknown_key);
    CHECK(blank[2].status == signature_status::unknown_key);
    CHECK(blank[2].signer.empty());
    CHECK(blank[2].key.starts_with(u8"SHA256:"));
    CHECK(blank[3].status == signature_status::none);
}

TEST_CASE("parse_git_signature_records reads gpg results and skips malformed records", "[signing][parser]")
{
    const std::vector<commit_signature> records { parse_git_signature_records(fixture("signing_log_gpg.bin")) };
    REQUIRE(records.size() == 2);
    CHECK(records[0].status == signature_status::bad);
    CHECK(records[1].status == signature_status::good);
    CHECK(records[1].signer == u8"Ann Author <ann@example.com>");
    CHECK(records[1].key.size() == 16);

    const std::string sha(40, 'a');
    const std::string junk { "garbage" + std::string(1, '\0') + sha + "\x1f" + "GG\x1f\x1f" + std::string(1, '\0') + sha + "\x1f" + "G\x1f" + "who\x1f" + "KEY" + std::string(1, '\0') };
    const std::vector<commit_signature> partial { parse_git_signature_records(junk) };
    REQUIRE(partial.size() == 1);
    CHECK(partial[0].signer == u8"who");
    CHECK(partial[0].key == u8"KEY");
    CHECK(parse_git_signature_records("").empty());
}

// ---------------------------------------------------------------------- 설정

TEST_CASE("parse_git_signing_config separates the scopes and keeps the effective value", "[signing][parser]")
{
    const signing_config_layers layers { parse_git_signing_config(fixture("signing_config_scopes.bin")) };

    // 전역은 commit.gpgsign=true, 지역이 false로 덮는다. 실효 값은 지역이다.
    CHECK(layers.global.at(signing_key_id::commit_gpgsign).value == "true");
    CHECK(layers.local.at(signing_key_id::commit_gpgsign).value == "false");
    CHECK(layers.effective.at(signing_key_id::commit_gpgsign).origin == config_origin::local);
    CHECK(layers.effective.commit_signing() == false);
    CHECK(layers.global.commit_signing());

    // `[tag] gpgsign`처럼 `=`가 없는 불리언은 참이다.
    CHECK(layers.global.at(signing_key_id::tag_gpgsign).valueless);
    CHECK(layers.effective.tag_signing());
    CHECK(layers.local.at(signing_key_id::tag_gpgsign).is_set() == false);

    CHECK(layers.effective.format() == signing_format::ssh);
    CHECK(layers.global.format() == signing_format::openpgp);
    CHECK(layers.effective.has_signing_key());
    CHECK(layers.effective.at(signing_key_id::user_signingkey).value.ends_with("key_ann.pub"));
    CHECK(layers.effective.at(signing_key_id::gpg_program).origin == config_origin::global);
    CHECK(layers.effective.at(signing_key_id::gpg_program).value == "C:/Program Files/Git/usr/bin/gpg.exe");
    CHECK(layers.effective.at(signing_key_id::gpg_ssh_program).origin == config_origin::local);
    CHECK(layers.effective.at(signing_key_id::gpg_ssh_allowed_signers).value.ends_with("allowed_ann"));
    CHECK(layers.global.has_signing_key() == false);

    const signing_config_layers empty { parse_git_signing_config("") };
    CHECK(empty.effective.commit_signing() == false);
    CHECK(empty.effective.format() == signing_format::openpgp);
    CHECK(empty.effective.at(signing_key_id::gpg_format).is_set() == false);
}

TEST_CASE("config booleans follow git's spelling", "[signing][domain]")
{
    bool value { false };
    for (const char* word : { "true", "TRUE", "yes", "On", "1" })
    {
        value = false;
        CHECK(parse_config_bool(word, value));
        CHECK(value);
    }
    for (const char* word : { "false", "No", "off", "0", "" })
    {
        value = true;
        CHECK(parse_config_bool(word, value));
        CHECK(value == false);
    }
    value = true;
    CHECK(parse_config_bool("maybe", value) == false);
    CHECK(value);

    signing_format format { signing_format::ssh };
    CHECK(parse_signing_format("OpenPGP", format));
    CHECK(format == signing_format::openpgp);
    CHECK(parse_signing_format("x509", format) == false);
}

// ---------------------------------------------------------------------- gpg 키 목록

TEST_CASE("parse_gpg_secret_keys reads the colon listing of real gpg", "[signing][parser]")
{
    const std::vector<gpg_secret_key> keys { parse_gpg_secret_keys(fixture("gpg_secret_keys.bin"), 1800000000) };
    REQUIRE(keys.size() == 2);

    const gpg_secret_key& ed { keys[0] };
    CHECK(ed.key_id.size() == 16);
    CHECK(ed.fingerprint.size() == 40);
    CHECK(ed.fingerprint.ends_with(ed.key_id));
    CHECK(ed.algorithm == "ed25519");
    CHECK(ed.can_sign);
    CHECK(ed.expired == false);
    CHECK(ed.revoked == false);
    CHECK(ed.expires == 0);
    // 사용자 ID 둘이고 하위 키(cv25519, 암호화용)의 지문이 주 키 지문을 덮지 않는다.
    REQUIRE(ed.user_ids.size() == 2);
    CHECK(ed.user_ids[0] == u8"Ann Author <ann@example.com>");
    CHECK(ed.user_ids[1] == u8"Ann Work <ann@work.example>");

    const gpg_secret_key& rsa { keys[1] };
    CHECK(rsa.algorithm == "rsa3072");
    CHECK(rsa.can_sign);
    REQUIRE(rsa.user_ids.size() == 1);
    CHECK(rsa.user_ids[0] == u8"Rsa Keeper <rsa@example.com>");
    CHECK(rsa.fingerprint != ed.fingerprint);

    CHECK(parse_gpg_secret_keys("", 0).empty());
}

TEST_CASE("parse_gpg_secret_keys flags expired and revoked keys and decodes escaped user ids", "[signing][parser]")
{
    std::string listing {};
    listing += "sec:e:255:22:AAAAAAAAAAAAAAAA:1600000000:1700000000::u:::sc:::+::ed25519::\n";
    listing += "fpr:::::::::1111111111111111111111111111111111111111:\n";
    listing += "uid:e::::1600000000::HASH::Old Key (a\\x3ab) <old@example.com>::::::::::0:\n";
    listing += "sec:r:255:22:BBBBBBBBBBBBBBBB:1600000000:::u:::sc:::+::ed25519::\n";
    listing += "fpr:::::::::2222222222222222222222222222222222222222:\n";
    listing += "uid:r::::1600000000::HASH::Revoked <rev@example.com>::::::::::0:\n";
    listing += "sec:u:255:22:CCCCCCCCCCCCCCCC:1600000000:1700000000::u:::sc:::+::ed25519::\n";
    listing += "fpr:::::::::3333333333333333333333333333333333333333:\n";
    listing += "uid:u::::1600000000::HASH::Lapsed <lapsed@example.com>::::::::::0:\n";
    const std::vector<gpg_secret_key> keys { parse_gpg_secret_keys(listing, 1800000000) };
    REQUIRE(keys.size() == 3);
    CHECK(keys[0].expired);
    CHECK(keys[0].user_ids[0] == u8"Old Key (a:b) <old@example.com>");
    CHECK(keys[1].revoked);
    // 상태 글자는 u인데 만료 시각이 지났으면 만료로 본다.
    CHECK(keys[2].expired);
    CHECK(keys[2].fingerprint == std::string(40, '3'));
    // 기준 시각이 만료 전이면 만료가 아니다.
    CHECK(parse_gpg_secret_keys(listing, 1650000000)[2].expired == false);
}

// ---------------------------------------------------------------------- 명령줄 조립

TEST_CASE("signing command builders produce the commands the parsers expect", "[signing][builder]")
{
    CHECK(to_text(make_git_verify_signatures_args(false)) == text_list { "log", "--no-walk=unsorted", "--stdin", "-z", "--format=%H%x1f%G?%x1f%GS%x1f%GK" });
    CHECK(to_text(make_git_verify_signatures_args(true)) == text_list { "-c", "gpg.ssh.allowedSignersFile=", "log", "--no-walk=unsorted", "--stdin", "-z", "--format=%H%x1f%G?%x1f%GS%x1f%GK" });
    CHECK(to_text(make_git_signing_config_args()).size() == 5);
    CHECK(to_text(make_git_signing_config_args())[0] == "config");
    CHECK(to_text(make_git_signing_config_args())[2] == "--show-scope");
    CHECK(to_text(make_git_signing_config_args())[4].find("commit\\.gpgsign") != std::string::npos);
    CHECK(to_text(make_git_allowed_signers_path_args()) == text_list { "config", "--type=path", "--get", "gpg.ssh.allowedsignersfile" });
    CHECK(to_text(make_git_signing_config_set_args(signing_key_id::commit_gpgsign, "true", false)) == text_list { "config", "--local", "commit.gpgsign", "true" });
    CHECK(to_text(make_git_signing_config_set_args(signing_key_id::user_signingkey, "C:/k/id.pub", true)) == text_list { "config", "--global", "user.signingkey", "C:/k/id.pub" });
    CHECK(to_text(make_git_signing_config_unset_args(signing_key_id::gpg_program, false)) == text_list { "config", "--local", "--unset", "gpg.program" });
    CHECK(to_text(make_gpg_list_secret_keys_args())[0] == "--list-secret-keys");
    CHECK(to_text(make_gpg_list_secret_keys_args())[1] == "--with-colons");
}

TEST_CASE("signing command builders reject values that could change the command", "[signing][builder]")
{
    const std::vector<std::string> good { std::string(40, 'a'), std::string(64, 'B') };
    CHECK(make_git_verify_signatures_input(good) == std::string(40, 'a') + "\n" + std::string(64, 'B') + "\n");
    for (const std::string bad : { std::string { "HEAD" }, std::string { "--all" }, std::string(40, 'g'), std::string(39, 'a'), std::string(40, 'a') + "\nHEAD" })
    {
        const std::vector<std::string> shas { bad };
        CHECK_THROWS_AS(make_git_verify_signatures_input(shas), std::invalid_argument);
    }
    CHECK_THROWS_AS(make_git_signing_config_set_args(signing_key_id::gpg_format, "ssh\nx", false), std::invalid_argument);
    CHECK_THROWS_AS(make_git_signing_config_set_args(signing_key_id::gpg_format, std::string("a\0b", 3), false), std::invalid_argument);
}

TEST_CASE("commit and tag builders add the signing flags", "[signing][builder]")
{
    git_commit_options options {};
    CHECK(to_text(make_git_commit_args(options)) == text_list { "commit", "-F", "-" });
    options.sign = true;
    CHECK(to_text(make_git_commit_args(options)) == text_list { "commit", "-S", "-F", "-" });
    options.sign = false;
    CHECK(to_text(make_git_commit_args(options)) == text_list { "commit", "--no-gpg-sign", "-F", "-" });
    options.amend = true;
    options.keep_message = true;
    options.sign = true;
    CHECK(to_text(make_git_commit_args(options)) == text_list { "commit", "--amend", "-S", "--no-edit" });

    CHECK(to_text(make_git_tag_args("v1", "", true)) == text_list { "tag", "--no-sign", "--", "v1" });
    CHECK(to_text(make_git_annotated_tag_args("v1", "HEAD~1", true)) == text_list { "tag", "-a", "--no-sign", "-F", "-", "--", "v1", "HEAD~1" });
    CHECK(to_text(make_git_signed_tag_args("v1")) == text_list { "tag", "-s", "-F", "-", "--", "v1" });
    CHECK(to_text(make_git_signed_tag_args("v1", "abc1234")) == text_list { "tag", "-s", "-F", "-", "--", "v1", "abc1234" });
    CHECK_THROWS_AS(make_git_signed_tag_args("bad name"), std::invalid_argument);
}

// ---------------------------------------------------------------------- 서명 실패 분류

TEST_CASE("classify_git_failure recognizes signing failures captured from real git, gpg and ssh-keygen", "[signing][error]")
{
    // gpg: 키가 없다 (FAILURE sign 17).
    CHECK(classify_git_failure(fixture("err_sign_gpg_no_secret_key.bin")) == git_failure_kind::signing_key_missing);
    CHECK(classify_git_failure(fixture("err_sign_tag_gpg_no_secret_key.bin")) == git_failure_kind::signing_key_missing);
    // gpg: pinentry가 없다(85), 사용자가 취소했다(99).
    CHECK(classify_git_failure(fixture("err_sign_gpg_no_pinentry.bin")) == git_failure_kind::signing_passphrase_failed);
    CHECK(classify_git_failure(fixture("err_sign_gpg_cancelled.bin")) == git_failure_kind::signing_passphrase_failed);
    // ssh: 공개 키 파일이 없다, passphrase가 맞지 않는다.
    CHECK(classify_git_failure(fixture("err_sign_ssh_missing_key.bin")) == git_failure_kind::signing_key_missing);
    CHECK(classify_git_failure(fixture("err_sign_ssh_passphrase.bin")) == git_failure_kind::signing_passphrase_failed);
    // 서명 프로그램을 실행하지 못했다.
    CHECK(classify_git_failure(fixture("err_sign_gpg_spawn.bin")) == git_failure_kind::signing_failed);
    CHECK(classify_git_failure(fixture("err_sign_ssh_spawn.bin")) == git_failure_kind::signing_failed);
}

TEST_CASE("signing classification reads the gpg status code, not only the English text", "[signing][error]")
{
    // 번역된 gpg 문구여도 `[GNUPG:] FAILURE sign <코드>`의 아래 16비트가 같다.
    CHECK(classify_git_failure("error: gpg failed to sign the data:\n[GNUPG:] FAILURE sign 67108949\nfatal: failed to write commit object") == git_failure_kind::signing_passphrase_failed);
    CHECK(classify_git_failure("error: gpg failed to sign the data:\n[GNUPG:] FAILURE sign 67108875\n") == git_failure_kind::signing_passphrase_failed);
    CHECK(classify_git_failure("error: gpg failed to sign the data:\n[GNUPG:] FAILURE sign 67108963\n") == git_failure_kind::signing_passphrase_failed);
    CHECK(classify_git_failure("error: gpg failed to sign the data:\n[GNUPG:] FAILURE sign 17\n") == git_failure_kind::signing_key_missing);
    CHECK(classify_git_failure("error: gpg failed to sign the data:\n[GNUPG:] FAILURE sign 33554433\n") == git_failure_kind::signing_failed);
}

TEST_CASE("signing classification does not swallow unrelated failures", "[signing][error]")
{
    // ssh 키 passphrase 오류는 fetch에서도 나온다. 서명이 아니라 인증 실패다.
    CHECK(classify_git_failure("Load key \"C:/k\": incorrect passphrase supplied to decrypt private key\ngit@host: Permission denied (publickey).\nfatal: Could not read from remote repository.")
        == git_failure_kind::permission_denied_publickey);
    // 서명과 무관한 "failed to write commit object"(디스크 오류 등)는 서명 실패로 보지 않는다.
    CHECK(classify_git_failure("error: unable to write file .git/objects/ab/cd: No space left on device\nfatal: failed to write commit object") == git_failure_kind::unclassified);
    CHECK(classify_git_failure("fatal: Could not read from remote repository.") == git_failure_kind::unclassified);
}

TEST_CASE("signing_failure_summary tells gpg and ssh problems apart", "[signing][error]")
{
    const std::u8string gpg_passphrase { signing_failure_summary(failure_kind::signing_passphrase_failed, fixture("err_sign_gpg_no_pinentry.bin")) };
    CHECK(gpg_passphrase == u8"gpg가 passphrase를 받지 못했습니다 — gpg-agent 또는 pinentry 설정을 확인하세요.");
    const std::u8string ssh_passphrase { signing_failure_summary(failure_kind::signing_passphrase_failed, fixture("err_sign_ssh_passphrase.bin")) };
    CHECK(ssh_passphrase.find(u8"ssh") != std::u8string::npos);
    CHECK(ssh_passphrase.find(u8"gpg") == std::u8string::npos);
    CHECK(signing_failure_summary(failure_kind::signing_key_missing, fixture("err_sign_gpg_no_secret_key.bin")).find(u8"gpg") != std::u8string::npos);
    CHECK(signing_failure_summary(failure_kind::signing_key_missing, fixture("err_sign_ssh_missing_key.bin")).find(u8".pub") != std::u8string::npos);
    CHECK(signing_failure_summary(failure_kind::signing_failed, fixture("err_sign_gpg_spawn.bin")).find(u8"gpg.program") != std::u8string::npos);
    CHECK(signing_failure_summary(failure_kind::conflict, "x").empty());

    // 새 종류의 이름과 기본 문구가 있다.
    CHECK(failure_kind_name(failure_kind::signing_failed) == "signing_failed");
    CHECK(failure_kind_name(failure_kind::signing_key_missing) == "signing_key_missing");
    CHECK(failure_kind_name(failure_kind::signing_passphrase_failed) == "signing_passphrase_failed");
    CHECK(failure_kind_message(failure_kind::signing_key_missing).empty() == false);
    CHECK(operation_kind_name(operation_kind::verify_signatures) == "verify_signatures");
}
