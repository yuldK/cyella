// 커밋 서명의 화면 흐름이다: 커밋 상자의 서명 토글, 태그 대화 상자의 서명, 상세 패널의 서명 배지, 보이는 커밋의 서명 요청.
// workspace는 가짜 runner와 수동 dispatcher로 세우고 job은 실행하지 않는다. operation 사건은 test가 직접 넣는다.
#include "application/app_messages.h"
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/dialog_intents.h"
#include "presentation/shell_controller.h"
#include "presentation/signing_intents.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    namespace snap = cyella::test::snapshot;

    struct harness
    {
        harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(workspace) }
        {
            controller.start({});
            controller.open_repository(u8"C:\\work\\repo");
            repository = controller.selected_repository_id();
        }

        static shell_controller::dependencies make_dependencies(git_workspace& workspace)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } }; };
            dependencies.temporary_directory = u8"C:\\temp\\cyella-test";
            return dependencies;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            controller.handle(luil::app_message { std::move(intent) });
            controller.after_events();
        }

        // 읽어 둔 저장소다. `signing`을 주면 refresh가 담는 서명 설정의 실효 값으로 쓴다.
        repository_session& load(const std::size_t count = 5, const std::uint32_t changed = 0, const signing_config& signing = {})
        {
            repository_session* const session { workspace.find_session(repository) };
            REQUIRE(session != nullptr);
            const std::uint64_t generation { session->begin_refresh() };
            repository_snapshot snapshot { snap::make_snapshot(snap::linear_history(count).front().sha, changed) };
            snapshot.signing = signing;
            REQUIRE(session->apply(snapshot_loaded { generation, std::move(snapshot) }));
            REQUIRE(session->apply(commits_loaded { .generation = generation, .first_index = 0, .commits = snap::linear_history(count), .page_complete = true, .history_complete = true }));
            controller.after_events();
            return *session;
        }

        [[nodiscard]] view_snapshot view()
        {
            return controller.make_view(1440.0f, 900.0f, 1.0f);
        }

        template<typename request_type>
        [[nodiscard]] std::vector<const request_type*> requests() const
        {
            std::vector<const request_type*> found {};
            for (const operation_id id : workspace.operation_order())
                if (const git_operation* const request { workspace.request_of(id) }; request != nullptr)
                    if (const auto* const typed { std::get_if<request_type>(request) })
                        found.push_back(typed);
            return found;
        }

        void type(const std::size_t field, const std::u8string& text)
        {
            luil::text_edit_request request {};
            request.target = to_luil_target(static_cast<text_target>(static_cast<std::uint32_t>(text_target::form_field_0) + field));
            request.command = luil::text::text_edit_command::insert;
            request.text = text;
            send(edit_text_intent { request });
        }

        void type_commit_subject(const std::u8string& text)
        {
            luil::text_edit_request request {};
            request.target = to_luil_target(text_target::commit_subject);
            request.command = luil::text::text_edit_command::insert;
            request.text = text;
            send(edit_text_intent { request });
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_operation_runner runner;
        git_workspace workspace;
        shell_controller controller;
        std::string repository {};
    };

    signing_config config_with(const signing_key_id key, const std::string& value, const config_origin origin = config_origin::local)
    {
        signing_config config {};
        config.at(key) = signing_entry { .value = value, .origin = origin };
        return config;
    }

    void select_working_area(harness& test)
    {
        test.send(select_commit_intent { graph_item_key(std::string { graph_wip_sha }) });
    }
} // namespace

TEST_CASE("the commit box sign toggle starts from commit.gpgsign and commit sends sign only when it matters", "[signing][presentation]")
{
    // 설정이 서명하지 않는다: 토글은 꺼져 있고, 건드리지 않으면 설정을 따른다 (sign을 싣지 않는다).
    {
        harness test {};
        test.load(5, 2);
        select_working_area(test);
        view_snapshot view { test.view() };
        REQUIRE(view.repository->working.has_value());
        CHECK_FALSE(view.repository->working->sign);

        test.type_commit_subject(u8"첫 커밋");
        test.send(commit_intent {});
        auto commits { test.requests<commit_request>() };
        REQUIRE(commits.size() == 1);
        CHECK_FALSE(commits[0]->sign.has_value());
    }
    // 토글을 켜면 -S다.
    {
        harness test {};
        test.load(5, 2);
        select_working_area(test);
        test.send(toggle_sign_intent {});
        CHECK(test.view().repository->working->sign);
        test.type_commit_subject(u8"서명 커밋");
        test.send(commit_intent {});
        auto commits { test.requests<commit_request>() };
        REQUIRE(commits.size() == 1);
        REQUIRE(commits[0]->sign.has_value());
        CHECK(*commits[0]->sign);
    }
    // 설정이 서명한다: 토글은 켜져 있고 그대로 커밋하면 -S, 끄면 --no-gpg-sign으로 설정을 덮는다.
    {
        harness test {};
        test.load(5, 2, config_with(signing_key_id::commit_gpgsign, "true"));
        select_working_area(test);
        CHECK(test.view().repository->working->sign);
        test.type_commit_subject(u8"설정을 따름");
        test.send(commit_intent {});
        auto first { test.requests<commit_request>() };
        REQUIRE(first.size() == 1);
        REQUIRE(first[0]->sign.has_value());
        CHECK(*first[0]->sign);
    }
    {
        harness test {};
        test.load(5, 2, config_with(signing_key_id::commit_gpgsign, "true"));
        select_working_area(test);
        test.send(toggle_sign_intent {});
        CHECK_FALSE(test.view().repository->working->sign);
        test.type_commit_subject(u8"서명하지 않음");
        test.send(commit_intent {});
        auto commits { test.requests<commit_request>() };
        REQUIRE(commits.size() == 1);
        REQUIRE(commits[0]->sign.has_value());
        CHECK_FALSE(*commits[0]->sign);
    }
}

