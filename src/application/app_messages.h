#pragma once

#include "application/git_operation.h"
#include "domain/operation_status.h"

#include <cstdint>
#include <string>

namespace cyella {
    // logic thread가 처리하는 앱 메시지(intent)다. 평범한 구조체이며 luil의
    // `app_message`에 담겨 스레드를 건넌다. 이 헤더는 luil UI 헤더를 include하지 않는다.

    // 창 크기와 배율이 정해졌거나 바뀌었다. 길이는 물리 픽셀이다.
    struct window_metrics_intent
    {
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
    };

    // 창을 닫는다 (caption X, Alt+F4).
    struct close_intent
    {};

    // worker 결과가 `operation_result_relay`에 쌓였으니 비우라는 깨우기다. 내용은 relay 큐에
    // 있고 이 메시지는 아무것도 나르지 않는다.
    struct relay_drain_intent
    {};

    // 저장소 탭을 연다. 이미 열린 경로면 그 탭이다. 처음 열면 refresh가 돈다.
    struct open_repository_intent
    {
        std::u8string path {};
    };

    struct close_repository_intent
    {
        std::string repository_id {};
    };

    // UI가 저장소 operation을 낸다. 값은 `git_operation.h`의 요청 타입이다.
    struct submit_operation_intent
    {
        std::string repository_id {};
        git_operation operation {};
    };

    // clone·init·inspect_path를 낸다. 저장소 id 대신 대상 경로로 묶인다.
    struct submit_setup_intent
    {
        git_operation operation {};
    };

    struct cancel_operation_intent
    {
        operation_id id { 0 };
    };

    // 인증 대화 상자의 답이다. `needs_input{passphrase|host_key|credentials}`의 `prompt_id`를 그대로
    // 돌려준다. 이미 답했거나 취소했거나 끝난 질문에 대한 답은 아무 일도 하지 않는다. 빈 답도
    // 유효한 답이다 (`ssh-keygen`의 "passphrase 없음").
    //
    // app_message는 값을 불변으로 공유하지만 답은 비밀일 수 있어, 쓰고 나서 지울 수 있게
    // `mutable`로 둔다. 쓰는 쪽(`cyella_driver`)이 workspace에 넘기고 이 문자열을 지운다.
    // 같은 메시지를 다시 처리해도 질문은 이미 닫혀 있어 무시되므로 지워진 값을 보는 일은 없다.
    struct answer_input_intent
    {
        operation_id id { 0 };
        std::uint64_t prompt_id { 0 };
        mutable std::u8string answer {};
    };

    // 인증 대화 상자를 답 없이 닫는다. 그 operation은 `auth_cancelled`로 끝난다.
    struct cancel_input_intent
    {
        operation_id id { 0 };
        std::uint64_t prompt_id { 0 };
    };
} // namespace cyella