TEST_CASE("the commit box warns when ssh signing has no key", "[signing][presentation]")
{
    harness test {};
    signing_config config { config_with(signing_key_id::gpg_format, "ssh") };
    config.at(signing_key_id::commit_gpgsign) = signing_entry { .value = "true", .origin = config_origin::global };
    test.load(5, 2, config);
    select_working_area(test);
    CHECK(test.view().repository->working->sign_hint.find(u8"ssh 서명 키가 없습니다") != std::u8string::npos);

    // 서명을 끄면 알릴 것이 없다. 키를 정하면 사라진다.
    test.send(toggle_sign_intent {});
    CHECK(test.view().repository->working->sign_hint.empty());
    config.at(signing_key_id::user_signingkey) = signing_entry { .value = "C:/k/id.pub", .origin = config_origin::local };
    test.load(5, 2, config);
    CHECK(test.view().repository->working->sign_hint.empty());
}

TEST_CASE("selecting a commit asks for its signature once and the detail panel shows the badge", "[signing][presentation]")
{
    harness test {};
    test.load(5);
    // 보이는 행으로 이미 요청했을 수 있으니 이 커밋만 따로 센다.
    const std::string target { snap::sha_of(2) };
    test.send(select_commit_intent { graph_item_key(target) });

    std::size_t asked { 0 };
    operation_id verify_id { 0 };
    for (const operation_id id : test.workspace.operation_order())
        if (const operation_status* const status { test.workspace.find_operation(id) }; status != nullptr && status->kind == operation_kind::verify_signatures)
        {
            const auto* const request { std::get_if<verify_signatures_request>(test.workspace.request_of(id)) };
            REQUIRE(request != nullptr);
            for (const std::string& sha : request->shas)
                if (sha == target)
                {
                    ++asked;
                    verify_id = id;
                }
        }
    CHECK(asked == 1);
    REQUIRE(verify_id != 0);
    // 화면을 채우는 읽기라 자동 operation이다 (상태 줄·작업 로그에 보이지 않는다).
    CHECK(test.workspace.find_operation(verify_id)->automatic);

    // 읽는 동안은 확인 중이다.
    view_snapshot view { test.view() };
    REQUIRE(view.repository->commit.has_value());
    CHECK_FALSE(view.repository->commit->signature.known);
    CHECK(view.repository->commit->signature.badge == u8"확인 중…");

    // 결과가 오면 배지가 선다.
    commit_signature good {};
    good.sha = target;
    good.status = signature_status::good;
    good.signer = u8"ann@example.com";
    good.key = u8"SHA256:abc";
    test.workspace.apply(operation_event { verify_id, test.repository, signatures_verified { { good } } });
    test.controller.after_events();
    view = test.view();
    const signature_view& badge { view.repository->commit->signature };
    CHECK(badge.known);
    CHECK(badge.badge == u8"서명 확인됨");
    CHECK(badge.tone == signature_tone::good);
    CHECK(badge.signer == u8"ann@example.com");
    CHECK(badge.key == u8"SHA256:abc");
    // 그래프 행도 같은 값을 읽는다.
    REQUIRE(view.repository->graph.signatures != nullptr);
    CHECK(view.repository->graph.signatures->at(target).status == signature_status::good);

    // 같은 커밋을 다시 고르면 다시 묻지 않는다.
    test.send(select_commit_intent { graph_item_key(snap::sha_of(1)) });
    const std::size_t before { test.requests<verify_signatures_request>().size() };
    test.send(select_commit_intent { graph_item_key(target) });
    for (const verify_signatures_request* const request : test.requests<verify_signatures_request>())
        CHECK(std::ranges::count(request->shas, target) <= 1);
    CHECK(test.requests<verify_signatures_request>().size() == before);
}

TEST_CASE("the visible graph rows are verified in one batch without the WIP row", "[signing][presentation]")
{
    harness test {};
    test.load(5, 2);
    const auto batches { test.requests<verify_signatures_request>() };
    REQUIRE(batches.size() == 1);
    CHECK(batches[0]->shas.size() == 5);
    CHECK(std::ranges::find(batches[0]->shas, std::string { graph_wip_sha }) == batches[0]->shas.end());
    // 같은 행을 거듭 요청하지 않는다.
    test.controller.after_events();
    test.controller.after_events();
    CHECK(test.requests<verify_signatures_request>().size() == 1);
}

TEST_CASE("the signature badge says why an ssh signature could not be verified", "[signing][presentation]")
{
    commit_signature unverifiable {};
    unverifiable.status = signature_status::cannot_check;
    unverifiable.note = signature_note::allowed_signers_missing;
    unverifiable.key = u8"SHA256:xyz";
    const signature_view view { make_signature_view(&unverifiable) };
    CHECK(view.badge == u8"확인할 수 없음 (allowed signers 없음)");
    CHECK(view.tone == signature_tone::warning);
    CHECK(view.description.find(u8"allowedSignersFile") != std::u8string::npos);

    CHECK(signature_tone_of(signature_status::good) == signature_tone::good);
    CHECK(signature_tone_of(signature_status::bad) == signature_tone::bad);
    CHECK(signature_tone_of(signature_status::revoked_key) == signature_tone::bad);
    CHECK(signature_tone_of(signature_status::unknown_key) == signature_tone::warning);
    CHECK(signature_tone_of(signature_status::expired_signature) == signature_tone::warning);
    CHECK(signature_tone_of(signature_status::expired_key) == signature_tone::warning);
    CHECK(signature_tone_of(signature_status::none) == signature_tone::neutral);
    CHECK(make_signature_view(nullptr).known == false);
}

TEST_CASE("the tag dialog signs with tag.gpgsign as the default and forces annotated when signing", "[signing][presentation]")
{
    const auto open = [](harness& test) {
        test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = snap::sha_of(3) }, 0.0f, 0.0f });
        test.send(menu_select_intent { u8"create_tag" });
    };
    // 서명하지 않는 설정: 체크는 꺼져 있고 lightweight다. 켜면 annotated로 옮겨가고 메시지가 있어야 만들어진다.
    {
        harness test {};
        test.load();
        open(test);
        view_snapshot view { test.view() };
        REQUIRE(view.form.has_value());
        REQUIRE(view.form->checks.size() == 1);
        CHECK(view.form->checks[0].label == u8"서명 (tag -s)");
        CHECK_FALSE(view.form->checks[0].checked);
        CHECK(view.form->chosen == 0);

        test.type(0, u8"v3.0");
        test.send(form_check_intent { 0 });
        view = test.view();
        CHECK(view.form->checks[0].checked);
        CHECK(view.form->chosen == 1);
        REQUIRE(view.form->fields.size() == 2);
        test.type(1, u8"서명한 릴리스");
        test.send(form_button_intent { u8"submit" });
        auto tags { test.requests<create_tag_request>() };
        REQUIRE(tags.size() == 1);
        CHECK(tags[0]->annotated);
        REQUIRE(tags[0]->sign.has_value());
        CHECK(*tags[0]->sign);
    }
    // lightweight를 고르면 서명이 꺼진다.
    {
        harness test {};
        test.load();
        open(test);
        test.send(form_check_intent { 0 });
        test.send(form_choose_intent { 0 });
        view_snapshot view { test.view() };
        CHECK_FALSE(view.form->checks[0].checked);
        test.type(0, u8"v3.1");
        test.send(form_button_intent { u8"submit" });
        auto tags { test.requests<create_tag_request>() };
        REQUIRE(tags.size() == 1);
        CHECK_FALSE(tags[0]->annotated);
        CHECK_FALSE(tags[0]->sign.has_value());
    }
    // tag.gpgsign가 켜져 있다: 기본으로 서명하는 annotated이고, 서명을 끄고 lightweight를 고르면 설정을 덮는다.
    {
        harness test {};
        test.load(5, 0, config_with(signing_key_id::tag_gpgsign, "true"));
        open(test);
        view_snapshot view { test.view() };
        CHECK(view.form->checks[0].checked);
        CHECK(view.form->chosen == 1);
        test.send(form_choose_intent { 0 });
        test.type(0, u8"v3.2");
        test.send(form_button_intent { u8"submit" });
        auto tags { test.requests<create_tag_request>() };
        REQUIRE(tags.size() == 1);
        CHECK_FALSE(tags[0]->annotated);
        REQUIRE(tags[0]->sign.has_value());
        CHECK_FALSE(*tags[0]->sign);
    }
}
